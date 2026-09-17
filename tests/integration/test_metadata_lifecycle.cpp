// =============================================================================
//  C6.4：校验和 —— 服务端计算并**覆写**（含"算法跟随驱动"与流式回算）
// =============================================================================
//  契约 §2.6 第 7 步 / §3.4：`checksum = storageUtil.getChecksum(persistentLocation)`，
//  非空则**回写覆盖** `FileSourceInfo.Checksum` + `ChecksumAlgorithm`。
//
//  ★★ 客户端传入的 `Checksum`/`ChecksumAlgorithm` 是**被覆写的输入**，不是"待校验的断言"。
//     上游证据 ①（`docs/01-osdu-research.md` §2.3）：「校验和：**服务端覆写**客户端传入的
//     `Checksum`/`ChecksumAlgorithm`（至少 Azure 实现如此）」，且第 7 条的失败语义是 `—`（非致命）。
//     上游证据 ②（vendored 样例 `tests/conformance/fixtures/upstream/File_CorrectPayload.json`）：
//     客户端给的是 `MD5("") = d41d8cd9…` 却声明 `ChecksumAlgorithm = "SHA-256"`，**期望响应 201**。
//     任何"比对不符就拒绝"的实现都会在这条权威样例上返回 400 —— 那会直接破坏 G1（OSDU 兼容）。
//     ⇒ 本项目计划里曾写的"客户端提供但不符 → 400 + 删除对象"是**没有上游依据的臆断**，
//       已在 `docs/00-final-design.md` §5 记录为被推翻的结论（P6-D05）。
//
//  ★ 三条仍然成立的硬要求（本文件都有断言）：
//     ① 校验和必须**流式**计算（不能把对象读进内存）—— 见末尾的 RSS 用例 + 整块读回自证对照；
//     ② 算法必须**跟随驱动**：Azure 风格的驱动给 MD5，记录里就得写 `MD5`（C6.4 的"算法覆盖"）；
//        驱动给的原生值若不是合法 hex（如 `ETAG`），必须**回退到流式回算 SHA-256**，
//        绝不能把 `ETAG` 当校验和写进记录（那是"看起来有值"的假象）；
//     ③ 覆写必须同时落到**两处**（`data.Checksum/ChecksumAlgorithm` 与 `FileSourceInfo.*`）。
// =============================================================================
#include <catch2/catch.hpp>

#include "app_fixture.h"
#include "big_file.h"       // BigFileBytes / RssLimitKib（1 GiB 规模可被构建方式覆盖）
#include "http_fixture.h"   // HttpFixture / HttpDo / Authed（端到端 correlation-id 用例）
#include "raw_http.h"
#include "temp_dir.h"

#include "app/services/location_issuer.h"
#include "app/services/object_key_policy.h"
#include "app/usecases/usecases.h"
#include "common/bytes/bytes.h"
#include "common/crypto/crypto.h"
#include "infra/blob/posix/posix_blob_store.h"
#include "infra/location/memory/memory_location_repository.h"
#include "infra/metadata/memory/memory_metadata_repository.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using fss::app::CreateFileMetadata;
using fss::app::GetFileMetadata;
using fss::app::GetUploadLocation;
using fss::app::ObjectRefFromLocation;
using fss::domain::ObjectRef;
using fss::domain::StorageZone;
using fss::test::AppFixture;

namespace {

//  上传一份内容到 staging，返回 (FileSource, 对象引用)
struct Uploaded {
  std::string file_source;
  ObjectRef staging_ref;
};

//  与 fixture 的成员名解耦：内存装配与 POSIX 装配都能用同一份上传辅助
template <typename Fixture>
Uploaded UploadSource(Fixture& fx, fss::bytes::ByteSource& source) {
  GetUploadLocation upload(*fx.ports);
  const auto up = upload.Execute(fx.caller, std::nullopt, "1H");
  REQUIRE(up.ok());
  const auto location = fx.locations.Find(fx.caller.partition, up.value().file_id);
  REQUIRE(location.ok());
  const auto ref = ObjectRefFromLocation(location.value());
  REQUIRE(ref.ok());
  const auto put = fx.blob.put(ref.value(), source, fss::domain::PutOptions{});
  INFO("put 失败：" << (put.ok() ? std::string() : put.error().ToString()));
  REQUIRE(put.ok());
  return Uploaded{up.value().file_source, ref.value()};
}

Uploaded UploadWithContent(AppFixture& fx, const std::string& content) {
  fss::bytes::StringSource source(content);
  return UploadSource(fx, source);
}

//  persistent 目标引用（与用例内部算法一致：容器按 zone 派生、key 不变）
ObjectRef PersistentRef(const AppFixture& fx, const ObjectRef& staging_ref) {
  const auto container = fss::app::ObjectKeyPolicy::ContainerFor(
      fx.caller.partition, StorageZone::kPersistent);
  REQUIRE(container.ok());
  ObjectRef ref;
  ref.container = container.value();
  ref.key = staging_ref.key;
  return ref;
}

//  取回记录（一并断言两处 Checksum/ChecksumAlgorithm 一致）
template <typename Fixture>
fss::Result<fss::domain::FileMetadataRecord> Fetch(Fixture& fx, const std::string& id) {
  GetFileMetadata get(*fx.ports);
  const auto stored = get.Execute(fx.caller, id);
  if (!stored.ok()) return stored.error();
  const auto& info = stored.value().data.dataset_properties.file_source_info;
  REQUIRE(stored.value().data.checksum.has_value());
  REQUIRE(info.checksum.has_value());
  REQUIRE(*stored.value().data.checksum == *info.checksum);
  REQUIRE(stored.value().data.checksum_algorithm.has_value());
  REQUIRE(info.checksum_algorithm.has_value());
  REQUIRE(*stored.value().data.checksum_algorithm == *info.checksum_algorithm);
  return stored.value();
}

//  按客户端声明的算法算摘要（用于构造"客户端给的是对的、也必须被覆写"的场景）
std::string DigestOf(const std::string& content, fss::crypto::ChecksumAlgorithm algorithm) {
  fss::crypto::Hasher hasher(algorithm);
  hasher.Update(content);
  return hasher.HexDigest();
}

}  // namespace

