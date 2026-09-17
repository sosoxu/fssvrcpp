// =============================================================================
//  阶段 7 切片 3：扩展 RPC 的字节通道（C7.6 / C7.10）
// =============================================================================
//  覆盖
//    · C7.6：1 GiB 上传/下载（RSS < 64 MiB）+ 0 字节对象 + **中途取消**
//            （不遗留 `.tmp.*`、不泄漏句柄、对象不会被留成"半个"）
//    · C7.10：gRPC 区间读与 HTTP `Range` 在**相同 offset/length** 下字节完全一致，
//            且都**不整文件读取**（在 `IBlobStore::get` 边界上直接断言区间）
//    · 另加：`ServerSideCopy`（含路径安全边界）、`registerMetadata=true` 复用 12 步用例
//
//  ★ 全部跑在**真实 POSIX 存储 + 真实 gRPC/REST 端口**上（`PosixDualProtocolFixture`）：
//    内存适配器会把对象整体放进 RAM，用它测"流式"等于没测（P6-D13 的教训）。
// =============================================================================
#include <catch2/catch.hpp>

#include "big_file.h"
#include "grpc_fixture.h"
#include "raw_http.h"

#include "adapters/grpc/dto/grpc_dto.h"
#include "app/services/object_key_policy.h"

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using fss::test::Authed;
using fss::test::BigFileBytes;
using fss::test::HttpDo;
using fss::test::PosixDualProtocolFixture;
using fss::test::RepeatingPatternDigest;
using fss::test::RepeatingPatternDigestAt;
using fss::test::RssLimitKib;
using fss::test::TargetOf;
using osdu::file::v1::FileService;

//  ---- 经 gRPC 取一个上传位置（staging），返回 file_source/file_id ----
struct IssuedLocation {
  std::string file_id;
  std::string file_source;
};

IssuedLocation RpcIssueUpload(PosixDualProtocolFixture& fx,
                             const std::string& requested_file_id = "") {
  auto context = fx.Context();
  osdu::file::v1::GetUploadLocationRequest request;
  if (!requested_file_id.empty()) request.set_file_id(requested_file_id);
  osdu::file::v1::LocationResponse response;
  const auto status = fx.stub->GetUploadLocation(context.get(), request, &response);
  REQUIRE(status.ok());
  IssuedLocation out;
  out.file_id = response.file_id();
  out.file_source = response.location().file_source();
  REQUIRE_FALSE(out.file_source.empty());
  return out;
}

//  ---- 客户端流上传：从 `RepeatingSource` 分片发送（不整块驻留）----
struct UploadOutcome {
  bool ok = false;
  int code = 0;
  std::string message;
  osdu::file::v1::UploadFileResponse response;
};

UploadOutcome RpcUploadPattern(PosixDualProtocolFixture& fx, const std::string& file_source,
                               std::int64_t total, std::int64_t chunk_bytes = 256 * 1024) {
  UploadOutcome out;
  auto context = fx.Context();
  auto writer = fx.stub->UploadFile(context.get(), &out.response);
  osdu::file::v1::UploadFileRequest info;
  info.mutable_info()->set_file_source(file_source);
  if (!writer->Write(info)) return out;

  fss::bytes::RepeatingSource source(total);
  std::vector<char> buffer(static_cast<std::size_t>(chunk_bytes));
  while (true) {
    const auto read = source.Read(buffer.data(), buffer.size());
    REQUIRE(read.ok());
    if (read.value() == 0) break;
    osdu::file::v1::UploadFileRequest chunk;
    chunk.set_chunk(buffer.data(), read.value());
    if (!writer->Write(chunk)) return out;
  }
  //  ★ 协议要求：数据分片之后必须发显式结束标记，否则服务端视为"流不完整"（P8-D05）
  osdu::file::v1::UploadFileRequest end;
  end.set_end_of_stream(true);
  if (!writer->Write(end)) return out;
  writer->WritesDone();
  const auto status = writer->Finish();
  out.ok = status.ok();
  out.code = static_cast<int>(status.error_code());
  out.message = status.error_message();
  return out;
}

