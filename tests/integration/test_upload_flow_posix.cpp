// =============================================================================
//  C4.8：端到端垂直切片（POSIX 模式）—— uploadURL → PUT 字节 → metadata → downloadURL
//        → GET 字节 → SHA-256 比对 → DELETE → 404
// =============================================================================
//  为什么必须走真实端口 + 真实字节：
//    · 数据面走的是 `/v1/transfer/{token}`（自签 URL），**不是**普通 JSON 端点；
//      只有真正 PUT/GET 一遍才能证明"token 校验 + 流式转发 + 自签 URL 的查询串"接对了。
//    · 字节比对用 SHA-256：只比长度会漏掉"内容被截断/串位"这类缺陷。
//
//  规模：默认 **100 MiB**（判据要求 ≥100 MiB）；`FSS_TEST_BIG_BYTES` 更小时取更小值
//  （sanitizer 构建用它缩小规模；该次运行不作为 C4.8 的证据）。
//
//  装配（真实自签 codec + 数据面回调 + 真实端口）在 `tests/framework/http_fixture.h`。
// =============================================================================
#include <catch2/catch.hpp>

#include "big_file.h"
#include "http_fixture.h"

#include "common/crypto/crypto.h"
#include "common/json/json.h"
#include "domain/model/file_metadata.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using fss::test::Authed;
using fss::test::HttpDo;
using fss::test::HttpFixture;
using fss::test::TargetOf;

std::int64_t UploadBytes() {
  const std::int64_t kHundredMiB = 100LL * 1024 * 1024;
  return std::min(kHundredMiB, fss::test::BigFileBytes());
}

std::string MakePayload(std::size_t bytes) {
  std::string payload(bytes, '\0');
  for (std::size_t i = 0; i < bytes; ++i) {
    payload[i] = static_cast<char>('A' + (i * 7 % 26));
  }
  return payload;
}

//  在同一个 fixture 里把一条记录传完（uploadURL → metadata），返回各关键值
struct UploadedFile {
  std::string file_id;
  std::string file_source;
  std::string put_url;
  std::string get_url;
  std::string record_id;
};

UploadedFile Upload(HttpFixture& fixture, int port, const std::string& payload) {
  UploadedFile out;
  const auto upload = HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed());
  REQUIRE(upload.status == 200);
  const auto upload_json = fss::json::ParseObject(upload.body);
  REQUIRE(upload_json.ok());
  out.file_id = upload_json.value()["FileID"].get<std::string>();
  out.file_source = upload_json.value()["Location"]["FileSource"].get<std::string>();
  out.put_url = upload_json.value()["Location"]["SignedURL"].get<std::string>();
  REQUIRE(out.put_url.find("/v1/transfer/") != std::string::npos);

  const auto put = HttpDo(port, "PUT", TargetOf(out.put_url), Authed(), payload);
  REQUIRE(put.status == 200);

  auto record = fss::test::AppFixture::MakeRecord(out.file_source, "e2e.bin");
  const auto created = HttpDo(port, "POST", "/api/file/v2/files/metadata", Authed(),
                              fss::json::Dump(fss::domain::ToJson(record)));
  REQUIRE(created.status == 201);
  const auto created_json = fss::json::ParseObject(created.body);
  REQUIRE(created_json.ok());
  out.record_id = created_json.value()["id"].get<std::string>();

  const auto download =
      HttpDo(port, "GET", "/api/file/v2/files/" + out.file_id + "/downloadURL", Authed());
  REQUIRE(download.status == 200);
  const auto download_json = fss::json::ParseObject(download.body);
  REQUIRE(download_json.ok());
  out.get_url = download_json.value()["SignedUrl"].get<std::string>();
  return out;
}

}  // namespace