TEST_CASE("★ C6.4 客户端未提供校验和 → 服务端计算 SHA-256 并写回两处",
          "[phase6][integration][c6.4]") {
  AppFixture fx;
  const std::string payload = "checksum-from-server";
  const auto uploaded = UploadWithContent(fx, payload);

  auto record = AppFixture::MakeRecord(uploaded.file_source, "auto.bin");
  CreateFileMetadata create(*fx.ports);
  const auto id = create.Execute(fx.caller, record);
  INFO("create 失败：" << (id.ok() ? std::string() : id.error().ToString()));
  REQUIRE(id.ok());

  const auto stored = Fetch(fx, id.value());
  REQUIRE(stored.ok());
  REQUIRE(*stored.value().data.checksum == fss::crypto::Sha256Hex(payload));
  REQUIRE(*stored.value().data.checksum_algorithm == "SHA256");
}

TEST_CASE("★ C6.4 客户端提供的校验和被**覆写**（不是待校验的断言）—— 上游 golden 样例语义",
          "[phase6][integration][c6.4]") {
  AppFixture fx;
  const std::string payload = "golden-sample";
  const auto uploaded = UploadWithContent(fx, payload);
  const ObjectRef persistent = PersistentRef(fx, uploaded.staging_ref);

  //  ★ 与 `File_CorrectPayload.json` 完全同形：值错，且声明的算法（32 个 hex + SHA-256）也不自洽
  auto record = AppFixture::MakeRecord(uploaded.file_source, "client-sum.bin");
  record.data.dataset_properties.file_source_info.checksum = "d41d8cd98f00b204e9800998ecf8427e";
  record.data.dataset_properties.file_source_info.checksum_algorithm = "SHA-256";

  CreateFileMetadata create(*fx.ports);
  const auto id = create.Execute(fx.caller, record);
  INFO("create 失败：" << (id.ok() ? std::string() : id.error().ToString()));
  REQUIRE(id.ok());  // ← 上游 golden 样例期望 201；"比对不符 → 400"会在这里破功（P6-D05）

  //  自证对照（R1）：服务端必须写回**真实**摘要。若实现原样回传客户端值，这里立刻失败。
  const auto stored = Fetch(fx, id.value());
  REQUIRE(stored.ok());
  REQUIRE(*stored.value().data.checksum == fss::crypto::Sha256Hex(payload));
  REQUIRE(*stored.value().data.checksum != "d41d8cd98f00b204e9800998ecf8427e");
  REQUIRE(*stored.value().data.checksum_algorithm == "SHA256");

  //  且 persistent 对象**仍然存在**（覆写语义下不存在"因校验和不符而回滚删除"这条路径）
  const auto after = fx.blob.stat(persistent);
  REQUIRE(after.ok());
  REQUIRE(after.value().exists);

  //  幂等键也已占用：同一 FileSource 再提交一次返回同一条记录（而不是又新建一条）
  const auto again = create.Execute(fx.caller, record);
  REQUIRE(again.ok());
  REQUIRE(again.value() == id.value());
}

TEST_CASE("★ C6.4 客户端点名别的算法（MD5/SHA-1）也**不改变**服务端结果",
          "[phase6][integration][c6.4]") {
  struct AlgorithmCase {
    const char* name;
    fss::crypto::ChecksumAlgorithm algorithm;
  };
  const std::vector<AlgorithmCase> cases = {
      {"SHA-256", fss::crypto::ChecksumAlgorithm::kSha256},
      {"MD5", fss::crypto::ChecksumAlgorithm::kMd5},
      {"SHA-1", fss::crypto::ChecksumAlgorithm::kSha1},
  };

  for (const auto& test_case : cases) {
    INFO("客户端声明算法：" << test_case.name);
    AppFixture fx;
    const std::string payload = std::string("client-algo-") + test_case.name;
    const auto uploaded = UploadWithContent(fx, payload);

    //  客户端给的是**它按自己声明的算法算出来的正确值** —— 仍然必须被覆写
    auto record = AppFixture::MakeRecord(uploaded.file_source, "client-algo.bin");
    record.data.dataset_properties.file_source_info.checksum = DigestOf(payload, test_case.algorithm);
    record.data.dataset_properties.file_source_info.checksum_algorithm = test_case.name;

    CreateFileMetadata create(*fx.ports);
    const auto id = create.Execute(fx.caller, record);
    INFO("create 失败：" << (id.ok() ? std::string() : id.error().ToString()));
    REQUIRE(id.ok());

    //  记录里是**存储侧**的算法与值（内存驱动的原生算法恒为 SHA-256）
    const auto stored = Fetch(fx, id.value());
    REQUIRE(stored.ok());
    REQUIRE(*stored.value().data.checksum == fss::crypto::Sha256Hex(payload));
    REQUIRE(*stored.value().data.checksum_algorithm == "SHA256");
  }
}