//  ---- 客户端流上传：一次性载荷（小对象用例用；大对象用上面的分片生成器）----
UploadOutcome RpcUploadPayload(PosixDualProtocolFixture& fx, const std::string& file_source,
                               const std::string& payload,
                               const osdu::file::v1::FileMetadataRecord* metadata = nullptr) {
  UploadOutcome out;
  auto context = fx.Context();
  auto writer = fx.stub->UploadFile(context.get(), &out.response);
  osdu::file::v1::UploadFileRequest info;
  info.mutable_info()->set_file_source(file_source);
  if (metadata != nullptr) {
    info.mutable_info()->set_register_metadata(true);
    *info.mutable_info()->mutable_metadata() = *metadata;
  }
  if (!writer->Write(info)) return out;
  if (!payload.empty()) {
    osdu::file::v1::UploadFileRequest chunk;
    chunk.set_chunk(payload);
    if (!writer->Write(chunk)) return out;
  }
  osdu::file::v1::UploadFileRequest end;
  end.set_end_of_stream(true);
  if (!writer->Write(end)) return out;
  writer->WritesDone();
  const auto status = writer->Finish();
  out.ok = status.ok();
  out.code = static_cast<int>(status.error_code());
  out.message = status.error_message();
  return out;
}

//  ---- 服务端流下载：边收边算 SHA-256（不整块驻留），返回摘要/字节数/尾块 ----
struct DownloadOutcome {
  bool ok = false;
  std::uint64_t bytes = 0;
  std::uint64_t chunks = 0;
  std::string checksum;
  std::uint64_t total_size = 0;
};

DownloadOutcome RpcDownload(PosixDualProtocolFixture& fx, const std::string& file_id,
                            std::uint64_t offset = 0, std::uint64_t length = 0) {
  DownloadOutcome out;
  auto context = fx.Context();
  osdu::file::v1::DownloadFileRequest request;
  request.set_file_id(file_id);
  request.set_offset(offset);
  request.set_length(length);
  auto reader = fx.stub->DownloadFile(context.get(), request);
  fss::crypto::Hasher hasher(fss::crypto::ChecksumAlgorithm::kSha256);
  osdu::file::v1::DownloadFileResponse message;
  while (reader->Read(&message)) {
    if (message.chunk().empty()) {
      //  尾块：只有 totalSize/checksum
      out.total_size = message.total_size();
      out.checksum = message.checksum();
      continue;
    }
    ++out.chunks;
    out.bytes += message.chunk().size();
    hasher.Update(message.chunk());
  }
  out.ok = reader->Finish().ok();
  if (out.ok) out.checksum = hasher.HexDigest();  // 用**客户端**算的摘要做比对基准
  return out;
}

//  ---- 统计存储层被要求读了多少：用于证明"区间读没有退化成整文件读" ----
//  这是一个纯粹的测试替身（L2 端口装饰器），只转发 + 记账。
class RecordingGetStore final : public fss::domain::IBlobStore {
 public:
  explicit RecordingGetStore(fss::domain::IBlobStore& inner) : inner_(inner) {}

  fss::domain::ByteRange last_range;
  std::uint64_t range_reads = 0;
  std::uint64_t whole_reads = 0;

  fss::domain::BlobCapabilities capabilities() const override { return inner_.capabilities(); }
  fss::Result<void> ensure_container(const std::string& container) override {
    return inner_.ensure_container(container);
  }
  fss::Result<fss::domain::SignedLocation> presign_put(
      const fss::domain::ObjectRef& ref, const fss::domain::PresignOptions& options) override {
    return inner_.presign_put(ref, options);
  }
  fss::Result<fss::domain::SignedLocation> presign_get(
      const fss::domain::ObjectRef& ref, const fss::domain::PresignOptions& options) override {
    return inner_.presign_get(ref, options);
  }
  fss::Result<void> put(const fss::domain::ObjectRef& ref, fss::bytes::ByteSource& source,
                        const fss::domain::PutOptions& options) override {
    return inner_.put(ref, source, options);
  }
  fss::Result<void> get(const fss::domain::ObjectRef& ref, fss::bytes::ByteSink& sink,
                        const fss::domain::ByteRange& range) override {
    last_range = range;
    if (range.IsWholeObject()) {
      ++whole_reads;
    } else {
      ++range_reads;
    }
    return inner_.get(ref, sink, range);
  }
  fss::Result<fss::domain::ObjectStat> stat(const fss::domain::ObjectRef& ref) override {
    return inner_.stat(ref);
  }
  fss::Result<void> remove(const fss::domain::ObjectRef& ref) override {
    return inner_.remove(ref);
  }
  fss::Result<fss::domain::ObjectStat> copy(const fss::domain::ObjectRef& from,
                                            const fss::domain::ObjectRef& to) override {
    return inner_.copy(from, to);
  }
  fss::Result<fss::domain::ListPage> list(const std::string& container, const std::string& prefix,
                                          const std::string& continuation_token,
                                          int limit) override {
    return inner_.list(container, prefix, continuation_token, limit);
  }