TEST_CASE("★ C4.8 端到端：自签 URL 上传真实字节 → 元数据 → 下载 → SHA-256 一致 → 删除",
          "[phase4][integration][c4.8]") {
  HttpFixture fixture;
  const int port = fixture.port();

  const std::int64_t total = UploadBytes();
  INFO("上传字节数 " << total);

  //  ① 上传 + 登记元数据 + 取下载地址（整条链路见 Upload()）
  const std::string payload = MakePayload(static_cast<std::size_t>(total));
  const std::string expected_sha = fss::crypto::Sha256Hex(payload);
  const UploadedFile file = Upload(fixture, port, payload);
  const std::string& get_url = file.get_url;
  const std::string& record_id = file.record_id;

  //  ⑤ 下载并做 SHA-256 比对（只比长度会漏掉内容错位）
  const auto got = HttpDo(port, "GET", TargetOf(get_url), Authed());
  REQUIRE(got.status == 200);
  REQUIRE(got.body.size() == static_cast<std::size_t>(total));
  REQUIRE(fss::crypto::Sha256Hex(got.body) == expected_sha);

  //  ⑤b `Range`：设计里承诺"数据面支持断点续传/分片下载"（R15：能力型约定必须有测试
  //      真的走过那条路径）。取**尾部**区间是为了同时验证 `BlobByteSource::Seek`——
  //      从头开始读的实现会"碰巧"通过首段区间测试。
  const std::vector<std::string> tail_headers = {"Range: bytes=" + std::to_string(total - 16) +
                                                 "-" + std::to_string(total - 1)};
  const auto ranged = HttpDo(port, "GET", TargetOf(get_url), tail_headers);
  REQUIRE(ranged.status == 206);
  REQUIRE(ranged.body == payload.substr(static_cast<std::size_t>(total) - 16, 16));

  //  ⑥ 删除 → 204；再取元数据 → 404
  const auto removed =
      HttpDo(port, "DELETE", "/api/file/v2/files/" + record_id + "/metadata", Authed());
  REQUIRE(removed.status == 204);
  const auto gone = HttpDo(port, "GET", "/api/file/v2/files/" + record_id + "/metadata", Authed());
  REQUIRE(gone.status == 404);
}

TEST_CASE("★ C4.8 边界：0 字节对象同样能走完整条链路", "[phase4][integration][c4.8]") {
  HttpFixture fixture;
  const int port = fixture.port();

  //  0 字节：PUT 必须显式 `Content-Length: 0`（HttpDo 已统一补），其余各步与 100 MiB 用例相同
  const UploadedFile file = Upload(fixture, port, "");
  const auto got = HttpDo(port, "GET", TargetOf(file.get_url), Authed());
  REQUIRE(got.status == 200);
  REQUIRE(got.body.empty());
  REQUIRE(fss::crypto::Sha256Hex(got.body) == fss::crypto::Sha256Hex(""));
}

TEST_CASE("★ C4.8 负例：数据面的鉴权/存在性判定（P4-D06 的 404 分支自证）",
          "[phase4][integration][c4.8]") {
  HttpFixture fixture;
  const int port = fixture.port();
  const std::string base = "http://127.0.0.1" + std::string(fss::adapters::http::kDefaultBasePath);

  //  自证对照：同一个签名密钥、同一个容器，**只把 object_key 换成不存在的键**。
  //  若 `Stat` 把 `exists=false` 当成 size=0，这里会得到 200 + 空体（P4-D06 的原症状）。
  fss::domain::TransferToken ghost;
  ghost.partition = "opendes";
  ghost.file_id = "00000000000000000000000000000099";
  ghost.container = "opendes-persistent";
  ghost.object_key = "osdu-user/ghost/does-not-exist";
  ghost.zone = fss::domain::StorageZone::kPersistent;
  ghost.op = "get";
  ghost.expires_at_epoch_seconds = fixture.clock.NowEpochSeconds() + 3600;
  const auto ghost_url = fixture.codec.Encode(ghost, base);
  REQUIRE(ghost_url.ok());

  const auto missing = HttpDo(port, "GET", TargetOf(ghost_url.value()), Authed());
  REQUIRE(missing.status == 404);  // ★ 不是 200

  //  用途绑定：`put` token 不能用来下载（内核判 `op`，不只看签名）
  fss::domain::TransferToken wrong_op = ghost;
  wrong_op.op = "put";
  const auto wrong_url = fixture.codec.Encode(wrong_op, base);
  REQUIRE(wrong_url.ok());
  const auto mismatch = HttpDo(port, "GET", TargetOf(wrong_url.value()), Authed());
  REQUIRE(mismatch.status == 403);

  //  过期：exp 已过 → 401（签名对、时间不对）
  fss::domain::TransferToken expired = ghost;
  expired.expires_at_epoch_seconds = fixture.clock.NowEpochSeconds() - 1;
  const auto expired_url = fixture.codec.Encode(expired, base);
  REQUIRE(expired_url.ok());
  const auto late = HttpDo(port, "GET", TargetOf(expired_url.value()), Authed());
  REQUIRE(late.status == 401);

  //  签名篡改：改一位 sig → 401
  std::string tampered = TargetOf(ghost_url.value());
  const auto sig_pos = tampered.find("&sig=");
  REQUIRE(sig_pos != std::string::npos);
  tampered[sig_pos + 6] = tampered[sig_pos + 6] == 'A' ? 'B' : 'A';
  const auto forged = HttpDo(port, "GET", tampered, Authed());
  REQUIRE(forged.status == 401);
}