TEST_CASE("★ C6.4 算法跟随驱动：原生给 MD5（Azure 风格）→ 记录写回 MD5",
          "[phase6][integration][c6.4]") {
  AppFixture fx;
  const std::string payload = "azure-style-md5";
  //  ★ 驱动替身把 `copy` 的原生校验和换成 MD5（模拟 `storageUtil.getChecksum` 在 Azure 上
  //    返回 MD5 —— 调研 §2.1 第 7 条的括号注释）。写法故意用小写，验证规范名。
  const std::string md5_hex = DigestOf(payload, fss::crypto::ChecksumAlgorithm::kMd5);

  fss::test::CapabilityOverrideBlobStore store(fx.blob, fx.blob.capabilities(),
                                               "https://self.invalid");
  store.copy_checksum_override = md5_hex;
  store.copy_checksum_algorithm = "md5";
  fx.factory.SetZoneStore(StorageZone::kStaging, store);
  fx.factory.SetZoneStore(StorageZone::kPersistent, store);

  const auto uploaded = UploadWithContent(fx, payload);
  auto record = AppFixture::MakeRecord(uploaded.file_source, "md5-native.bin");
  //  客户端给一个**错的** SHA-256：它依然被覆写（覆写成驱动给的 MD5）
  record.data.dataset_properties.file_source_info.checksum = std::string(64, 'b');
  record.data.dataset_properties.file_source_info.checksum_algorithm = "SHA-256";

  CreateFileMetadata create(*fx.ports);
  const auto id = create.Execute(fx.caller, record);
  INFO("create 失败：" << (id.ok() ? std::string() : id.error().ToString()));
  REQUIRE(id.ok());

  const auto stored = Fetch(fx, id.value());
  REQUIRE(stored.ok());
  REQUIRE(*stored.value().data.checksum == md5_hex);          // 值来自驱动
  REQUIRE(*stored.value().data.checksum_algorithm == "MD5");  // 规范名，且**不是** SHA256
}

TEST_CASE("★ C6.4 原生校验和不可用（ETAG/空值/非法 hex）→ 回退**流式回算 SHA-256**",
          "[phase6][integration][c6.4]") {
  struct NativeCase {
    const char* checksum;
    const char* algorithm;
  };
  const std::vector<NativeCase> cases = {
      {"9bb58f26192e4ba00f01e2e7b136bbd8", "ETAG"},  // 32 个 hex，但不是 SHA-256 的摘要长度
      {"\"9bb58f26192e4ba00f01e2e7b136bbd8\"", ""},  // S3 风格 ETag（带引号）、算法未给
      {"", "SHA256"},                                // 算法认识但没有值
      {"zzzz", "SHA256"},                            // 长度与字符集都不对
  };

  for (const auto& test_case : cases) {
    INFO("驱动原生值：" << test_case.checksum << " / " << test_case.algorithm);
    AppFixture fx;
    const std::string payload = std::string("native-unusable-") + test_case.algorithm;
    fss::test::CapabilityOverrideBlobStore store(fx.blob, fx.blob.capabilities(),
                                                 "https://self.invalid");
    store.copy_checksum_override = test_case.checksum;
    store.copy_checksum_algorithm = test_case.algorithm;
    fx.factory.SetZoneStore(StorageZone::kStaging, store);
    fx.factory.SetZoneStore(StorageZone::kPersistent, store);

    const auto uploaded = UploadWithContent(fx, payload);
    auto record = AppFixture::MakeRecord(uploaded.file_source, "native-unusable.bin");

    CreateFileMetadata create(*fx.ports);
    const auto id = create.Execute(fx.caller, record);
    INFO("create 失败：" << (id.ok() ? std::string() : id.error().ToString()));
    REQUIRE(id.ok());

    const auto stored = Fetch(fx, id.value());
    REQUIRE(stored.ok());
    //  ★ 绝不能把 ETAG 写进记录（那是"看起来有值"的假象）
    REQUIRE(*stored.value().data.checksum == fss::crypto::Sha256Hex(payload));
    REQUIRE(*stored.value().data.checksum_algorithm == "SHA256");
  }
}

// =============================================================================
//  C6.4 / C6.9：校验和必须是**流式**的（真实磁盘栈 + 绝对 RSS 上限 + 自证对照）
// =============================================================================
//  为什么不用内存适配器测这一条
//    `InMemoryBlobStore::copy` 本身就会在 RAM 里复制一份对象数据，RSS 增长必然 ≈ 对象大小 ——
//    那是**适配器**的固有开销，会把"校验和是否流式"的信号淹没。
//    所以这一条必须跑在**真实 POSIX 存储**上：对象在磁盘上，复制走跨 store 流式路径。
//
//  怎么把实现逼上"回算"这条路
//    持久区驱动替身 `hide_checksum = true`（模拟"驱动不提供校验和"）→ 原生不可用 →
//    必须**流式回算** SHA-256。这正是"驱动能力弱"的真实场景。
//
//  自证对照（R1）
//    同一进程内再做一次**故意的整块读回**（`StringSink`），断言 RSS 增长 ≥ 32 MiB。
//    若没有这一步，"增长 < 8 MiB"无法区分"实现流式"与"测量根本没生效"。
namespace {

//  最小 POSIX 装配：staging / persistent 两个**不同**的 PosixBlobStore
struct PosixTwoStoreFixture {
  fss::test::TempDir dir{"metadata_lifecycle"};
  fss::ManualClock clock{1700000000};
  fss::SequentialIdGenerator ids{1};
  fss::infra::PosixBlobStore blob;  // staging
  std::unique_ptr<fss::infra::PosixBlobStore> persistent;
  //  ★ 持久区外面套一层"不报校验和"的能力替身 → 把用例逼上流式回算路径
  std::unique_ptr<fss::test::CapabilityOverrideBlobStore> persistent_view;
  fss::infra::InMemoryLocationRepository locations;
  fss::infra::InMemoryMetadataRepository metadata{clock};
  fss::test::FakeBlobStoreFactory factory{blob};
  std::unique_ptr<fss::app::LocationIssuer> issuer;
  std::unique_ptr<fss::app::UseCasePorts> ports;
  fss::app::CallerContext caller{"opendes", "osdu-user", "Bearer test-token"};