  //  C9.25：转发
  fss::Result<fss::domain::TempSweepResult> remove_temp_files(const std::string& container,
                                                 std::int64_t older_than_epoch_seconds,
                                                 bool dry_run) override {
    return inner_.remove_temp_files(container, older_than_epoch_seconds, dry_run);
  }

 private:
  fss::domain::IBlobStore& inner_;
};

//  递归找出含 `.tmp.` 的文件名（POSIX 驱动的临时文件标记）
std::vector<std::string> TempResidue(const std::string& root) {
  std::vector<std::string> found;
  std::error_code error;
  if (!std::filesystem::exists(root, error)) return found;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root, error)) {
    if (entry.is_regular_file(error) &&
        entry.path().filename().string().find(".tmp.") != std::string::npos) {
      found.push_back(entry.path().string());
    }
  }
  return found;
}

std::size_t OpenFdCount() {
  std::error_code error;
  std::size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd", error)) {
    (void)entry;
    ++count;
  }
  return count;
}

}  // namespace

// =============================================================================
//  C7.6：1 GiB 上传 + 下载（真实磁盘 + 绝对 RSS 上限 + 独立 SHA-256 + 对照）
// =============================================================================
TEST_CASE("★ C7.6 1 GiB gRPC 流式上传/下载：RSS 上限 + 独立 SHA-256 + 整块对照",
          "[phase7][integration][c7.6]") {
  PosixDualProtocolFixture fx;
  const std::int64_t total = BigFileBytes();
  INFO("对象大小 " << total << " 字节，RSS 上限 " << RssLimitKib() << " KiB");

  const auto issued = RpcIssueUpload(fx);

  //  ---- 上传：客户端边生成边发，服务端边收边落盘 ----
  const std::uint64_t up_baseline = fss::test::CurrentRssKib();
  const auto uploaded = RpcUploadPattern(fx, issued.file_source, total);
  const std::uint64_t up_after = fss::test::CurrentRssKib();
  INFO("上传状态 " << uploaded.code << " " << uploaded.message);
  INFO("上传后 totalSize=" << uploaded.response.bytes_written()
                           << " checksum=" << uploaded.response.checksum());
  REQUIRE(uploaded.ok);
  REQUIRE(uploaded.response.bytes_written() == static_cast<std::uint64_t>(total));
  const std::uint64_t up_growth = up_after > up_baseline ? up_after - up_baseline : 0;
  INFO("上传前后 RSS 增长 " << up_growth << " KiB（对象 " << total / 1024 << " KiB）");
  REQUIRE(up_growth < RssLimitKib());

  //  期望值由测试**独立流式算出**（R8：不抄实现里的常量）
  const std::string expected = RepeatingPatternDigest(total);
  REQUIRE(uploaded.response.checksum() == expected);

  //  ---- 下载：客户端边收边算，服务端按区间流式读出 ----
  const std::uint64_t down_baseline = fss::test::CurrentRssKib();
  const auto downloaded = RpcDownload(fx, issued.file_id);
  const std::uint64_t down_after = fss::test::CurrentRssKib();
  const std::uint64_t down_growth = down_after > down_baseline ? down_after - down_baseline : 0;
  INFO("下载后 bytes=" << downloaded.bytes << " chunks=" << downloaded.chunks
                       << " RSS 增长 " << down_growth << " KiB");
  REQUIRE(downloaded.ok);
  REQUIRE(downloaded.bytes == static_cast<std::uint64_t>(total));
  REQUIRE(downloaded.checksum == expected);  // 上传/下载内容一致（C7.6 的"CRC/SHA 一致"）
  REQUIRE(downloaded.total_size == static_cast<std::uint64_t>(total));
  REQUIRE(down_growth < RssLimitKib());

  //  ---- 自证对照（R1）：同一测量必须能发现"整块驻留" ----
  //  若去掉这一段，"增长 < 64 MiB"无法区分"实现流式"与"测量没生效"。
  const std::uint64_t control_baseline = fss::test::CurrentRssKib();
  fss::bytes::StringSink whole;
  fss::domain::ByteRange whole_range;
  const auto staging_container = fss::app::ObjectKeyPolicy::ContainerFor(
      "opendes", fss::domain::StorageZone::kStaging);
  REQUIRE(staging_container.ok());
  const auto parsed = fss::app::ObjectKeyPolicy::ParseFileSource(issued.file_source);
  REQUIRE(parsed.ok());
  fss::domain::ObjectRef ref;
  ref.container = staging_container.value();
  ref.key = fss::app::ObjectKeyPolicy::MakePosixKey(parsed.value());
  REQUIRE(fx.stack.blob.get(ref, whole, whole_range).ok());
  const std::uint64_t control_after = fss::test::CurrentRssKib();
  const std::uint64_t control_growth =
      control_after > control_baseline ? control_after - control_baseline : 0;
  INFO("对照（整块读回）RSS 增长 " << control_growth << " KiB");
  REQUIRE(whole.bytes_written() == static_cast<std::size_t>(total));
  REQUIRE(control_growth >= static_cast<std::uint64_t>(total) / 2 / 1024);
}

