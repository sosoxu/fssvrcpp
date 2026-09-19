// =============================================================================
//  test_posix_tmp_names.cpp —— C6.13：临时文件名唯一性（复现 ADR-009 M1）
// =============================================================================
//  风险 M1（ADR-009 §3 / AGENTS §4.4）：临时文件名不含**实例标识**时，两个实例
//  往同一个业务序号（同一个对象 key）写数据会**静默串数据** —— P3 的探针实测 40 次里
//  有 21 次内容错乱。
//
//  本实现的临时名 = `<目标路径>.tmp.<instance_id>.<pid>.<进程级序号>`：
//    · `instance_id` 区分**跨实例**（共享存储上的两个进程）；
//    · `pid` 区分**同实例的不同进程**（重启/多进程）；
//    · **进程级**序号区分**同一进程内的多次写入**（含同一进程里装配出来的两个 store 对象）。
//  三者缺一就会出现"两个写入者算出同一个临时路径"。
//
//  ★ 判据要求两条：
//    ① 正例：2 个"实例"并发写同一业务序号 → **无内容错乱**（结果必是其中一份**完整**数据）；
//    ② 对照：把临时名换成"不含实例标识"的朴素写法 → **必须**能复现错乱（证明检测方法有效）。
//       对照用测试内自带的朴素写入器（固定临时名 + 不用 O_EXCL + 故意交错写），
//       不依赖产品代码里的开关（R1：对照要能复现问题本身）。
// =============================================================================
#include <catch2/catch.hpp>

#include "temp_dir.h"

#include "common/crypto/crypto.h"
#include "infra/blob/posix/posix_blob_store.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using fss::domain::ObjectRef;
using fss::infra::PosixBlobStore;
using fss::infra::PosixBlobStoreOptions;

ObjectRef Ref(const std::string& key) {
  ObjectRef ref;
  ref.container = "opendes-staging";
  ref.key = key;
  return ref;
}

//  一份"可辨别"的载荷：整体重复同一个字节，但**头尾**带上自己的标记，
//  这样"两份数据混在一起"能被一眼看出来（长度相同、内容不同）。
std::string PayloadOf(char marker, std::size_t size) {
  std::string out(size, marker);
  const std::string head = std::string("HEAD-") + marker + "-";
  const std::string tail = std::string("-TAIL-") + marker;
  out.replace(0, head.size(), head);
  out.replace(out.size() - tail.size(), tail.size(), tail);
  return out;
}

bool IsExactlyOneOf(const std::string& content, const std::string& a, const std::string& b) {
  return content == a || content == b;
}

}  // namespace

TEST_CASE("★ C6.13 两个实例并发写同一业务序号：结果必是**一份完整数据**（不是两份混在一起）",
          "[phase6][integration][c6.13]") {
  fss::test::TempDir dir("tmp_names");
  const std::string root = dir.child("blobs");
  fss::SystemClock clock;

  //  ★ 两个 store 对象**故意共用同一个 instance_id**：这正是"同进程里装配了两次"的场景。
  //    若临时序号是"每个 store 各自从 0 开始"，两者会算出同一个临时路径 → 至少一个 put 失败。
  PosixBlobStoreOptions options;
  options.instance_id = "same-instance";
  options.fsync_policy = fss::infra::FsyncPolicy::kNever;
  PosixBlobStore first(root, clock, options);
  PosixBlobStore second(root, clock, options);
  REQUIRE(first.ensure_container("opendes-staging").ok());
  REQUIRE(second.ensure_container("opendes-staging").ok());

  const std::string payload_a = PayloadOf('A', 4 * 1024 * 1024);
  const std::string payload_b = PayloadOf('B', 4 * 1024 * 1024);
  const auto ref = Ref("osdu-user/seq-42/file");

  //  并发写同一个 key，反复多轮（单轮"没串"可能只是运气）
  for (int round = 0; round < 12; ++round) {
    std::atomic<int> failures{0};
    std::string errors;
    std::mutex errors_mutex;
    const auto put = [&](PosixBlobStore& store, const std::string& payload) {
      fss::bytes::StringSource source(payload);
      const auto result = store.put(ref, source, fss::domain::PutOptions{});
      if (!result.ok()) {
        failures.fetch_add(1);
        std::lock_guard<std::mutex> lock(errors_mutex);
        errors += result.error().ToString() + " ";
      }
    };

    std::thread writer_a(put, std::ref(first), std::cref(payload_a));
    std::thread writer_b(put, std::ref(second), std::cref(payload_b));
    writer_a.join();
    writer_b.join();

    INFO("第 " << round << " 轮，put 失败数 " << failures.load() << " 错误：" << errors);
    //  ★ 两个写入者都必须成功：临时路径唯一 → 谁也不该踩到谁
    REQUIRE(failures.load() == 0);

    //  ★ 关键断言：最终对象**恰好等于其中一份完整数据**（长度相同、标记在头尾，
    //    混写必然既不是 A 也不是 B）
    fss::bytes::StringSink sink;
    REQUIRE(first.get(ref, sink, fss::domain::ByteRange{}).ok());
    const std::string content = sink.str();
    INFO("第 " << round << " 轮，大小 " << content.size() << " 首尾 "
               << content.substr(0, 7) << " … " << content.substr(content.size() - 7));
    REQUIRE(content.size() == payload_a.size());
    REQUIRE(IsExactlyOneOf(content, payload_a, payload_b));
  }
}