  PosixTwoStoreFixture() : blob(dir.child("blobs"), clock) {
    fss::infra::PosixBlobStoreOptions options;
    options.fsync_policy = fss::infra::FsyncPolicy::kBySize;  // 大文件不必每条都 fsync
    persistent =
        std::make_unique<fss::infra::PosixBlobStore>(dir.child("persistent"), clock, options);
    persistent_view = std::make_unique<fss::test::CapabilityOverrideBlobStore>(
        *persistent, persistent->capabilities(), "https://self.invalid");
    persistent_view->hide_checksum = true;
    factory.SetZoneStore(fss::domain::StorageZone::kPersistent, *persistent_view);
    issuer = std::make_unique<fss::app::LocationIssuer>(factory, locations, codec_, clock, ids,
                                                       "https://self.invalid");
    //  ★ 必须用**成员**替身：局部变量的引用会被 UseCasePorts 存下来，构造完就悬空
    ports = std::make_unique<fss::app::UseCasePorts>(fss::app::UseCasePorts{
        factory, locations, metadata, authorizer_, events_, audit_, partitions_, legal_,
        schema_, *issuer, clock, ids});
  }

 private:
  fss::test::RecordingSelfSignedCodec codec_;
  fss::test::AllowAllAuthorizer authorizer_;
  fss::test::RecordingEventPublisher events_;
  fss::test::RecordingAuditLogger audit_;
  fss::test::FakePartitionRegistry partitions_;
  fss::test::NoopLegalValidator legal_;
  fss::test::NoopSchemaValidator schema_;
};

//  ★ "流式"这条断言的 RSS 上限：默认 8 MiB（对象 64 MiB 的 1/8，判别力足够）。
//    ASan/UBSan 构建下影子内存与隔离区会显著抬高 RSS 基线，"流式"不再度量同一件事 ——
//    与 `tests/framework/big_file.h` 的既有约定一致，允许用 `FSS_TEST_RSS_LIMIT_KIB` 覆盖
//    （`scripts/run_sanitizers.sh` 设为 512 MiB）。**紧的那个上限在普通门槛里跑**：
//    这是刻意的取舍，不是"绕过失败"（1 GiB 的完整用例同理，见 run_sanitizers.sh 顶部说明）。
std::uint64_t StreamRssLimitKib() {
  if (const char* env = std::getenv("FSS_TEST_RSS_LIMIT_KIB")) {
    const auto value = std::strtoull(env, nullptr, 10);
    if (value > 0) return value;
  }
  return 8 * 1024;
}

//  流式算一个"重复模式"字节源的摘要（测试自己算期望值，同样不整块驻留）
std::string DigestOfPattern(std::int64_t total) {
  fss::crypto::Hasher hasher(fss::crypto::ChecksumAlgorithm::kSha256);
  fss::bytes::RepeatingSource source(total);
  std::vector<char> buffer(1024 * 1024);
  while (true) {
    const auto read = source.Read(buffer.data(), buffer.size());
    REQUIRE(read.ok());
    if (read.value() == 0) break;
    hasher.Update(std::string_view(buffer.data(), read.value()));
  }
  return hasher.HexDigest();
}

}  // namespace

TEST_CASE("★ C6.4/C6.9 校验和**流式**回算：64 MiB 对象 + RSS 上限 + 整块读回对照",
          "[phase6][integration][c6.4]") {
  PosixTwoStoreFixture fx;
  constexpr std::int64_t kObjectBytes = 64LL * 1024 * 1024;

  fss::bytes::RepeatingSource source(kObjectBytes);
  const auto uploaded = UploadSource(fx, source);

  auto record = AppFixture::MakeRecord(uploaded.file_source, "big.bin");

  const std::uint64_t baseline = fss::test::CurrentRssKib();
  CreateFileMetadata create(*fx.ports);
  const auto id = create.Execute(fx.caller, record);
  const std::uint64_t after = fss::test::CurrentRssKib();
  INFO("create 结果：" << (id.ok() ? std::string("ok") : id.error().ToString()));
  REQUIRE(id.ok());

  const std::uint64_t growth = after > baseline ? after - baseline : 0;
  INFO("流式回算前后 RSS 增长 " << growth << " KiB（对象 65536 KiB，上限 "
                               << StreamRssLimitKib() << " KiB）");
  //  ★ 流式：增长必须远小于对象（默认上限 8 MiB = 对象的 1/8）
  REQUIRE(growth < StreamRssLimitKib());

  //  期望值由测试**独立**流式算出（不是抄实现里的常量）
  const std::string expected = DigestOfPattern(kObjectBytes);
  const auto stored = Fetch(fx, id.value());
  REQUIRE(stored.ok());
  REQUIRE(*stored.value().data.checksum == expected);
  REQUIRE(*stored.value().data.checksum_algorithm == "SHA256");

  //  ---- 自证对照（R1）：整块读回 64 MiB 必须被同一套测量抓到 ----------------
  const auto persistent_container = fss::app::ObjectKeyPolicy::ContainerFor(
      fx.caller.partition, StorageZone::kPersistent);
  REQUIRE(persistent_container.ok());
  ObjectRef persistent_ref;
  persistent_ref.container = persistent_container.value();
  persistent_ref.key = uploaded.staging_ref.key;

  const std::uint64_t control_baseline = fss::test::CurrentRssKib();
  fss::bytes::StringSink whole_object;
  const auto read_all = fx.persistent->get(persistent_ref, whole_object, fss::domain::ByteRange{});
  REQUIRE(read_all.ok());
  REQUIRE(whole_object.bytes_written() == static_cast<std::size_t>(kObjectBytes));
  const std::uint64_t control_after = fss::test::CurrentRssKib();
  const std::uint64_t control_growth =
      control_after > control_baseline ? control_after - control_baseline : 0;
  INFO("对照（整块读回）RSS 增长 " << control_growth << " KiB");
  REQUIRE(control_growth >= 32 * 1024);  // 证明该测量确实能发现整块驻留
}