// =============================================================================
//  C7.6：0 字节对象（上传/下载都成功，且尾块明确给出 totalSize = 0）
// =============================================================================
TEST_CASE("★ C7.6 0 字节对象：上传/下载成功、尾块 totalSize=0、校验和是空串的 SHA-256",
          "[phase7][integration][c7.6]") {
  PosixDualProtocolFixture fx;
  const auto issued = RpcIssueUpload(fx);

  const auto uploaded = RpcUploadPattern(fx, issued.file_source, 0);
  REQUIRE(uploaded.ok);
  REQUIRE(uploaded.response.bytes_written() == 0);
  REQUIRE(uploaded.response.checksum() == fss::crypto::Sha256Hex(""));

  const auto downloaded = RpcDownload(fx, issued.file_id);
  REQUIRE(downloaded.ok);
  REQUIRE(downloaded.bytes == 0);
  REQUIRE(downloaded.chunks == 0);
  REQUIRE(downloaded.total_size == 0);
}

// =============================================================================
//  C7.6：中途取消 —— 不留临时文件、不泄漏句柄、不产生"半个对象"
// =============================================================================
TEST_CASE("★ C7.6 上传中途取消：无 .tmp. 残留、fd 不增长、对象仍是签发时的空对象",
          "[phase7][integration][c7.6]") {
  PosixDualProtocolFixture fx;
  const auto issued = RpcIssueUpload(fx);

  const std::size_t fd_before = OpenFdCount();
  {
    auto context = fx.Context();
    osdu::file::v1::UploadFileResponse response;
    auto writer = fx.stub->UploadFile(context.get(), &response);
    osdu::file::v1::UploadFileRequest info;
    info.mutable_info()->set_file_source(issued.file_source);
    REQUIRE(writer->Write(info));
    osdu::file::v1::UploadFileRequest chunk;
    chunk.set_chunk(std::string(256 * 1024, 'x'));
    for (int i = 0; i < 16; ++i) {
      REQUIRE(writer->Write(chunk));  // 4 MiB 之后取消
    }
    //  ★ 取消之后**不要**再 `WritesDone()`：半关闭会被服务端看成"正常结束"，
    //    这正是"截断对象被提交"的另一种形态（真实中断 = 直接放弃流）。
    context->TryCancel();
    const auto status = writer->Finish();
    INFO("取消后的状态：" << status.error_code() << " " << status.error_message());
    REQUIRE_FALSE(status.ok());
  }

  //  ① 临时文件必须被清掉（POSIX 驱动在失败路径 unlink；`.tmp.` 是它的标记）。
  //  ★ 必须**轮询实际条件**而不是立刻断言：客户端 `Finish()` 返回（CANCELLED）时，
  //    服务端线程可能还在 `put` 里；取消信号要等它下一次 `Read()` 才被看见。
  //    固定 sleep 会既慢又不稳（AGENTS.md §4.3）。
  std::vector<std::string> residue;
  for (int attempt = 0; attempt < 100; ++attempt) {
    residue = TempResidue(fx.blob_root());
    if (residue.empty()) break;
    ::usleep(50 * 1000);
  }
  std::string residue_list;
  for (const auto& path : residue) residue_list += path + "\n";
  CAPTURE(residue_list);
  REQUIRE(residue.empty());

  //  ② 句柄不泄漏（取消发生在 4 MiB 之后，此时临时文件已打开过）
  const std::size_t fd_after = OpenFdCount();
  INFO("fd 数量：取消前 " << fd_before << "，取消后 " << fd_after);
  REQUIRE(fd_after <= fd_before + 2);  // gRPC 自身的短连接抖动容忍 2 个

  //  ③ 对象仍然是**签发时**的 0 字节空对象（不是被截断的半个 4 MiB）
  const auto downloaded = RpcDownload(fx, issued.file_id);
  REQUIRE(downloaded.ok);
  REQUIRE(downloaded.bytes == 0);
  REQUIRE(downloaded.total_size == 0);
}