TEST_CASE("★ C4.8 真实栈（POSIX + SQLite）：同一套链路在真实文件系统与真实仓储上跑通",
          "[phase4][integration][c4.8]") {
  //  与上面两条的区别：适配器换成 `PosixBlobStore` + `SqliteLocationRepository`。
  //  ★ 这条用例的由来：5 分钟长跑实测里 `POST metadata` 返回 **500**
  //    （"位置记录缺少物理引用"）—— SQLite 仓储把 `extra.container/object_key`
  //    当成已知键吞掉了（P4-D09）。内存适配器永远发现不了它。
  fss::test::PosixStackFixture fixture;
  const int port = fixture.port();

  const std::string payload = "posix-stack-payload-0123456789";
  const auto upload = HttpDo(port, "GET", "/api/file/v2/files/uploadURL", Authed());
  REQUIRE(upload.status == 200);
  const auto upload_json = fss::json::ParseObject(upload.body);
  REQUIRE(upload_json.ok());
  const std::string file_id = upload_json.value()["FileID"].get<std::string>();
  const std::string file_source =
      upload_json.value()["Location"]["FileSource"].get<std::string>();
  const std::string put_url = upload_json.value()["Location"]["SignedURL"].get<std::string>();

  REQUIRE(HttpDo(port, "PUT", TargetOf(put_url), Authed(), payload).status == 200);

  auto record = fss::test::AppFixture::MakeRecord(file_source, "posix-stack.bin");
  const auto created = HttpDo(port, "POST", "/api/file/v2/files/metadata", Authed(),
                              fss::json::Dump(fss::domain::ToJson(record)));
  INFO("metadata 响应: " << created.body);
  REQUIRE(created.status == 201);  // ★ 曾经是 500（P4-D09）

  //  服务端覆写的校验和必须等于真实字节的 SHA-256（证明复制到的确实是刚上传的内容）
  const auto created_json = fss::json::ParseObject(created.body);
  REQUIRE(created_json.ok());
  const std::string record_id = created_json.value()["id"].get<std::string>();
  const auto fetched = HttpDo(port, "GET", "/api/file/v2/files/" + record_id + "/metadata",
                              Authed());
  REQUIRE(fetched.status == 200);
  const auto fetched_json = fss::json::ParseObject(fetched.body);
  REQUIRE(fetched_json.value()["data"]["Checksum"].get<std::string>() ==
          fss::crypto::Sha256Hex(payload));

  const auto download =
      HttpDo(port, "GET", "/api/file/v2/files/" + file_id + "/downloadURL", Authed());
  REQUIRE(download.status == 200);
  const auto download_json = fss::json::ParseObject(download.body);
  const auto got = HttpDo(port, "GET",
                          TargetOf(download_json.value()["SignedUrl"].get<std::string>()),
                          Authed());
  REQUIRE(got.status == 200);
  REQUIRE(got.body == payload);
}