// =============================================================================
//  C6.3：契约 §2.6 的 12 步序列 + **6 个故障注入点**
// =============================================================================
//  分工（故障点 ↔ 期望）：
//    ① 第 1 步 `IN_PROGRESS` 事件失败 → **非致命**，流程继续（仍是 201）
//    ② 第 6 步 复制失败            → `502` + FAILED 事件 + 审计失败 + 无记录 + 无 persistent 对象
//    ③ 第 7 步 校验和回算失败      → **回滚删除 persistent** + FAILED + 502 + 无记录
//    ④ 第 9 步 写记录失败          → **回滚删除 persistent** + FAILED + 500；staging 仍然保留
//    ⑤ 第 10 步 `SUCCESS` 事件失败 → **非致命**，仍是 201 且记录已落地
//    ⑥ 第 11 步 删 staging 失败    → 仍是 **201**（上游 issue #76）+ 审计告警 `createMetadataStagingCleanupFailure`
//
//  ★ 顺序断言用的是**可观测副作用**（事件序列 / 审计 / 对象是否存在 / 仓储计数），而不是
//    "读一遍代码觉得顺序对"：例如"第 6 步失败时记录数为 0"证明复制发生在写记录**之前**；
//    "第 9 步失败时 staging 仍在"证明 staging 清理发生在写记录**之后**。
namespace {

bool HasAudit(const fss::test::RecordingAuditLogger& audit, std::string_view operation,
              std::string_view result) {
  for (const auto& event : audit.events) {
    if (event.operation == operation && event.result == result) return true;
  }
  return false;
}

std::size_t CountAudit(const fss::test::RecordingAuditLogger& audit, std::string_view operation) {
  std::size_t n = 0;
  for (const auto& event : audit.events) {
    if (event.operation == operation) ++n;
  }
  return n;
}

}  // namespace

TEST_CASE("★ C6.3 12 步序列：正常路径的顺序与副作用", "[phase6][integration][c6.3]") {
  AppFixture fx;
  fx.caller.correlation_id = "corr-c6.3";  // 适配层会从 `correlation-id` 头填这个字段
  const std::string payload = "sequence-happy";
  const auto uploaded = UploadWithContent(fx, payload);
  const ObjectRef persistent = PersistentRef(fx, uploaded.staging_ref);

  auto record = AppFixture::MakeRecord(uploaded.file_source, "happy.bin");
  CreateFileMetadata create(*fx.ports);
  const auto id = create.Execute(fx.caller, record);
  INFO("create 失败：" << (id.ok() ? std::string() : id.error().ToString()));
  REQUIRE(id.ok());

  // 第 1/10 步：IN_PROGRESS 在前、SUCCESS 在后（各自恰好一次）
  const auto statuses = fx.events.statuses();
  REQUIRE(statuses == std::vector<std::string>{"IN_PROGRESS", "SUCCESS"});
  REQUIRE(fx.events.events[1].version == 1);   // SUCCESS 带 version
  REQUIRE(fx.events.events[0].dataset_sync == "DATASET_SYNC");

  // 第 6 步：persistent 对象存在（且内容正确）
  const auto stat = fx.blob.stat(persistent);
  REQUIRE(stat.ok());
  REQUIRE(stat.value().exists);

  // 第 7 步：校验和已写回（记录里的值 == 真实字节的 SHA-256）
  const auto stored = Fetch(fx, id.value());
  REQUIRE(stored.ok());
  REQUIRE(*stored.value().data.checksum == fss::crypto::Sha256Hex(payload));

  // 第 11 步：staging 对象**已被清理**（这是"顺序在写记录之后"的可观测证据）
  const auto staging_after = fx.blob.stat(uploaded.staging_ref);
  REQUIRE(staging_after.ok());
  REQUIRE_FALSE(staging_after.value().exists);

  // 第 10 步的**第二个**事件：datasetDetails（上游 `FileDatasetDetailsPublisher` 的形状）
  REQUIRE(fx.events.details.size() == 1);
  const auto& details = fx.events.details.front();
  REQUIRE(fx.events.last_details_topic == "datasetDetails");
  REQUIRE(details.dataset_id == id.value());
  REQUIRE(details.dataset_version_id == "1");
  REQUIRE(details.dataset_type == "FILE");
  REQUIRE(details.record_count == 1);
  REQUIRE(std::string(fss::domain::DatasetDetailsEvent::kKind) == "datasetDetails");
  //  correlation-id 由适配层填进 CallerContext（这里显式设置，证明它被透传）
  REQUIRE(details.correlation_id == "corr-c6.3");
  REQUIRE(details.timestamp_millis == fx.clock.NowEpochMillis());

  // 第 9/11 步之后的审计：成功事件必须存在，且**没有**失败记录
  REQUIRE(HasAudit(fx.audit, "createMetadataSuccess", "success"));
  REQUIRE_FALSE(HasAudit(fx.audit, "createMetadataFailure", "failure"));
  REQUIRE_FALSE(HasAudit(fx.audit, "createMetadataStagingCleanupFailure", "failure"));

  // 位置记录已迁到 persistent 并记下上传者（getFileList 的 UserID 过滤依赖它）
  const auto location = fx.locations.FindByFileSource(fx.caller.partition, uploaded.file_source);
  REQUIRE(location.ok());
  REQUIRE(location.value().zone == StorageZone::kPersistent);
  REQUIRE(location.value().user_id == fx.caller.user_id);
}