// =============================================================================
//  C7.10：gRPC 区间读 == HTTP Range（同 offset/length，SHA-256 逐字节一致）
// =============================================================================
TEST_CASE("★ C7.10 gRPC 区间读与 HTTP Range 字节一致，且都走区间读（不整文件读）",
          "[phase7][integration][c7.10]") {
  PosixDualProtocolFixture fx;
  //  对象大小：默认 128 MiB（区间读只看一小段）；sanitizer 构建下随 FSS_TEST_BIG_BYTES 缩小
  const std::int64_t total = std::min<std::int64_t>(BigFileBytes(), 128LL * 1024 * 1024);
  const std::int64_t offset = total / 2;
  const std::int64_t length = 1024 * 1024;  // 1 MiB
  INFO("对象 " << total << " 字节，区间 [" << offset << ", +" << length << ")");

  const auto issued = RpcIssueUpload(fx);
  const auto uploaded = RpcUploadPattern(fx, issued.file_source, total);
  REQUIRE(uploaded.ok);

  //  用记账装饰器包住存储：两条链路都必须以**区间**形式访问存储
  RecordingGetStore recording(fx.stack.blob);
  auto& mutable_factory = fx.stack.factory;
  mutable_factory.SetZoneStore(recording);

  //  ---- ① gRPC 区间读 ----
  const auto rpc = RpcDownload(fx, issued.file_id, static_cast<std::uint64_t>(offset),
                               static_cast<std::uint64_t>(length));
  REQUIRE(rpc.ok);
  REQUIRE(rpc.bytes == static_cast<std::uint64_t>(length));
  REQUIRE(rpc.total_size == static_cast<std::uint64_t>(total));
  REQUIRE(recording.last_range.offset == static_cast<std::uint64_t>(offset));
  REQUIRE(recording.last_range.length == static_cast<std::uint64_t>(length));
  REQUIRE(recording.whole_reads == 0);

  //  ---- ② HTTP Range（同一份位置记录 → 自签下载 URL）----
  const auto url_response =
      HttpDo(fx.http_port(), "GET", "/api/file/v2/files/" + issued.file_id + "/downloadURL",
             Authed());
  REQUIRE(url_response.status == 200);
  const auto url_json = fss::json::ParseObject(url_response.body);
  REQUIRE(url_json.ok());
  const std::string signed_url = url_json.value()["SignedUrl"].get<std::string>();
  const std::string range_header =
      "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + length - 1);
  auto http_headers = Authed();
  http_headers.push_back("Range: " + range_header);
  const auto http = HttpDo(fx.http_port(), "GET", TargetOf(signed_url), http_headers);
  INFO("HTTP Range 状态：" << http.status << " 长度 " << http.body.size());
  REQUIRE(http.status == 206);
  REQUIRE(http.body.size() == static_cast<std::size_t>(length));

  //  ---- ③ 逐字节等价：两条链路对同一区间给出同一 SHA-256 ----
  const std::string expected = RepeatingPatternDigestAt(offset, length);
  REQUIRE(fss::crypto::Sha256Hex(http.body) == expected);
  REQUIRE(rpc.checksum == expected);
  REQUIRE(recording.range_reads >= 2);       // gRPC 与 HTTP 各一次区间读
  REQUIRE(recording.whole_reads == 0);       // 都没有退化成整文件读
}