TEST_CASE("★ C6.13 对照：临时名不含实例标识时**必然**出现内容错乱（证明检测方法有效）",
          "[phase6][integration][c6.13]") {
  //  ★ 这是一个**故意写错的**朴素实现：两个写入者共用同一个临时路径
  //    （不含 instance_id/pid/序号），各自写自己那份数据的**一段**。
  //    它证明"结果 == 其中一份完整数据"这条断言**能失败** —— 否则正例里的"无错乱"
  //    分不清是"实现正确"还是"检测方法根本不看内容"。
  fss::test::TempDir dir("tmp_names_control");
  const std::string target = dir.child("object.bin");
  const std::string tmp = target + ".tmp";  // ← 朴素：不含实例标识

  const std::string payload_a = PayloadOf('A', 4 * 1024 * 1024);
  const std::string payload_b = PayloadOf('B', 4 * 1024 * 1024);
  const std::size_t half = payload_a.size() / 2;

  //  ★ 用两个栅栏把"交错"**固定下来**（否则测试本身就是 flaky 的）：
  //    A 先写前半段 → B 再写后半段。最终文件 = A 的头 + B 的尾，
  //    长度与一份正常对象相同、内容却不是任何一份 —— 正是"静默串数据"的样子。
  std::atomic<int> head_written{0};
  std::atomic<int> tail_written{0};
  const auto write_head = [&]() {
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT, 0644);
    REQUIRE(fd >= 0);
    std::size_t written = 0;
    while (written < half) {
      const ssize_t n = ::pwrite(fd, payload_a.data() + written, half - written,
                                 static_cast<off_t>(written));
      REQUIRE(n > 0);
      written += static_cast<std::size_t>(n);
    }
    head_written.store(1);
    while (tail_written.load() == 0) std::this_thread::yield();
    ::close(fd);
  };
  const auto write_tail = [&]() {
    while (head_written.load() == 0) std::this_thread::yield();
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT, 0644);
    REQUIRE(fd >= 0);
    std::size_t written = 0;
    while (written < payload_b.size() - half) {
      const ssize_t n = ::pwrite(fd, payload_b.data() + half + written,
                                 payload_b.size() - half - written,
                                 static_cast<off_t>(half + written));
      REQUIRE(n > 0);
      written += static_cast<std::size_t>(n);
    }
    tail_written.store(1);
    ::close(fd);
  };

  std::thread writer_a(write_head);
  std::thread writer_b(write_tail);
  writer_a.join();
  writer_b.join();
  REQUIRE(::rename(tmp.c_str(), target.c_str()) == 0);

  std::ifstream input(target, std::ios::binary);
  REQUIRE(input.good());
  const std::string content((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
  INFO("朴素实现的产物：大小 " << content.size() << " 首尾 " << content.substr(0, 7) << " … "
                             << content.substr(content.size() - 7));
  //  ★ 必须是 A 的头 + B 的尾：既不是 A 也不是 B
  REQUIRE(content.size() == payload_a.size());
  REQUIRE_FALSE(IsExactlyOneOf(content, payload_a, payload_b));
  REQUIRE(content.substr(0, 7) == payload_a.substr(0, 7));
  REQUIRE(content.substr(content.size() - 7) == payload_b.substr(payload_b.size() - 7));
  //  且它的**大小**与一份正常对象完全一致 → 只按大小自检会漏掉；
  //  只有校验和（或逐字节比对）能发现 —— 这正是"静默串数据"的含义
  REQUIRE(content.size() == payload_a.size());
  REQUIRE(fss::crypto::Sha256Hex(content) != fss::crypto::Sha256Hex(payload_a));
  REQUIRE(fss::crypto::Sha256Hex(content) != fss::crypto::Sha256Hex(payload_b));
}

// =============================================================================
//  ★ B1 补齐 ADR-009 §4.5 的**随机后缀**（M1 的关键一步）
// =============================================================================
//  为什么单靠 `instance_id` + `pid` + 进程内计数不够：默认 `deployment.instance_id`
//  是 `local`（组合根历史默认），共享挂载上两台主机可以有**相同**的 instance_id、
//  **相同**的 pid、**相同**的计数起点 → 临时名确定性撞车（ADR-009 §3 M1：40 次里
//  21 次静默串数据）。因此名字里必须再有一个**构造时生成一次**的随机 token。
//
//  判据（都能失败，R1）：
//    ① 同一 target 连续两次取路径必不同（计数维度）；
//    ② 两个**相同 instance_id** 的 store 取同一 target 必不同，且各自的随机 token 不同；
//    ③ 路径里**真的含**该 store 的随机 token（去掉随机后缀 → 这条必失败）。
// =============================================================================
TEST_CASE("★ C6.13/B1 临时名含随机后缀（ADR-009 §4.5）：同 target 必不同、同 id 不撞名",
          "[phase6][integration][c6.13]") {
  fss::test::TempDir dir("tmp_names_random");
  fss::SystemClock clock;
  //  ★ 故意共用同一个 instance_id：这正是"跨主机默认 local"的最坏情况。
  PosixBlobStoreOptions options;
  options.instance_id = "local";
  PosixBlobStore first(dir.child("blobs"), clock, options);
  PosixBlobStore second(dir.child("blobs"), clock, options);

  const std::string target = dir.child("blobs/opendes-staging/osdu-user/x/object.bin");

  //  ①② 随机 token 存在且两个 store 不同（每个 store 构造时生成一次）
  const std::string token_first = first.temp_name_token();
  const std::string token_second = second.temp_name_token();
  INFO("token_first=" << token_first << " token_second=" << token_second);
  REQUIRE_FALSE(token_first.empty());
  REQUIRE_FALSE(token_second.empty());
  REQUIRE(token_first != token_second);

  //  ③ 路径里真的含该 store 的随机 token（去掉随机成分 → 这里必然失败）
  const std::string path_first = first.TempPathForDiagnostics(target);
  const std::string path_second = second.TempPathForDiagnostics(target);
  INFO("path_first=" << path_first << " path_second=" << path_second);
  REQUIRE(path_first.find(token_first) != std::string::npos);
  REQUIRE(path_second.find(token_second) != std::string::npos);
  //  既有形态不变（`.tmp.` 前缀 + instance_id + pid）—— C9.25 的清理判据依赖 `.tmp.`
  REQUIRE(path_first.find(".tmp.local.") != std::string::npos);
  REQUIRE(path_first.compare(0, target.size(), target) == 0);  // 目标路径在最前

  //  ① 同一 store 对同一 target 连续取两次必不同（进程级计数）
  const std::string path_first_again = first.TempPathForDiagnostics(target);
  REQUIRE(path_first != path_first_again);
  REQUIRE(path_first_again.find(token_first) != std::string::npos);

  //  ② 两个相同 instance_id 的 store 对同一 target 不撞名
  REQUIRE(path_first != path_second);
}