TEST_CASE("★ C6.3 故障①：第 1 步 IN_PROGRESS 事件失败 → 非致命，仍 201",
          "[phase6][integration][c6.3]") {
  AppFixture fx;
  const std::string payload = "fault-step1";
  const auto uploaded = UploadWithContent(fx, payload);

  //  ★ 只让 IN_PROGRESS 失败：SUCCESS 仍必须发出去（否则区分不了"第 1 步非致命"与"事件全挂"）
  fx.events.fail_status = "IN_PROGRESS";

  auto record = AppFixture::MakeRecord(uploaded.file_source, "step1.bin");
  CreateFileMetadata create(*fx.ports);
  const auto id = create.Execute(fx.caller, record);
  INFO("create 失败：" << (id.ok() ? std::string() : id.error().ToString()));
  REQUIRE(id.ok());
  REQUIRE(fx.events.statuses() == std::vector<std::string>{"SUCCESS"});
  REQUIRE(HasAudit(fx.audit, "createMetadataSuccess", "success"));
}

TEST_CASE("★ C6.3 故障②：第 6 步复制失败 → 502 + FAILED + 无记录 + 无持久对象",
          "[phase6][integration][c6.3]") {
  AppFixture fx;
  const auto uploaded = UploadWithContent(fx, "fault-step6");
  const ObjectRef persistent = PersistentRef(fx, uploaded.staging_ref);

  fx.blob.Inject({fss::infra::InMemoryBlobStore::Op::kCopy, 1, fss::ErrorKind::kUnavailable,
                  "injected copy failure"});

  auto record = AppFixture::MakeRecord(uploaded.file_source, "step6.bin");
  CreateFileMetadata create(*fx.ports);
  const auto result = create.Execute(fx.caller, record);
  REQUIRE_FALSE(result.ok());
  REQUIRE(result.error().kind() == fss::ErrorKind::kBadGateway);  // → 502
  REQUIRE(result.error().message().find("复制到 persistent 失败") != std::string::npos);

  // 事件：IN_PROGRESS → FAILED（顺序断言）；审计：失败
  REQUIRE(fx.events.statuses() == std::vector<std::string>{"IN_PROGRESS", "FAILED"});
  REQUIRE(HasAudit(fx.audit, "createMetadataFailure", "failure"));

  // ★ 复制发生在写记录之前：一条记录都不能有
  fss::domain::MetadataQuery query;
  const auto page = fx.metadata.List(fx.caller.partition, query);
  REQUIRE(page.ok());
  REQUIRE(page.value().total == 0);

  // 持久区不留对象；staging 必须**保留**（第 11 步还没到，客户端可以重试）
  REQUIRE_FALSE(fx.blob.stat(persistent).value().exists);
  REQUIRE(fx.blob.stat(uploaded.staging_ref).value().exists);
}

TEST_CASE("★ C6.3 故障③：第 7 步校验和回算失败 → **回滚删除** persistent",
          "[phase6][integration][c6.3]") {
  AppFixture fx;
  const auto uploaded = UploadWithContent(fx, "fault-step7");
  const ObjectRef persistent = PersistentRef(fx, uploaded.staging_ref);

  // 驱动不报校验和 → 第 7 步必须回算；此时对 `get` 注入故障 = 第 7 步失败。
  //  ⚠️ 装饰器必须**就地**构造：`factory` 存的是指针，按值返回会立刻变成悬空引用。
  fss::test::CapabilityOverrideBlobStore store(fx.blob, fx.blob.capabilities(),
                                               "https://self.invalid");
  store.hide_checksum = true;
  fx.factory.SetZoneStore(StorageZone::kStaging, store);
  fx.factory.SetZoneStore(StorageZone::kPersistent, store);
  fx.blob.Inject({fss::infra::InMemoryBlobStore::Op::kGet, 1, fss::ErrorKind::kUnavailable,
                  "injected read failure"});

  auto record = AppFixture::MakeRecord(uploaded.file_source, "step7.bin");
  CreateFileMetadata create(*fx.ports);
  const auto result = create.Execute(fx.caller, record);
  REQUIRE_FALSE(result.ok());
  REQUIRE(result.error().kind() == fss::ErrorKind::kBadGateway);
  REQUIRE(result.error().message().find("计算校验和失败") != std::string::npos);

  REQUIRE(fx.events.statuses() == std::vector<std::string>{"IN_PROGRESS", "FAILED"});
  REQUIRE(HasAudit(fx.audit, "createMetadataFailure", "failure"));

  // ★ 第 12 步：已搬迁的 persistent 对象必须被**回滚删除**（此前这里是 `FSS_TRY`，漏了回滚）
  REQUIRE_FALSE(fx.blob.stat(persistent).value().exists);
  // staging 仍在（第 11 步未执行）
  REQUIRE(fx.blob.stat(uploaded.staging_ref).value().exists);

  fss::domain::MetadataQuery query;
  REQUIRE(fx.metadata.List(fx.caller.partition, query).value().total == 0);
}