// =============================================================================
//  ServerSideCopy（扩展：纯字节复制，不动记录）
// =============================================================================
TEST_CASE("★ C7.1 ServerSideCopy：字节复制到目标 zone，且不改动任何记录",
          "[phase7][integration][c7.1]") {
  PosixDualProtocolFixture fx;
  const std::string payload(64 * 1024, 'z');
  const auto issued = RpcIssueUpload(fx);
  //  用一段确定的载荷（0 字节对象不足以证明"复制了内容"）
  const auto uploaded = RpcUploadPayload(fx, issued.file_source, payload);
  REQUIRE(uploaded.ok);

  const std::string target_source = "/osdu-user/1700000000000-2023-11-14-22-13-20-000/target-1";
  auto context = fx.Context();
  osdu::file::v1::ServerSideCopyRequest request;
  request.set_source_file_source(issued.file_source);
  request.set_target_file_source(target_source);
  request.set_target_zone(osdu::file::v1::STORAGE_ZONE_PERSISTENT);
  osdu::file::v1::ServerSideCopyResponse response;
  const auto status = fx.stub->ServerSideCopy(context.get(), request, &response);
  INFO("ServerSideCopy → " << status.error_code() << " " << status.error_message());
  REQUIRE(status.ok());
  REQUIRE(response.file_source() == target_source);
  REQUIRE(response.bytes_copied() == payload.size());

  //  目标对象内容必须逐字节相同（直接读存储层的目标 ref）
  const auto container =
      fss::app::ObjectKeyPolicy::ContainerFor("opendes", fss::domain::StorageZone::kPersistent);
  REQUIRE(container.ok());
  const auto parts = fss::app::ObjectKeyPolicy::ParseFileSource(target_source);
  REQUIRE(parts.ok());
  fss::domain::ObjectRef target_ref;
  target_ref.container = container.value();
  target_ref.key = fss::app::ObjectKeyPolicy::MakePosixKey(parts.value());
  fss::bytes::StringSink sink;
  REQUIRE(fx.stack.blob.get(target_ref, sink, fss::domain::ByteRange{}).ok());
  REQUIRE(sink.str() == payload);

  //  副作用为零：位置记录仍是 staging，且没有新增元数据记录
  const auto location = fx.stack.locations->Find("opendes", issued.file_id);
  REQUIRE(location.ok());
  REQUIRE(location.value().zone == fss::domain::StorageZone::kStaging);
  const auto latest =
      fx.stack.metadata.GetLatestByFileSource("opendes", issued.file_source);
  REQUIRE_FALSE(latest.ok());  // kNotFound：没有登记过元数据

  //  ---- 错误路径 ----
  {
    auto bad_context = fx.Context();
    osdu::file::v1::ServerSideCopyRequest bad;
    bad.set_source_file_source("/no/such/source");
    bad.set_target_file_source(target_source);
    osdu::file::v1::ServerSideCopyResponse bad_response;
    const auto bad_status = fx.stub->ServerSideCopy(bad_context.get(), bad, &bad_response);
    REQUIRE(bad_status.error_code() == ::grpc::StatusCode::NOT_FOUND);
  }
  {
    auto bad_context = fx.Context();
    osdu::file::v1::ServerSideCopyRequest bad;
    bad.set_source_file_source(issued.file_source);
    bad.set_target_file_source("/osdu-user/../etc/passwd");  // 路径穿越 → 安全边界必须拒绝
    bad.set_target_zone(osdu::file::v1::STORAGE_ZONE_PERSISTENT);
    osdu::file::v1::ServerSideCopyResponse bad_response;
    const auto bad_status = fx.stub->ServerSideCopy(bad_context.get(), bad, &bad_response);
    INFO("非法目标 → " << bad_status.error_code() << " " << bad_status.error_message());
    REQUIRE(bad_status.error_code() == ::grpc::StatusCode::INVALID_ARGUMENT);
  }
}

