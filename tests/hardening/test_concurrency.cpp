// =============================================================================
//  P9 硬化：并发（C9.1）—— 100 并发 JSON 零 5xx；8 并发大文件校验和一致
// =============================================================================
//  判据
//    · 100 并发 JSON 请求 → **零 5xx**（除注入的故障）
//    · 数据面 8 并发大文件 → 每个文件的 SHA-256 与客户端算出的**完全一致**
//
//  为什么这一条必须在**真实端口 + 真实线程**上做：
//    并发缺陷（背压失效、连接数上限算错、计数不归还、共享缓冲串数据）只在
//    "多线程同时打真实 socket"时出现。P1 的并发用例证明了传输层；这一条证明的是
//    **整条业务链路**（路由 → 鉴权 → 用例 → 仓储 → 存储）在并发下仍然正确。
//
//  数据面用**真实 POSIX 存储**（`PosixStackFixture`）：内存适配器会把对象整体放进
//  RAM，8 并发 × 大文件会变成"测内存带宽"，与生产形态无关。
// =============================================================================
#include <catch2/catch.hpp>

#include "big_file.h"
#include "http_fixture.h"
#include "raw_http.h"

#include "common/crypto/crypto.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using fss::test::Authed;
using fss::test::BigFileBytes;
using fss::test::PosixStackFixture;
using fss::test::RawClient;
using fss::test::TargetOf;

//  数据面并发用例的对象大小：默认 8 MiB × 8 并发 = 64 MiB（秒级）；
//  sanitizer 构建下随 `FSS_TEST_BIG_BYTES` 等比例缩小，避免影子内存把用例拖垮。
std::int64_t DataPlaneBytes() {
  const auto scaled = BigFileBytes() / 128;  // 1 GiB → 8 MiB
  return scaled > 0 ? scaled : 1;
}

struct RestReply {
  int status = 0;
  std::string body;
};

RestReply RestGet(int port, const std::string& target) {
  RestReply reply;
  RawClient client(port, /*tcp_nodelay=*/true);
  if (!client.Connect()) return reply;
  if (!client.SendRequest("GET", target, Authed(), "")) return reply;
  const auto response = client.ReadResponse(30000);
  if (!response.has_value()) return reply;
  reply.status = response->status;
  reply.body = response->body;
  return reply;
}

}  // namespace