TEST_CASE("★ C6.3 故障④：第 9 步写记录失败 → 回滚删除 persistent + 500",
          "[phase6][integration][c6.3]") {
  AppFixture fx;
  const auto uploaded = UploadWithContent(fx, "fault-step9");
  const ObjectRef persistent = PersistentRef(fx, uploaded.staging_ref);

  fss::test::FaultyMetadataRepository failing{fx.metadata};
  failing.fail_create = true;
  fx.UseMetadata(failing);

  auto record = AppFixture::MakeRecord(uploaded.file_source, "step9.bin");
  CreateFileMetadata create(*fx.ports);
  const auto result = create.Execute(fx.caller, record);
  REQUIRE_FALSE(result.ok());
  REQUIRE(result.error().kind() == fss::ErrorKind::kInternal);  // → 500
  REQUIRE(result.error().message().find("写入元数据记录失败") != std::string::npos);
  REQUIRE(failing.create_calls == 1);

  REQUIRE(fx.events.statuses() == std::vector<std::string>{"IN_PROGRESS", "FAILED"});
  REQUIRE(HasAudit(fx.audit, "createMetadataFailure", "failure"));

  // ★ 回滚：刚复制过去的 persistent 对象必须被删掉（否则留下无主副本）
  REQUIRE_FALSE(fx.blob.stat(persistent).value().exists);
  // ★ staging 仍在 → 证明第 11 步的清理确实发生在第 9 步**之后**
  REQUIRE(fx.blob.stat(uploaded.staging_ref).value().exists);
}

TEST_CASE("★ C6.3 故障⑤：第 10 步 SUCCESS 事件失败 → 非致命，仍 201",
          "[phase6][integration][c6.3]") {
  AppFixture fx;
  const auto uploaded = UploadWithContent(fx, "fault-step10");
  const ObjectRef persistent = PersistentRef(fx, uploaded.staging_ref);

  fx.events.fail_status = "SUCCESS";

  auto record = AppFixture::MakeRecord(uploaded.file_source, "step10.bin");
  CreateFileMetadata create(*fx.ports);
  const auto id = create.Execute(fx.caller, record);
  INFO("create 失败：" << (id.ok() ? std::string() : id.error().ToString()));
  REQUIRE(id.ok());

  // 记录已落地、persistent 在、staging 已清理 —— 事件失败不影响任何一步
  const auto stored = Fetch(fx, id.value());
  REQUIRE(stored.ok());
  REQUIRE(fx.blob.stat(persistent).value().exists);
  REQUIRE_FALSE(fx.blob.stat(uploaded.staging_ref).value().exists);
  REQUIRE(fx.events.statuses() == std::vector<std::string>{"IN_PROGRESS"});
  REQUIRE(HasAudit(fx.audit, "createMetadataSuccess", "success"));
}

TEST_CASE("★ C6.3 故障⑥：第 11 步删 staging 失败 → 仍 201 + 审计告警",
          "[phase6][integration][c6.3]") {
  AppFixture fx;
  const auto uploaded = UploadWithContent(fx, "fault-step11");
  const ObjectRef persistent = PersistentRef(fx, uploaded.staging_ref);

  // 第 11 步的 remove 是本次流程里唯一一次 remove（6/7/9 都没失败）
  fx.blob.Inject({fss::infra::InMemoryBlobStore::Op::kRemove, 1, fss::ErrorKind::kUnavailable,
                  "injected remove failure"});

  auto record = AppFixture::MakeRecord(uploaded.file_source, "step11.bin");
  CreateFileMetadata create(*fx.ports);
  const auto id = create.Execute(fx.caller, record);
  INFO("create 失败：" << (id.ok() ? std::string() : id.error().ToString()));
  // ★ 上游 issue #76：清理失败**不得**让已成功的登记变失败
  REQUIRE(id.ok());

  const auto stored = Fetch(fx, id.value());
  REQUIRE(stored.ok());
  REQUIRE(fx.blob.stat(persistent).value().exists);
  // staging 没删掉（故障注入生效）→ 必须留下审计告警，而不是静默
  REQUIRE(fx.blob.stat(uploaded.staging_ref).value().exists);
  REQUIRE(HasAudit(fx.audit, "createMetadataStagingCleanupFailure", "failure"));
  REQUIRE(CountAudit(fx.audit, "createMetadataStagingCleanupFailure") == 1);
  REQUIRE(HasAudit(fx.audit, "createMetadataSuccess", "success"));
  // 事件序列不受影响（第 10 步已经成功）
  REQUIRE(fx.events.statuses() == std::vector<std::string>{"IN_PROGRESS", "SUCCESS"});
}