// =============================================================================
//  UploadFile + registerMetadata=true：一次 RPC 走完"上传 + 登记"（复用 12 步用例）
// =============================================================================
TEST_CASE("★ C7.1 UploadFile(registerMetadata=true)：登记元数据、zone 迁到 persistent、校验和一致",
          "[phase7][integration][c7.1]") {
  PosixDualProtocolFixture fx;
  const std::string payload = "register-metadata-via-grpc";
  const auto issued = RpcIssueUpload(fx);

  auto record = fss::test::AppFixture::MakeRecord(issued.file_source, "via-grpc.bin");
  osdu::file::v1::FileMetadataRecord metadata_proto;
  fss::adapters::grpc::FillMetadataProto(record, &metadata_proto);

  const auto uploaded = RpcUploadPayload(fx, issued.file_source, payload, &metadata_proto);
  INFO("UploadFile(register) → " << uploaded.code << " " << uploaded.message);
  REQUIRE(uploaded.ok);
  REQUIRE_FALSE(uploaded.response.metadata_record_id().empty());
  REQUIRE(uploaded.response.checksum() == fss::crypto::Sha256Hex(payload));

  //  ① 元数据记录已写、且校验和是服务端算出的 SHA-256
  auto read_context = fx.Context();
  osdu::file::v1::GetFileMetadataRequest read_request;
  read_request.set_id(uploaded.response.metadata_record_id());
  osdu::file::v1::FileMetadataRecord stored;
  REQUIRE(fx.stub->GetFileMetadata(read_context.get(), read_request, &stored).ok());
  REQUIRE(stored.id() == uploaded.response.metadata_record_id());
  REQUIRE(stored.version() == 1);
  REQUIRE(stored.data().checksum() == fss::crypto::Sha256Hex(payload));

  //  ② 位置记录已迁到 persistent（第 10 步的副作用）
  const auto location = fx.stack.locations->Find("opendes", issued.file_id);
  REQUIRE(location.ok());
  REQUIRE(location.value().zone == fss::domain::StorageZone::kPersistent);
}

// =============================================================================
//  UploadFile 的授权锚点：只能写到**已签发**的位置记录上（不接受坐标覆盖）
// =============================================================================
//  没有这条约束，任何能调 `UploadFile` 的调用方都能往任意对象键写字节 ——
//  这是与 `/v1/transfer` 内核"对象键只来自 token 载荷"同一条防线。
TEST_CASE("★ C7.1 UploadFile 拒绝未签发的 file_source 与坐标覆盖",
          "[phase7][integration][c7.1]") {
  PosixDualProtocolFixture fx;

  //  ① 从未签发过的 FileSource → NOT_FOUND（不是"创建一条新记录"）
  {
    auto context = fx.Context();
    osdu::file::v1::UploadFileResponse response;
    auto writer = fx.stub->UploadFile(context.get(), &response);
    osdu::file::v1::UploadFileRequest info;
    info.mutable_info()->set_file_source("/opendes-user/1700000000000-2023-11-14-22-13-20-000/forged");
    REQUIRE(writer->Write(info));
    osdu::file::v1::UploadFileRequest chunk;
    chunk.set_chunk("forged");
    REQUIRE(writer->Write(chunk));
    writer->WritesDone();
    const auto status = writer->Finish();
    INFO("未签发 file_source → " << status.error_code() << " " << status.error_message());
    REQUIRE(status.error_code() == ::grpc::StatusCode::NOT_FOUND);
  }

  //  ② 显式 container/key 与已签发的记录不一致 → PERMISSION_DENIED（不接受覆盖）
  {
    const auto issued = RpcIssueUpload(fx);
    auto context = fx.Context();
    osdu::file::v1::UploadFileResponse response;
    auto writer = fx.stub->UploadFile(context.get(), &response);
    osdu::file::v1::UploadFileRequest info;
    info.mutable_info()->set_file_source(issued.file_source);
    info.mutable_info()->set_container("opendes-persistent");  // 想让字节写到别的容器
    REQUIRE(writer->Write(info));
    osdu::file::v1::UploadFileRequest chunk;
    chunk.set_chunk("hijack");
    REQUIRE(writer->Write(chunk));
    writer->WritesDone();
    const auto status = writer->Finish();
    INFO("坐标覆盖 → " << status.error_code() << " " << status.error_message());
    REQUIRE(status.error_code() == ::grpc::StatusCode::PERMISSION_DENIED);
  }

  //  ③ 下载：offset 越界 → INVALID_ARGUMENT（与 HTTP Range 的"不可满足"同类）
  {
    const auto issued = RpcIssueUpload(fx);
    const auto uploaded = RpcUploadPayload(fx, issued.file_source, "0123456789");
    REQUIRE(uploaded.ok);
    const auto beyond = RpcDownload(fx, issued.file_id, /*offset=*/4096, /*length=*/0);
    REQUIRE_FALSE(beyond.ok);
  }
}