TEST_CASE("★ C9.1 100 并发 JSON 请求：零 5xx（真实端口 + 真实线程）",
          "[phase9][hardening][c9.1]") {
  //  线程池 = 并发连接上限的硬上限（C1.12）：要真的"100 个同时在途"，
  //  必须把两者都配到 100 以上，否则测的是 503 拒绝而不是并发正确性。
  fss::http::ServerOptions server_options;
  server_options.worker_threads = 128;
  server_options.max_connections = 128;
  fss::adapters::http::RouterOptions router_options;
  fss::test::HttpFixture fx(router_options, server_options);
  const int port = fx.port();

  constexpr int kConcurrency = 100;
  constexpr int kRoundsPerThread = 5;
  std::atomic<int> ok{0};
  std::atomic<int> server_errors{0};
  std::atomic<int> unexpected{0};
  std::mutex message_mutex;
  std::vector<std::string> messages;
  const auto note = [&](const std::string& message) {
    std::lock_guard<std::mutex> guard(message_mutex);
    messages.push_back(message);
  };

  std::vector<std::thread> threads;
  threads.reserve(kConcurrency);
  for (int worker = 0; worker < kConcurrency; ++worker) {
    threads.emplace_back([&, worker] {
      for (int round = 0; round < kRoundsPerThread; ++round) {
        //  交错打三类 JSON 端点：探活（免鉴权）、上传地址（写位置记录）、列表（读仓储）
        const int kind = (worker + round) % 3;
        if (kind == 0) {
          const auto reply = RestGet(port, "/api/file/v2/info");
          if (reply.status >= 500) {
            ++server_errors;
            note("info 5xx: " + std::to_string(reply.status));
          } else if (reply.status == 200) {
            ++ok;
          } else {
            ++unexpected;
            note("info 非 200: " + std::to_string(reply.status));
          }
          continue;
        }
        if (kind == 1) {
          const auto reply = RestGet(port, "/api/file/v2/files/uploadURL");
          if (reply.status >= 500) {
            ++server_errors;
            note("uploadURL 5xx: " + std::to_string(reply.status) + " " + reply.body);
          } else if (reply.status == 200) {
            ++ok;
          } else {
            ++unexpected;
            note("uploadURL 非 200: " + std::to_string(reply.status) + " " + reply.body);
          }
          continue;
        }
        //  getFileList：并发下"没有记录"是 400（契约 §2.5），有记录是 200 —— 两者都算正常
        RawClient client(port, true);
        if (!client.Connect()) {
          ++unexpected;
          note("列表：连接失败");
          continue;
        }
        const std::string body = R"({"Items": 10, "PageNum": 0})";
        //  ★ `RawClient` **不补** `Content-Length`（它的价值是能构造畸形请求）：
        //    带体请求必须自己补，否则服务端按"读到连接关闭"等体、客户端等响应 → 双等（P4 的教训）。
        std::vector<std::string> headers = Authed();
        headers.push_back("content-type: application/json");
        headers.push_back("Content-Length: " + std::to_string(body.size()));
        if (!client.SendRequest("POST", "/api/file/v2/getFileList", headers, body)) {
          ++unexpected;
          note("列表：发送失败");
          continue;
        }
        const auto response = client.ReadResponse(30000);
        if (!response.has_value()) {
          ++unexpected;
          note("列表：读响应失败");
          continue;
        }
        if (response->status >= 500) {
          ++server_errors;
          note("列表 5xx: " + std::to_string(response->status) + " " + response->body);
        } else if (response->status == 200 || response->status == 400) {
          ++ok;
        } else {
          ++unexpected;
          note("列表非 200/400: " + std::to_string(response->status));
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();

  {
    std::lock_guard<std::mutex> guard(message_mutex);
    for (const auto& message : messages) INFO("失败样本：" << message);
  }
  CAPTURE(kConcurrency, kRoundsPerThread, ok.load(), server_errors.load(), unexpected.load());
  REQUIRE(server_errors.load() == 0);   // ★ 判据的核心：零 5xx
  REQUIRE(unexpected.load() == 0);      // 也不接受"意外的 4xx/连接失败"
  REQUIRE(ok.load() == kConcurrency * kRoundsPerThread);
}

TEST_CASE("★ C9.1 数据面 8 并发大文件：每一路的内容与 SHA-256 都必须属于自己",
          "[phase9][hardening][c9.1]") {
  PosixStackFixture fx;
  const int port = fx.port();
  const std::int64_t bytes = DataPlaneBytes();
  constexpr int kConcurrency = 8;
  INFO("对象大小 " << bytes << " 字节 × " << kConcurrency << " 并发");

  //  每一路的载荷 = "slot-<i>:" + 重复模式（其余部分相同）→ "串数据"必然被逐路比对抓到
  const auto payload_of = [&](int slot) {
    const std::string prefix = "slot-" + std::to_string(slot) + ":";
    fss::bytes::RepeatingSource source(bytes - static_cast<std::int64_t>(prefix.size()));
    std::string body = prefix;
    std::vector<char> buffer(256 * 1024);
    while (true) {
      const auto read = source.Read(buffer.data(), buffer.size());
      if (!read.ok() || read.value() == 0) break;
      body.append(buffer.data(), read.value());
    }
    return body;
  };
  //  期望值用**流式**算法算出（与下载侧的比对方式一致，且不额外驻留一份对象）
  const auto sha_of = [&](int slot) {
    const std::string prefix = "slot-" + std::to_string(slot) + ":";
    fss::crypto::Hasher hasher(fss::crypto::ChecksumAlgorithm::kSha256);
    hasher.Update(prefix);
    fss::bytes::RepeatingSource source(bytes - static_cast<std::int64_t>(prefix.size()));
    std::vector<char> buffer(256 * 1024);
    while (true) {
      const auto read = source.Read(buffer.data(), buffer.size());
      if (!read.ok() || read.value() == 0) break;
      hasher.Update(std::string_view(buffer.data(), read.value()));
    }
    return hasher.HexDigest();
  };

  struct Slot {
    std::string file_id;
    std::string upload_url;
  };
  std::vector<Slot> slots;
  for (int i = 0; i < kConcurrency; ++i) {
    const auto upload = RestGet(port, "/api/file/v2/files/uploadURL");
    REQUIRE(upload.status == 200);
    const auto json = fss::json::ParseObject(upload.body);
    REQUIRE(json.ok());
    slots.push_back(Slot{json.value()["FileID"].get<std::string>(),
                         json.value()["Location"]["SignedURL"].get<std::string>()});
  }

  //  ---- ① 8 路并发 PUT（真实回环 socket + 自签 token + POSIX 落盘）----
  std::atomic<int> uploaded{0};
  std::atomic<int> failed{0};
  {
    std::vector<std::thread> putters;
    for (int i = 0; i < kConcurrency; ++i) {
      putters.emplace_back([&, i] {
        const std::string body = payload_of(i);
        RawClient client(port, true);
        if (!client.Connect() ||
            !client.SendRequest("PUT", TargetOf(slots[i].upload_url),
                                {"Content-Length: " + std::to_string(body.size()),
                                 "data-partition-id: opendes"},
                                body)) {
          ++failed;
          return;
        }
        const auto response = client.ReadResponse(120000);
        if (!response.has_value() || response->status != 200) {
          ++failed;
          return;
        }
        ++uploaded;
      });
    }
    for (auto& thread : putters) thread.join();
  }
  REQUIRE(failed.load() == 0);
  REQUIRE(uploaded.load() == kConcurrency);

  //  ---- ② 8 路并发 GET：每一路都必须收到**自己**那份字节 ----
  std::atomic<int> verified{0};
  std::atomic<int> mismatched{0};
  std::mutex mismatch_mutex;
  std::vector<std::string> mismatch_messages;
  {
    std::vector<std::thread> getters;
    for (int i = 0; i < kConcurrency; ++i) {
      getters.emplace_back([&, i] {
        const auto url = RestGet(port, "/api/file/v2/files/" + slots[i].file_id + "/downloadURL");
        if (url.status != 200) {
          ++mismatched;
          std::lock_guard<std::mutex> guard(mismatch_mutex);
          mismatch_messages.push_back("slot " + std::to_string(i) + " downloadURL " +
                                      std::to_string(url.status));
          return;
        }
        const auto json = fss::json::ParseObject(url.body);
        if (!json.ok()) {
          ++mismatched;
          return;
        }
        RawClient client(port, true);
        if (!client.Connect() ||
            !client.SendRequest("GET", TargetOf(json.value()["SignedUrl"].get<std::string>()),
                                {"data-partition-id: opendes"}, "")) {
          ++mismatched;
          return;
        }
        const auto response = client.ReadResponse(120000);
        if (!response.has_value() || response->status != 200) {
          ++mismatched;
          std::lock_guard<std::mutex> guard(mismatch_mutex);
          mismatch_messages.push_back("slot " + std::to_string(i) + " GET " +
                                      std::to_string(response.has_value() ? response->status : -1));
          return;
        }
        const std::string actual = fss::crypto::Sha256Hex(response->body);
        if (response->body.size() != static_cast<std::size_t>(bytes) || actual != sha_of(i)) {
          ++mismatched;
          std::lock_guard<std::mutex> guard(mismatch_mutex);
          mismatch_messages.push_back("slot " + std::to_string(i) + " 内容/摘要不符（收到 " +
                                      std::to_string(response->body.size()) + " 字节）");
          return;
        }
        ++verified;
      });
    }
    for (auto& thread : getters) thread.join();
  }
  for (const auto& message : mismatch_messages) INFO("不匹配：" << message);
  CAPTURE(kConcurrency, bytes, verified.load(), mismatched.load());
  REQUIRE(mismatched.load() == 0);
  REQUIRE(verified.load() == kConcurrency);

  //  ---- ③ 存储侧的独立对账：每个对象的大小与 checksum 都必须正确 ----
  for (int i = 0; i < kConcurrency; ++i) {
    const auto location = fx.locations->Find("opendes", slots[i].file_id);
    REQUIRE(location.ok());
    const auto ref = fss::app::ObjectRefFromLocation(location.value());
    REQUIRE(ref.ok());
    const auto stat = fx.blob.stat(ref.value());
    REQUIRE(stat.ok());
    REQUIRE(stat.value().exists);
    REQUIRE(stat.value().size == bytes);
    //  ★ POSIX 驱动在写入过程中增量算出 SHA-256（侧车文件）——与该路的期望值一致，
    //    说明"边收边算"在多并发下也没有串数据
    REQUIRE(stat.value().checksum == sha_of(i));
  }
}