TEST_CASE("★ C6.3 故障⑦（追加）：第 10 步 datasetDetails 发布失败 → 非致命，仍 201",
          "[phase6][integration][c6.3]") {
  AppFixture fx;
  const auto uploaded = UploadWithContent(fx, "fault-dataset-details");
  fx.events.fail_dataset_details = true;

  auto record = AppFixture::MakeRecord(uploaded.file_source, "details.bin");
  CreateFileMetadata create(*fx.ports);
  const auto id = create.Execute(fx.caller, record);
  INFO("create 失败：" << (id.ok() ? std::string() : id.error().ToString()));
  //  ★ 上游只 `log.warning("Failed to publish dataset details")` —— 不得影响 201
  REQUIRE(id.ok());
  REQUIRE(fx.events.details_calls == 1);   // 真的尝试发布过
  REQUIRE(fx.events.details.empty());      // 但失败了
  REQUIRE(fx.events.statuses() == std::vector<std::string>{"IN_PROGRESS", "SUCCESS"});
  REQUIRE(HasAudit(fx.audit, "createMetadataSuccess", "success"));
}

// -----------------------------------------------------------------------------
//  C6.3 第 10 步的"真的把 correlation-id 透传下去了吗"—— 端到端（真实端口）
// -----------------------------------------------------------------------------
//  ★ R15：`CallerContext.correlation_id` 这条约定如果只有单元断言，就无法证明**适配层**
//    真的从 `x-correlation-id` 头填了它。这里走完整 HTTP 路径（uploadURL → PUT → POST metadata）。
TEST_CASE("★ C6.3 端到端：`x-correlation-id` 头 → datasetDetails 事件",
          "[phase6][integration][c6.3]") {
  fss::test::HttpFixture fx;
  const int port = fx.port();

  auto headers = fss::test::Authed();
  headers.push_back("x-correlation-id: corr-e2e-c6.3");

  const auto upload = fss::test::HttpDo(port, "GET", "/api/file/v2/files/uploadURL", headers);
  REQUIRE(upload.status == 200);
  const auto upload_json = fss::json::ParseObject(upload.body);
  REQUIRE(upload_json.ok());
  const std::string file_source =
      upload_json.value()["Location"]["FileSource"].get<std::string>();

  const auto put = fss::test::HttpDo(
      port, "PUT", fss::test::TargetOf(upload_json.value()["Location"]["SignedURL"].get<std::string>()),
      headers, "correlation-body");
  REQUIRE(put.status == 200);

  auto record = AppFixture::MakeRecord(file_source, "corr.bin");
  const auto created = fss::test::HttpDo(port, "POST", "/api/file/v2/files/metadata", headers,
                                        fss::json::Dump(fss::domain::ToJson(record)));
  INFO("POST metadata → " << created.status << " " << created.body);
  REQUIRE(created.status == 201);

  REQUIRE(fx.events.statuses() == std::vector<std::string>{"IN_PROGRESS", "SUCCESS"});
  REQUIRE(fx.events.details.size() == 1);
  const auto& details = fx.events.details.front();
  REQUIRE(details.correlation_id == "corr-e2e-c6.3");
  REQUIRE(details.partition == "opendes");
  REQUIRE(details.dataset_id ==
          fss::json::ParseObject(created.body).value()["id"].get<std::string>());
  REQUIRE(details.dataset_version_id == "1");
  REQUIRE(details.dataset_type == "FILE");
  REQUIRE(details.record_count == 1);
}

// =============================================================================
//  C6.9：≥1 GiB staging→persistent 搬迁 + 校验和回算的 RSS
// =============================================================================
//  ★ 为什么另立一例（而不是把上面那例的规模调大）：
//    上面 64 MiB 那例带"整块读回"的**自证对照**，跑得快、判别力强，适合每次门槛都跑；
//    这一例负责判据 C6.9 的**规模**（≥1 GiB，RSS 峰值增长 < 64 MiB）。
//  ★ 为什么用 `CurrentRssKib()` 的增量而不是 `PeakRssKib()`（VmHWM）：
//    VmHWM 是**单调不减**的，同一个二进制里前面的用例（尤其带 64 MiB 对照的那例）
//    会把它抬上去，之后测到的 Δ 会失真甚至变成 0（"恒真"的假断言）。
//    规模与上限可被构建方式覆盖（ASan 下缩到 64 MiB / 放宽上限，见 big_file.h）。
TEST_CASE("★ C6.9 ≥1 GiB 搬迁 + 流式回算：RSS 增长 < 64 MiB",
          "[phase6][integration][c6.9]") {
  PosixTwoStoreFixture fx;
  const std::int64_t bytes = fss::test::BigFileBytes();

  const std::uint64_t baseline = fss::test::CurrentRssKib();
  fss::bytes::RepeatingSource source(bytes);
  const auto uploaded = UploadSource(fx, source);
  auto record = AppFixture::MakeRecord(uploaded.file_source, "huge.bin");
  CreateFileMetadata create(*fx.ports);
  const auto id = create.Execute(fx.caller, record);
  const std::uint64_t after = fss::test::CurrentRssKib();
  INFO("create 结果：" << (id.ok() ? std::string("ok") : id.error().ToString()));
  REQUIRE(id.ok());

  const std::uint64_t growth = after > baseline ? after - baseline : 0;
  INFO("对象 " << bytes / (1024 * 1024) << " MiB，RSS 增长 " << growth << " KiB（上限 "
               << fss::test::RssLimitKib() << " KiB）");
  REQUIRE(growth < fss::test::RssLimitKib());

  //  值与独立流式算出的摘要一致（1 GiB 上也证明"读的是完整对象"）
  const std::string expected = DigestOfPattern(bytes);
  const auto stored = Fetch(fx, id.value());
  REQUIRE(stored.ok());
  REQUIRE(*stored.value().data.checksum == expected);
  REQUIRE(*stored.value().data.checksum_algorithm == "SHA256");
}