// =============================================================================
//  C7.6：**慢客户端**（分片之间停顿）也必须成功 —— 流式通道没有"整体超时"
// =============================================================================
//  与 HTTP 数据面的结论一致（C4.11：`/v1/transfer` 没有整体超时，只有空闲超时）。
//  这里发 16 片、每片之间停 25 ms（总计约 0.4 s）：若 gRPC 侧被套上"整体超时"，
//  这个用例会失败 —— 它是那个不变量在 RPC 面上的回归。
TEST_CASE("★ C7.6 慢客户端（分片间停顿）上传仍然成功", "[phase7][integration][c7.6]") {
  PosixDualProtocolFixture fx;
  const auto issued = RpcIssueUpload(fx);

  constexpr int kChunks = 16;
  const std::string piece(64 * 1024, 's');
  auto context = fx.Context();
  osdu::file::v1::UploadFileResponse response;
  auto writer = fx.stub->UploadFile(context.get(), &response);
  osdu::file::v1::UploadFileRequest info;
  info.mutable_info()->set_file_source(issued.file_source);
  REQUIRE(writer->Write(info));
  for (int i = 0; i < kChunks; ++i) {
    osdu::file::v1::UploadFileRequest chunk;
    chunk.set_chunk(piece);
    REQUIRE(writer->Write(chunk));
    ::usleep(25 * 1000);  // 慢客户端：让服务端在两次读之间空闲
  }
  osdu::file::v1::UploadFileRequest end;
  end.set_end_of_stream(true);
  REQUIRE(writer->Write(end));
  writer->WritesDone();
  const auto status = writer->Finish();
  INFO("慢客户端上传 → " << status.error_code() << " " << status.error_message());
  REQUIRE(status.ok());
  REQUIRE(response.bytes_written() ==
          static_cast<std::uint64_t>(kChunks) * piece.size());

  //  内容必须与"快客户端"完全一致（同样的分片、同样的顺序）
  std::string expected;
  for (int i = 0; i < kChunks; ++i) expected += piece;
  REQUIRE(response.checksum() == fss::crypto::Sha256Hex(expected));
  const auto downloaded = RpcDownload(fx, issued.file_id);
  REQUIRE(downloaded.ok);
  REQUIRE(downloaded.checksum == fss::crypto::Sha256Hex(expected));
}

// =============================================================================
//  P8-D05：**没有结束标记的流**必须被拒绝，且对象不能被"截断提交"
// =============================================================================
//  这是 P7-D07 的确定性版本：不依赖"取消与 EOF 的竞态"，而是直接违反协议 ——
//  客户端发完数据分片就 `WritesDone`（不发 `end_of_stream`）。
//  在 ASan 构建下，"取消当 EOF"的竞态曾让 512 KiB 的截断对象被 commit（P8-D05）；
//  有了显式结束标记，这个场景变成**确定性**拒绝。
TEST_CASE("★ P8-D05 缺 end_of_stream 的上传流：报错且对象保持为空",
          "[phase7][integration][c7.6]") {
  PosixDualProtocolFixture fx;
  const auto issued = RpcIssueUpload(fx);

  auto context = fx.Context();
  osdu::file::v1::UploadFileResponse response;
  auto writer = fx.stub->UploadFile(context.get(), &response);
  osdu::file::v1::UploadFileRequest info;
  info.mutable_info()->set_file_source(issued.file_source);
  REQUIRE(writer->Write(info));
  osdu::file::v1::UploadFileRequest chunk;
  chunk.set_chunk(std::string(256 * 1024, 'x'));
  REQUIRE(writer->Write(chunk));
  REQUIRE(writer->Write(chunk));
  writer->WritesDone();  // ★ 故意不发 end_of_stream
  const auto status = writer->Finish();
  INFO("缺结束标记 → " << status.error_code() << " " << status.error_message());
  REQUIRE_FALSE(status.ok());

  //  对象必须仍是签发时的 0 字节空对象（**不是**被提交的 512 KiB）
  const auto downloaded = RpcDownload(fx, issued.file_id);
  REQUIRE(downloaded.ok);
  REQUIRE(downloaded.bytes == 0);
  REQUIRE(downloaded.total_size == 0);

  //  失败路径同样不能留下临时文件
  std::vector<std::string> residue;
  for (int attempt = 0; attempt < 100; ++attempt) {
    residue = TempResidue(fx.blob_root());
    if (residue.empty()) break;
    ::usleep(50 * 1000);
  }
  REQUIRE(residue.empty());
}
