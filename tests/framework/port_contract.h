// =============================================================================
//  tests/framework/port_contract.h —— 端口契约测试基类（门槛 C2.10）
// =============================================================================
//
//  为什么需要"契约测试基类"
//  ---------------------------------------------------------------------------
//  同一个端口会有多个实现（`IBlobStore`：memory / posix / s3；两个仓储：
//  memory / sqlite / postgres / remote）。如果每个实现各写一套测试，"语义一致"
//  就只是口头承诺：A 实现里 `remove` 幂等、B 实现里报 404，上层代码就会写出
//  只在某个后端正确的分支 —— 这正是 ADR-003 要消除的"按驱动分支编程"。
//
//  所以把**语义**写成一份可复用的断言集：以后每个新实现只需要
//      TEST_CASE("... 契约：<实现名>") { <实现> impl; CheckXxxContract(impl); }
//  就能证明它满足同一份契约（C2.10 要求 sqlite/postgres 共用同一套测试，
//  这里是那套测试的第一批使用者：内存实现）。
//
//  ★ 设计约束
//    ① 只用端口公开的接口 —— 不得包含任何实现的头文件（否则契约就会退化成
//       "针对某个实现写的测试"）。
//    ② 能力门控：`native_presign` / `range_read` / `server_side_copy` 为 false 时，
//       对应操作**必须**报 `kUnimplemented`（而不是"碰巧不支持"或返回垃圾）。
//       为 true 时必须真的可用。这样"声明能力"与"实际行为"被同时钉住。
//    ③ 关键语义都配**正例**（AGENTS.md R16）：例如 expected_size 不符要失败，
//       相符必须成功；否则无法区分"校验正确"与"校验恒真"。
//    ④ 断言里的"期望值"来自 docs/03-api-contract.md 与 ADR-003/009/010 的结论，
//       不是"看起来合理"。
//
//  语义登记表（实现方必须遵守；有争议时以本表 + 证据文件为准）
//  ---------------------------------------------------------------------------
//    stat(缺失对象)          → Ok 且 exists == false（`ObjectStat::exists` 就是为此存在）
//    get(缺失对象)           → kNotFound
//    remove(缺失对象)        → Ok（对象存储的删除天然幂等）
//    list(缺失容器)          → kNotFound；list(空容器) → Ok + 空页
//    put(缺失容器)           → kNotFound（调用方必须先 ensure_container）
//    get 区间 offset >= size → kInvalidArgument（对齐 S3 的 416 语义）
//    get 区间超出末尾        → 截断到对象末尾（对齐 RFC 7233 §2.1）
//    copy(缺失源)            → kNotFound；copy 覆盖已存在的目标
//    presign（无原生能力）   → kUnimplemented（ADR-003 §1）
//    put 的 expected_size 不符 → kInvalidArgument（契约 §1.7 的"防静默截断"）
//    put 的 expected_checksum 不符 → kChecksumMismatch + details{expected,actual}（契约 §5）
//    Save(位置记录)          → 按 file_id **upsert**；(partition,file_source) 唯一（R5）
//    Create(元数据)          → 按 (partition,file_source) **幂等**，重复创建返回同一条（R5）
//    Update(元数据)          → version+1 的版本链，latest 切换（R6）
//    Delete(缺失记录)        → kNotFound
// =============================================================================
#pragma once

#include <catch2/catch.hpp>

#include "common/bytes/bytes.h"
#include "common/crypto/crypto.h"
#include "common/result/result.h"
#include "common/time/clock.h"
#include "domain/model/file_metadata.h"
#include "domain/model/types.h"
#include "app/services/location_issuer.h"
#include "domain/ports/ports.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace fss::test {

// =============================================================================
//  断言辅助
// =============================================================================
//  INFO 必须写在**函数作用域**（不能只写在 else 分支里）—— Catch2 的 INFO 是
//  RAII 作用域消息，出了分支就销毁，失败信息会丢。
inline void ContractOk(const fss::Result<void>& r, std::string_view what) {
  INFO("契约点：" << what);
  if (!r.ok()) {
    INFO("实际错误：" << fss::ErrorKindName(r.error().kind()) << " / " << r.error().message());
  }
  REQUIRE(r.ok());
}

template <typename T, typename = std::enable_if_t<!std::is_void_v<T>>>
T ContractOk(fss::Result<T>&& r, std::string_view what) {
  INFO("契约点：" << what);
  if (!r.ok()) {
    INFO("实际错误：" << fss::ErrorKindName(r.error().kind()) << " / " << r.error().message());
  }
  REQUIRE(r.ok());
  return std::move(r).value();
}

inline void ContractError(const fss::Result<void>& r, fss::ErrorKind expected,
                          std::string_view what) {
  INFO("契约点（期望失败 " << fss::ErrorKindName(expected) << "）：" << what);
  if (r.ok()) {
    INFO("实际：调用成功了（本应失败）");
  }
  REQUIRE_FALSE(r.ok());
  INFO("实际错误：" << fss::ErrorKindName(r.error().kind()) << " / " << r.error().message());
  REQUIRE(r.error().kind() == expected);
}

template <typename T, typename = std::enable_if_t<!std::is_void_v<T>>>
void ContractError(const fss::Result<T>& r, fss::ErrorKind expected, std::string_view what) {
  INFO("契约点（期望失败 " << fss::ErrorKindName(expected) << "）：" << what);
  if (r.ok()) {
    INFO("实际：调用成功了（本应失败）");
  }
  REQUIRE_FALSE(r.ok());
  INFO("实际错误：" << fss::ErrorKindName(r.error().kind()) << " / " << r.error().message());
  REQUIRE(r.error().kind() == expected);
}

inline domain::ObjectRef MakeRef(const std::string& container, const std::string& key) {
  domain::ObjectRef ref;
  ref.container = container;
  ref.key = key;
  return ref;
}

// =============================================================================
//  一、IBlobStore 契约（内存 / POSIX / S3 共用）
// =============================================================================
inline void CheckBlobStoreContract(domain::IBlobStore& store,
                                   const std::string& container = "contract-blob") {
  using domain::ByteRange;
  using domain::ObjectRef;
  using domain::PutOptions;
  using bytes::StringSink;
  using bytes::StringSource;

  SECTION("capabilities：只读声明，driver_name 非空且稳定") {
    const auto c1 = store.capabilities();
    const auto c2 = store.capabilities();
    REQUIRE_FALSE(c1.driver_name.empty());
    REQUIRE(c1.driver_name == c2.driver_name);
    REQUIRE(c1.native_presign == c2.native_presign);
    REQUIRE(c1.range_read == c2.range_read);
    REQUIRE(c1.server_side_copy == c2.server_side_copy);
  }

  SECTION("ensure_container：幂等（重复调用必须成功）") {
    ContractOk(store.ensure_container(container), "首次 ensure_container");
    ContractOk(store.ensure_container(container), "重复 ensure_container（必须幂等）");
  }

  SECTION("presign：能力门控（true → 原生 URL；false → kUnimplemented）") {
    const auto caps = store.capabilities();
    ContractOk(store.ensure_container(container), "ensure_container");
    const auto ref = MakeRef(container, "presign/obj.bin");

    domain::PresignOptions put_opts;
    put_opts.method = "PUT";
    put_opts.content_type = "application/octet-stream";
    put_opts.expires_in_seconds = 600;
    domain::PresignOptions get_opts = put_opts;
    get_opts.method = "GET";

    if (caps.native_presign) {
      const auto put = ContractOk(store.presign_put(ref, put_opts),
                                  "native_presign=true 时 presign_put 必须真的可用");
      REQUIRE_FALSE(put.url.empty());
      REQUIRE(put.native);
      REQUIRE(put.method == "PUT");
      const auto get = ContractOk(store.presign_get(ref, get_opts),
                                  "native_presign=true 时 presign_get 必须真的可用");
      REQUIRE_FALSE(get.url.empty());
      REQUIRE(get.method == "GET");
    } else {
      // ★ 反面同样被钉住：没有原生能力就不能"悄悄返回一个假 URL"
      ContractError(store.presign_put(ref, put_opts), fss::ErrorKind::kUnimplemented,
                    "native_presign=false 时 presign_put 必须报 kUnimplemented");
      ContractError(store.presign_get(ref, get_opts), fss::ErrorKind::kUnimplemented,
                    "native_presign=false 时 presign_get 必须报 kUnimplemented");
    }
  }

  SECTION("put/get：往返一致 + 空对象（正例对照）") {
    ContractOk(store.ensure_container(container), "ensure_container");
    const auto ref = MakeRef(container, "basics/hello.txt");
    StringSource src("hello world");
    PutOptions opts;
    opts.content_type = "text/plain";
    ContractOk(store.put(ref, src, opts), "put 正常对象");

    StringSink sink;
    ContractOk(store.get(ref, sink, ByteRange{}), "get 全量");
    REQUIRE(sink.str() == "hello world");

    // R16：只有"拒绝"用例无法证明实现正确 —— 空对象必须能存能取
    const auto empty_ref = MakeRef(container, "basics/empty.bin");
    StringSource empty("");
    ContractOk(store.put(empty_ref, empty, PutOptions{}), "put 空对象");
    StringSink empty_sink;
    ContractOk(store.get(empty_ref, empty_sink, ByteRange{}), "get 空对象");
    REQUIRE(empty_sink.str().empty());
  }

  SECTION("put：容器不存在 → kNotFound；空 container/key → kInvalidArgument") {
    ContractOk(store.ensure_container(container), "ensure_container");
    StringSource src("x");
    ContractError(store.put(MakeRef("no-such-container-xyz", "x"), src, PutOptions{}),
                  fss::ErrorKind::kNotFound, "put 到不存在的容器必须失败（先 ensure_container）");
    StringSource src2("x");
    ContractError(store.put(MakeRef(container, ""), src2, PutOptions{}),
                  fss::ErrorKind::kInvalidArgument, "空 key 必须被拒");
    ContractError(store.ensure_container(""), fss::ErrorKind::kInvalidArgument,
                  "空容器名必须被拒");
  }

  SECTION("put：expected_size 校验（不符 → kInvalidArgument，相符 → 必须成功）") {
    ContractOk(store.ensure_container(container), "ensure_container");
    const auto ref = MakeRef(container, "size/obj.bin");

    StringSource good("0123456789");  // 10 字节
    PutOptions ok_opts;
    ok_opts.expected_size = 10;
    ContractOk(store.put(ref, good, ok_opts), "expected_size 相符时必须成功（正例对照）");

    StringSource short_src("0123456789");
    PutOptions bad_opts;
    bad_opts.expected_size = 11;  // 承诺 11，实际 10 —— 静默截断的检测点
    ContractError(store.put(ref, short_src, bad_opts), fss::ErrorKind::kInvalidArgument,
                  "expected_size 与实际不符必须报 kInvalidArgument");
  }

  SECTION("put：expected_checksum 校验（正确 → 成功；错误 → kChecksumMismatch + details）") {
    ContractOk(store.ensure_container(container), "ensure_container");
    const auto ref = MakeRef(container, "checksum/obj.bin");
    const std::string payload = "checksum-me";

    StringSource good(payload);
    PutOptions ok_opts;
    ok_opts.expected_checksum = crypto::Sha256Hex(payload);
    ok_opts.checksum_algorithm = "SHA256";
    ContractOk(store.put(ref, good, ok_opts), "校验和正确时必须成功（正例对照）");

    StringSource bad(payload);
    PutOptions bad_opts;
    bad_opts.expected_checksum = crypto::Sha256Hex("something else");
    bad_opts.checksum_algorithm = "SHA256";
    const auto r = store.put(ref, bad, bad_opts);
    ContractError(r, fss::ErrorKind::kChecksumMismatch, "校验和不符必须报 kChecksumMismatch");
    // 契约 §5：kChecksumMismatch 的 details 必须携带期望/实际（便于排障与映射）
    REQUIRE(r.error().Find("expected").has_value());
    REQUIRE(r.error().Find("actual").has_value());

    // 声明了校验和却没给算法 → 无法校验，必须拒绝而不是"当作没要求"
    StringSource no_alg(payload);
    PutOptions no_alg_opts;
    no_alg_opts.expected_checksum = crypto::Sha256Hex(payload);
    ContractError(store.put(ref, no_alg, no_alg_opts), fss::ErrorKind::kInvalidArgument,
                  "有 expected_checksum 但无算法必须被拒");
  }

  SECTION("stat：存在/不存在；get 不存在对象 → kNotFound") {
    ContractOk(store.ensure_container(container), "ensure_container");
    const auto ref = MakeRef(container, "stat/obj.bin");
    StringSource src("0123456789");
    PutOptions opts;
    opts.content_type = "application/octet-stream";
    ContractOk(store.put(ref, src, opts), "put");

    const auto st = ContractOk(store.stat(ref), "stat 已存在对象");
    REQUIRE(st.exists);
    REQUIRE(st.size == 10);
    REQUIRE(st.content_type == "application/octet-stream");
    //  ★ P9-D04/P9-D08：`stat()` 的 `last_modified_epoch_seconds` 同样是**Unix 纪元秒**契约。
    //    这条断言以前不存在，于是两个驱动各自错着：POSIX 这里是对的（`st_mtime`）、
    //    但 S3 的 `HEAD` 路径解析不到 `Last-Modified`（mock 也没回）→ 恒为 0。
    //    界与 `list` 段一致：下界挡"时基错误/未初始化"，上界挡"单位错误/未来时间"。
    {
      const std::int64_t now_epoch =
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
      CAPTURE(st.last_modified_epoch_seconds);
      REQUIRE(st.last_modified_epoch_seconds >= 1600000000);
      REQUIRE(st.last_modified_epoch_seconds <= now_epoch + 86400);
    }

    const auto missing = ContractOk(store.stat(MakeRef(container, "stat/nope.bin")),
                                    "stat 缺失对象：Ok + exists=false（不是错误）");
    REQUIRE_FALSE(missing.exists);

    StringSink sink;
    ContractError(store.get(MakeRef(container, "stat/nope.bin"), sink, ByteRange{}),
                  fss::ErrorKind::kNotFound, "get 缺失对象 → kNotFound");
  }

  SECTION("get 区间读：能力门控 + 边界（offset>=size / 超出末尾截断）") {
    const auto caps = store.capabilities();
    ContractOk(store.ensure_container(container), "ensure_container");
    const auto ref = MakeRef(container, "range/obj.bin");
    StringSource src("0123456789");
    ContractOk(store.put(ref, src, PutOptions{}), "put 0123456789");

    if (!caps.range_read) {
      // 没有区间能力时，非全量区间必须显式报"未实现"，而不是返回整对象
      StringSink sink;
      ContractError(store.get(ref, sink, ByteRange{2, 3}), fss::ErrorKind::kUnimplemented,
                    "range_read=false 时非全量区间必须报 kUnimplemented");
      StringSink whole;
      ContractOk(store.get(ref, whole, ByteRange{}), "全量读仍必须可用");
      REQUIRE(whole.str() == "0123456789");
      return;
    }

    StringSink middle;
    ContractOk(store.get(ref, middle, ByteRange{2, 3}), "区间 [2,3)");
    REQUIRE(middle.str() == "234");

    StringSink tail;
    ContractOk(store.get(ref, tail, ByteRange{8, 0}), "区间 [8, EOF)");
    REQUIRE(tail.str() == "89");

    StringSink all;
    ContractOk(store.get(ref, all, ByteRange{0, 0}), "区间 [0, EOF) == 全量");
    REQUIRE(all.str() == "0123456789");

    // RFC 7233 §2.1：末端超界要**截断**，不是报错
    StringSink clamped;
    ContractOk(store.get(ref, clamped, ByteRange{5, 100}), "区间末端超界必须截断到末尾");
    REQUIRE(clamped.str() == "56789");

    // offset == size 不是"空区间"而是不可满足（对齐 S3 Range 的 416 语义）
    StringSink oob;
    ContractError(store.get(ref, oob, ByteRange{10, 0}), fss::ErrorKind::kInvalidArgument,
                  "offset == size 视为不可满足区间");
  }

  SECTION("copy：能力门控 + 内容一致 + 覆盖目标 + 源不存在") {
    const auto caps = store.capabilities();
    ContractOk(store.ensure_container(container), "ensure_container");
    const auto src_ref = MakeRef(container, "copy/src.bin");
    const auto dst_ref = MakeRef(container, "copy/dst.bin");
    StringSource payload("copy me");
    ContractOk(store.put(src_ref, payload, PutOptions{}), "put 源对象");

    StringSource old("old content");
    ContractOk(store.put(dst_ref, old, PutOptions{}), "put 旧目标对象");

    if (!caps.server_side_copy) {
      ContractError(store.copy(src_ref, dst_ref), fss::ErrorKind::kUnimplemented,
                    "server_side_copy=false 时 copy 必须报 kUnimplemented");
      return;
    }

    const auto st = ContractOk(store.copy(src_ref, dst_ref), "copy 到已存在的目标（必须覆盖）");
    REQUIRE(st.exists);
    REQUIRE(st.size == 7);
    StringSink sink;
    ContractOk(store.get(dst_ref, sink, ByteRange{}), "get 目标对象");
    REQUIRE(sink.str() == "copy me");
    StringSink src_sink;
    ContractOk(store.get(src_ref, src_sink, ByteRange{}), "get 源对象（复制后源必须仍在）");
    REQUIRE(src_sink.str() == "copy me");

    ContractError(store.copy(MakeRef(container, "copy/missing.bin"), dst_ref),
                  fss::ErrorKind::kNotFound, "copy 缺失源 → kNotFound");
    ContractError(store.copy(src_ref, MakeRef("no-such-container-xyz", "x")),
                  fss::ErrorKind::kNotFound, "copy 到不存在的容器 → kNotFound");
  }

  SECTION("remove：幂等（缺失对象再删也必须成功）") {
    ContractOk(store.ensure_container(container), "ensure_container");
    const auto ref = MakeRef(container, "remove/obj.bin");
    StringSource src("bye");
    ContractOk(store.put(ref, src, PutOptions{}), "put");
    ContractOk(store.remove(ref), "remove 已存在对象");
    const auto st = ContractOk(store.stat(ref), "stat 删除后");
    REQUIRE_FALSE(st.exists);
    StringSink sink;
    ContractError(store.get(ref, sink, ByteRange{}), fss::ErrorKind::kNotFound, "删除后 get");
    ContractOk(store.remove(ref), "remove 缺失对象（必须幂等）");
  }

  SECTION("list：字典序 + 前缀过滤 + continuation token 分页（不重不漏）") {
    ContractOk(store.ensure_container(container), "ensure_container");
    // 故意乱序写入，验证 list 的排序是**语义**而不是"插入顺序"
    const std::vector<std::string> keys = {"p/c", "p/a", "p/e", "p/b", "p/d"};
    for (const auto& k : keys) {
      StringSource src("v:" + k);
      ContractOk(store.put(MakeRef(container, k), src, PutOptions{}), "put " + k);
    }
    StringSource other("other");
    ContractOk(store.put(MakeRef(container, "q/x"), other, PutOptions{}), "put q/x");

    const auto all = ContractOk(store.list(container, "p/", "", 100), "list 前缀 p/");
    REQUIRE(all.entries.size() == 5);
    REQUIRE_FALSE(all.truncated);
    REQUIRE(all.continuation_token.empty());
    for (std::size_t i = 1; i < all.entries.size(); ++i) {
      REQUIRE(all.entries[i - 1].key < all.entries[i].key);  // 严格升序
    }
    REQUIRE(all.entries.front().key == "p/a");
    REQUIRE(all.entries.back().key == "p/e");
    REQUIRE(all.entries.front().size == 5);  // "v:p/a"
    //  ★ P9-D04：`last_modified_epoch_seconds` 的契约是**Unix 纪元秒**。
    //  这条断言以前**不存在**，于是 POSIX 驱动把 `std::filesystem::last_write_time()`
    //  的 file_clock 时基当成 Unix 秒（得到 -4.7e9 的负数）而无人发现；
    //  受害的是 GC 的"太新 → 保护"比较（`> cutoff` 恒假 ⇒ 误删在途对象）。
    //  下界 1600000000（2020-09-13）挡"时基错误/未初始化"，
    //  上界 = 系统当前时间 + 1 天挡"单位错误（毫秒/纳秒）与未来时间"。
    //  两个界都不假设具体时钟：契约测试用可注入时钟（ManualClock 默认 1.7e9）。
    const std::int64_t now_epoch =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    for (const auto& entry : all.entries) {
      CAPTURE(entry.key, entry.last_modified_epoch_seconds);
      REQUIRE(entry.last_modified_epoch_seconds >= 1600000000);
      REQUIRE(entry.last_modified_epoch_seconds <= now_epoch + 86400);
    }

    const auto none = ContractOk(store.list(container, "zzz/", "", 100), "list 无匹配前缀");
    REQUIRE(none.entries.empty());
    REQUIRE_FALSE(none.truncated);

    // 分页：limit=2 → 2 / 2 / 1，token 接续，页间不重不漏
    std::vector<std::string> collected;
    std::string token;
    int pages = 0;
    for (;;) {
      const auto page = ContractOk(store.list(container, "p/", token, 2), "list 分页");
      ++pages;
      REQUIRE(page.entries.size() <= 2);
      for (const auto& e : page.entries) collected.push_back(e.key);
      if (!page.truncated) {
        REQUIRE(page.continuation_token.empty());
        break;
      }
      REQUIRE_FALSE(page.continuation_token.empty());
      token = page.continuation_token;
      REQUIRE(pages < 10);  // 防止 token 不前进导致死循环
    }
    REQUIRE(pages == 3);
    REQUIRE(collected == std::vector<std::string>{"p/a", "p/b", "p/c", "p/d", "p/e"});

    ContractError(store.list("no-such-container-xyz", "", "", 10), fss::ErrorKind::kNotFound,
                  "list 不存在的容器 → kNotFound");
  }

  SECTION("容器隔离：同名 key 在不同容器互不串扰") {
    ContractOk(store.ensure_container(container), "ensure_container A");
    ContractOk(store.ensure_container(container + "-b"), "ensure_container B");
    StringSource a("content-a");
    StringSource b("content-b");
    ContractOk(store.put(MakeRef(container, "same/key"), a, PutOptions{}), "put A");
    ContractOk(store.put(MakeRef(container + "-b", "same/key"), b, PutOptions{}), "put B");

    StringSink sa;
    ContractOk(store.get(MakeRef(container, "same/key"), sa, ByteRange{}), "get A");
    REQUIRE(sa.str() == "content-a");
    StringSink sb;
    ContractOk(store.get(MakeRef(container + "-b", "same/key"), sb, ByteRange{}), "get B");
    REQUIRE(sb.str() == "content-b");
  }
}

// =============================================================================
//  二、IFileLocationRepository 契约（memory / sqlite / postgres 共用）
// =============================================================================
inline domain::FileLocation MakeLocation(const std::string& file_id,
                                         const std::string& file_source,
                                         domain::StorageZone zone = domain::StorageZone::kStaging,
                                         std::int64_t created = 1700000000,
                                         std::int64_t updated = 1700000000,
                                         const std::string& user_id = "") {
  domain::FileLocation loc;
  loc.file_id = file_id;
  loc.driver = domain::StorageDriver::kPosix;
  loc.zone = zone;
  loc.file_source = file_source;
  loc.user_id = user_id;
  loc.created_at_epoch_seconds = created;
  loc.updated_at_epoch_seconds = updated;
  return loc;
}

inline void RequireSameLocation(const domain::FileLocation& got,
                                const domain::FileLocation& want) {
  REQUIRE(got.file_id == want.file_id);
  REQUIRE(got.driver == want.driver);
  REQUIRE(got.zone == want.zone);
  REQUIRE(got.file_source == want.file_source);
  REQUIRE(got.user_id == want.user_id);
  REQUIRE(got.signed_url == want.signed_url);
  REQUIRE(got.created_at_epoch_seconds == want.created_at_epoch_seconds);
  REQUIRE(got.updated_at_epoch_seconds == want.updated_at_epoch_seconds);
  // 开放字段必须原样保留（否则"读-改-写"会静默丢字段）
  REQUIRE(json::Dump(got.extra) == json::Dump(want.extra));
}

inline void CheckLocationRepositoryContract(domain::IFileLocationRepository& repo,
                                            const std::string& partition_prefix = "contract-part") {
  //  ★ `partition_prefix`（默认值保持既有调用点逐字不变）让**共享 PG 上的测试**可以用
  //    `pgtest-<pid>-…` 这样每次运行都唯一的租户名，从而"可重跑 + 不打扰别人的数据"；
  //    内存/SQLite 实现每次都是全新状态，沿用默认前缀即可。
  const std::string pa = partition_prefix + "-a";
  const std::string pb = partition_prefix + "-b";
  const std::string pe = partition_prefix + "-empty";

  SECTION("Save/Find：字段逐一往返（含 extra 开放字段）") {
    auto loc = MakeLocation("file-1", "/u/1/ts/file-1", domain::StorageZone::kStaging, 100, 100);
    loc.signed_url = "https://example.invalid/a?sig=1";
    loc.extra["CustomField"] = "custom-value";
    //  ★ **必须**带上 LocationIssuer 真正写的那两个键：物理引用就藏在 extra 里，
    //    只用自造的 `CustomField` 测不出"实现把它们当成已知字段吞掉"这类缺陷
    //    （SQLite 实现曾因此静默丢引用 → CreateFileMetadata 报 500，P4-D09）。
    loc.extra[std::string(fss::app::LocationIssuer::kExtraContainer)] = "opendes-staging";
    loc.extra[std::string(fss::app::LocationIssuer::kExtraObjectKey)] = "osdu-user/1-ts/file-1";
    ContractOk(repo.Save(pa, loc), "Save");
    const auto got = ContractOk(repo.Find(pa, "file-1"), "Find");
    RequireSameLocation(got, loc);
    REQUIRE(got.extra.value("CustomField", std::string{}) == "custom-value");
    REQUIRE(got.extra.value(std::string(fss::app::LocationIssuer::kExtraContainer), std::string{}) ==
            "opendes-staging");
    REQUIRE(got.extra.value(std::string(fss::app::LocationIssuer::kExtraObjectKey), std::string{}) ==
            "osdu-user/1-ts/file-1");
  }

  SECTION("Save 是 upsert：同 file_id 再次 Save 更新 zone（CreateFileMetadata 依赖它）") {
    auto staging = MakeLocation("file-2", "/u/2/ts/file-2", domain::StorageZone::kStaging);
    ContractOk(repo.Save(pa, staging), "Save staging");
    auto persistent = staging;
    persistent.zone = domain::StorageZone::kPersistent;
    persistent.updated_at_epoch_seconds = 200;
    ContractOk(repo.Save(pa, persistent), "Save persistent（同 file_id 必须覆盖而不是报冲突）");
    const auto got = ContractOk(repo.Find(pa, "file-2"), "Find");
    REQUIRE(got.zone == domain::StorageZone::kPersistent);
    REQUIRE(got.updated_at_epoch_seconds == 200);
  }

  SECTION("幂等键 (partition,file_source) 唯一（R5）—— 不同 file_id 抢同一 file_source 必须被拒") {
    ContractOk(repo.Save(pa, MakeLocation("file-3", "/u/3/ts/file-3")), "Save 3");
    ContractError(repo.Save(pa, MakeLocation("file-3b", "/u/3/ts/file-3")),
                  fss::ErrorKind::kLocationAlreadyExists,
                  "同一 (partition,file_source) 不能绑定到第二个 file_id");
    // 被拒后原记录必须**保持不变**（不能出现半写入）
    const auto got = ContractOk(repo.Find(pa, "file-3"), "Find 3");
    REQUIRE(got.file_id == "file-3");
  }

  SECTION("Find/FindByFileSource：缺失 → kNotFound") {
    ContractError(repo.Find(pa, "no-such-file"), fss::ErrorKind::kNotFound, "Find 缺失");
    ContractError(repo.FindByFileSource(pa, "/no/such/source"),
                  fss::ErrorKind::kNotFound, "FindByFileSource 缺失");
  }

  SECTION("FindByFileSource：按幂等键取回；Save 更新后仍指向同一条") {
    auto loc = MakeLocation("file-4", "/u/4/ts/file-4");
    ContractOk(repo.Save(pa, loc), "Save");
    const auto by_source = ContractOk(repo.FindByFileSource(pa, "/u/4/ts/file-4"),
                                      "FindByFileSource");
    REQUIRE(by_source.file_id == "file-4");
    loc.zone = domain::StorageZone::kPersistent;
    ContractOk(repo.Save(pa, loc), "Save 更新");
    const auto again = ContractOk(repo.FindByFileSource(pa, "/u/4/ts/file-4"),
                                  "FindByFileSource 更新后");
    REQUIRE(again.zone == domain::StorageZone::kPersistent);
  }

  SECTION("UpdateSignedUrl：只改 signed_url/updated_at，created_at 不变；缺失 → kNotFound") {
    auto loc = MakeLocation("file-5", "/u/5/ts/file-5", domain::StorageZone::kStaging, 111, 111);
    ContractOk(repo.Save(pa, loc), "Save");
    ContractOk(repo.UpdateSignedUrl(pa, "file-5", "https://example.invalid/s?sig=2", 222),
               "UpdateSignedUrl");
    const auto got = ContractOk(repo.Find(pa, "file-5"), "Find");
    REQUIRE(got.signed_url == "https://example.invalid/s?sig=2");
    REQUIRE(got.updated_at_epoch_seconds == 222);
    REQUIRE(got.created_at_epoch_seconds == 111);
    ContractError(repo.UpdateSignedUrl(pa, "no-such-file", "u", 1), fss::ErrorKind::kNotFound,
                  "UpdateSignedUrl 缺失 → kNotFound");
  }

  SECTION("Delete：删除后 Find/FindByFileSource → kNotFound；file_source 被释放；缺失 → kNotFound") {
    ContractOk(repo.Save(pa, MakeLocation("file-6", "/u/6/ts/file-6")), "Save");
    ContractOk(repo.Delete(pa, "file-6"), "Delete");
    ContractError(repo.Find(pa, "file-6"), fss::ErrorKind::kNotFound, "删除后 Find");
    ContractError(repo.FindByFileSource(pa, "/u/6/ts/file-6"), fss::ErrorKind::kNotFound,
                  "删除后 FindByFileSource");
    // 幂等键必须被释放（否则 file_source 永远无法被复用）
    ContractOk(repo.Save(pa, MakeLocation("file-6b", "/u/6/ts/file-6")), "复用已释放的 file_source");
    ContractError(repo.Delete(pa, "no-such-file"), fss::ErrorKind::kNotFound, "删除缺失");
  }

  SECTION("★ partition 严格隔离：同 file_id / 同 file_source 在不同租户各自独立") {
    ContractOk(repo.Save(pa, MakeLocation("shared-id", "/shared/source")), "Save 到 A");
    ContractOk(repo.Save(pb, MakeLocation("shared-id", "/shared/source")), "Save 到 B");
    auto a = ContractOk(repo.Find(pa, "shared-id"), "Find A");
    auto b = ContractOk(repo.Find(pb, "shared-id"), "Find B");
    REQUIRE(a.file_id == "shared-id");
    REQUIRE(b.file_id == "shared-id");

    // 跨租户不得命中
    ContractError(repo.Find(pe, "shared-id"), fss::ErrorKind::kNotFound,
                  "第三个租户不得看到 A/B 的记录");
    // 删除 A 不影响 B
    ContractOk(repo.Delete(pa, "shared-id"), "Delete A");
    ContractError(repo.Find(pa, "shared-id"), fss::ErrorKind::kNotFound, "A 已删");
    ContractOk(repo.Find(pb, "shared-id"), "B 必须仍在");
  }

  SECTION("参数校验：空 partition / file_id / file_source 必须被拒") {
    ContractError(repo.Save("", MakeLocation("f", "/s")), fss::ErrorKind::kInvalidArgument,
                  "空 partition");
    ContractError(repo.Save(pa, MakeLocation("", "/s")), fss::ErrorKind::kInvalidArgument,
                  "空 file_id");
    ContractError(repo.Save(pa, MakeLocation("f", "")), fss::ErrorKind::kInvalidArgument,
                  "空 file_source");
  }

  SECTION("List：用户/时间过滤 + 稳定排序 + limit/offset + total（getFileList 的数据面）") {
    //  故意乱序插入，验证排序是**语义**而不是插入顺序
    ContractOk(repo.Save(pa, MakeLocation("l-2", "/u/b/1-ts/l-2", domain::StorageZone::kStaging,
                                          200, 200, "user-b")),
               "Save l-2");
    ContractOk(repo.Save(pa, MakeLocation("l-1", "/u/a/1-ts/l-1", domain::StorageZone::kStaging,
                                          100, 100, "user-a")),
               "Save l-1");
    ContractOk(repo.Save(pa, MakeLocation("l-3", "/u/a/2-ts/l-3", domain::StorageZone::kStaging,
                                          300, 300, "user-a")),
               "Save l-3");
    ContractOk(repo.Save(pb, MakeLocation("l-9", "/u/a/1-ts/l-9", domain::StorageZone::kStaging,
                                          100, 100, "user-a")),
               "Save 到另一个 partition");

    // 全部：稳定全序（created_at 升序）
    domain::LocationQuery all;
    all.limit = 100;
    const auto page = ContractOk(repo.List(pa, all), "List 全量");
    REQUIRE(page.total == 3);
    REQUIRE(page.records.size() == 3);
    REQUIRE(page.records[0].file_id == "l-1");
    REQUIRE(page.records[1].file_id == "l-2");
    REQUIRE(page.records[2].file_id == "l-3");

    // ★ partition 隔离
    const auto other = ContractOk(repo.List(pb, all), "List 另一个 partition");
    REQUIRE(other.total == 1);
    REQUIRE(other.records[0].file_id == "l-9");

    // 用户过滤
    domain::LocationQuery by_user;
    by_user.user_id = "user-a";
    by_user.limit = 100;
    const auto user_page = ContractOk(repo.List(pa, by_user), "List 按 user_id");
    REQUIRE(user_page.total == 2);
    REQUIRE(user_page.records[0].file_id == "l-1");
    REQUIRE(user_page.records[1].file_id == "l-3");

    // 时间边界含端点；-1 = 无界
    domain::LocationQuery window;
    window.created_after_epoch_seconds = 100;
    window.created_before_epoch_seconds = 200;
    window.limit = 100;
    const auto window_page = ContractOk(repo.List(pa, window), "List 时间窗口 [100,200]");
    REQUIRE(window_page.total == 2);

    // 分页：limit=2 → 2 + 1；total 是过滤后、分页前的总数
    domain::LocationQuery page1;
    page1.limit = 2;
    page1.offset = 0;
    const auto p1 = ContractOk(repo.List(pa, page1), "List limit=2 offset=0");
    REQUIRE(p1.records.size() == 2);
    REQUIRE(p1.total == 3);
    domain::LocationQuery page2 = page1;
    page2.offset = 2;
    const auto p2 = ContractOk(repo.List(pa, page2), "List limit=2 offset=2");
    REQUIRE(p2.records.size() == 1);
    REQUIRE(p2.records[0].file_id == "l-3");
    REQUIRE(p2.total == 3);

    // 空 partition / 无匹配 → Ok + 空页（不是错误；"无记录返回 400"是**用例层**的契约）
    domain::LocationQuery empty_query;
    empty_query.user_id = "no-such-user";
    empty_query.limit = 100;
    const auto empty = ContractOk(repo.List(pa, empty_query), "List 无匹配 → 空页");
    REQUIRE(empty.total == 0);
    REQUIRE(empty.records.empty());
    domain::LocationQuery missing_partition;
    missing_partition.limit = 100;
    const auto missing = ContractOk(repo.List(pe, missing_partition),
                                    "List 不存在的 partition → 空页");
    REQUIRE(missing.total == 0);

    // 参数校验
    domain::LocationQuery bad_limit;
    bad_limit.limit = 0;
    ContractError(repo.List(pa, bad_limit), fss::ErrorKind::kInvalidArgument, "limit<=0");
    domain::LocationQuery bad_offset;
    bad_offset.offset = -1;
    ContractError(repo.List(pa, bad_offset), fss::ErrorKind::kInvalidArgument, "offset<0");
  }
}

// =============================================================================
//  三、IMetadataRepository 契约（memory / sqlite / postgres / remote 共用）
// =============================================================================
inline domain::FileMetadataRecord MakeRecord(const std::string& partition,
                                             const std::string& id_suffix,
                                             const std::string& file_source,
                                             const std::string& name,
                                             const std::string& kind = "") {
  domain::FileMetadataRecord r;
  r.id = partition + ":dataset--File.Generic:" + id_suffix;
  r.kind = kind.empty() ? (partition + ":wks:dataset--File.Generic:1.0.0") : kind;
  r.acl.viewers = {"viewer@example.com"};
  r.acl.owners = {"owner@example.com"};
  r.legal.legaltags = {"opendes-public-1"};
  r.legal.status = domain::LegalStatus::kCompliant;
  r.data.name = name;
  r.data.endian = "LITTLE";
  r.data.checksum = "deadbeef";
  r.data.checksum_algorithm = "SHA256";
  r.data.dataset_properties.file_source_info.file_source = file_source;
  return r;
}

inline void CheckMetadataRepositoryContract(domain::IMetadataRepository& repo,
                                            fss::ManualClock& clock,
                                            const std::string& partition_prefix = "contract-part") {
  //  ★ `partition_prefix`（默认值保持既有调用点逐字不变）让**共享 PG 上的测试**可以用
  //    `pgtest-<pid>-…` 这样每次运行都唯一的租户名（可重跑 + 不打扰别人的数据），
  //    与 `CheckLocationRepositoryContract` / `CheckLeaseContract` 同一套约定。
  const std::string pa = partition_prefix + "-a";
  const std::string pb = partition_prefix + "-b";

  SECTION("Create：version=1，GetById / GetLatestByFileSource 可取回") {
    const auto rec = MakeRecord(pa, "aaa1", "/u/1/ts/aaa1", "record-one");
    const auto created = ContractOk(repo.Create(pa, rec), "Create");
    REQUIRE(created.id == rec.id);
    REQUIRE(created.version == 1);

    const auto by_id = ContractOk(repo.GetById(pa, rec.id), "GetById");
    REQUIRE(by_id.version == 1);
    REQUIRE(by_id.data.name.has_value());
    REQUIRE(*by_id.data.name == "record-one");

    const auto by_source = ContractOk(repo.GetLatestByFileSource(pa, "/u/1/ts/aaa1"),
                                      "GetLatestByFileSource");
    REQUIRE(by_source.id == rec.id);
    REQUIRE(by_source.version == 1);
  }

  SECTION("Create 幂等（R5）：同 (partition,file_source) 重复创建不产生新记录/新版本") {
    const auto first = MakeRecord(pa, "bbb1", "/u/2/ts/bbb1", "first-name");
    ContractOk(repo.Create(pa, first), "Create 第一次");
    // 第二次换了 id 与内容，但幂等键相同 —— 必须返回**第一次那条**，而不是静默插一条新的
    const auto second = MakeRecord(pa, "bbb2", "/u/2/ts/bbb1", "second-name");
    const auto got = ContractOk(repo.Create(pa, second), "Create 第二次（幂等键相同）");
    REQUIRE(got.id == first.id);
    REQUIRE(got.version == 1);
    REQUIRE(got.data.name.has_value());
    REQUIRE(*got.data.name == "first-name");

    // 且第二次的 id **没有**被创建
    ContractError(repo.GetById(pa, second.id), fss::ErrorKind::kNotFound,
                  "幂等重复创建不得产生第二条记录");
    const auto page = ContractOk(repo.List(pa, domain::MetadataQuery{}), "List");
    REQUIRE(page.total == 1);
  }

  SECTION("Create 参数校验：id 与 partition 不一致 / 空 file_source / 重复 id 必须被拒") {
    ContractError(repo.Create(partition_prefix + "-x", MakeRecord(pa, "ccc1", "/u/3/ts/ccc1", "n")),
                  fss::ErrorKind::kInvalidArgument,
                  "record.id 的前缀与 partition 参数不一致（防止绕过租户隔离）");
    ContractError(repo.Create(pa, MakeRecord(pa, "ccc2", "", "n")),
                  fss::ErrorKind::kInvalidArgument, "空 file_source（幂等键缺失）");
    ContractOk(repo.Create(pa, MakeRecord(pa, "ccc3", "/u/3/ts/ccc3", "n")), "Create");
    ContractError(repo.Create(pa, MakeRecord(pa, "ccc3", "/u/3/ts/ccc3b", "n")),
                  fss::ErrorKind::kInvalidArgument, "同 id 不同 file_source 必须被拒");
    ContractError(repo.Create("", MakeRecord(pa, "ccc4", "/u/3/ts/ccc4", "n")),
                  fss::ErrorKind::kInvalidArgument, "空 partition");
  }

  SECTION("Update：version+1 的版本链，latest 切换，List 只看到 latest") {
    const auto v1 = MakeRecord(pa, "ddd1", "/u/4/ts/ddd1", "name-v1");
    ContractOk(repo.Create(pa, v1), "Create v1");
    clock.AdvanceSeconds(5);

    auto v2 = v1;
    v2.data.name = "name-v2";
    const auto updated = ContractOk(repo.Update(pa, v2), "Update → v2");
    REQUIRE(updated.id == v1.id);
    REQUIRE(updated.version == 2);

    const auto by_id = ContractOk(repo.GetById(pa, v1.id), "GetById 应为 latest");
    REQUIRE(by_id.version == 2);
    REQUIRE(by_id.data.name.has_value());
    REQUIRE(*by_id.data.name == "name-v2");
    const auto latest = ContractOk(repo.GetLatestByFileSource(pa, "/u/4/ts/ddd1"),
                                   "GetLatestByFileSource");
    REQUIRE(latest.version == 2);

    const auto page = ContractOk(repo.List(pa, domain::MetadataQuery{}), "List");
    REQUIRE(page.total == 1);  // 版本链不产生"多条记录"
    REQUIRE(page.records.size() == 1);
    REQUIRE(page.records.front().version == 2);

    // 失败路径：不存在的 id；以及试图改写幂等键（file_source）
    auto ghost = MakeRecord(pa, "dddd", "/u/4/ts/dddd", "ghost");
    ContractError(repo.Update(pa, ghost), fss::ErrorKind::kNotFound, "Update 缺失 → kNotFound");
    auto moved = v2;
    moved.data.dataset_properties.file_source_info.file_source = "/u/4/ts/other";
    ContractError(repo.Update(pa, moved), fss::ErrorKind::kInvalidArgument,
                  "Update 不得改写 (partition,file_source) 幂等键");
  }

  SECTION("Delete：删除全部版本；缺失 → kNotFound") {
    const auto rec = MakeRecord(pa, "eee1", "/u/5/ts/eee1", "to-delete");
    ContractOk(repo.Create(pa, rec), "Create");
    auto v2 = rec;
    v2.data.name = "to-delete-v2";
    ContractOk(repo.Update(pa, v2), "Update");
    ContractOk(repo.Delete(pa, rec.id), "Delete");
    ContractError(repo.GetById(pa, rec.id), fss::ErrorKind::kNotFound, "删除后 GetById");
    ContractError(repo.GetLatestByFileSource(pa, "/u/5/ts/eee1"), fss::ErrorKind::kNotFound,
                  "删除后 GetLatestByFileSource");
    ContractError(repo.Delete(pa, rec.id), fss::ErrorKind::kNotFound, "重复 Delete → kNotFound");
    const auto page = ContractOk(repo.List(pa, domain::MetadataQuery{}), "List");
    REQUIRE(page.total == 0);
  }

  SECTION("List：kind / name_prefix / 时间区间过滤 + limit/offset + total + 排序") {
    const std::int64_t t0 = clock.NowEpochSeconds();
    ContractOk(repo.Create(pa, MakeRecord(pa, "f1", "/u/6/ts/f1", "alpha",
                                          "part:dataset--File.Generic:1.0.0")),
               "Create alpha");
    clock.AdvanceSeconds(1);
    ContractOk(repo.Create(pa, MakeRecord(pa, "f2", "/u/6/ts/f2", "alphabet",
                                          "part:dataset--File.Generic:1.0.0")),
               "Create alphabet");
    clock.AdvanceSeconds(1);
    ContractOk(repo.Create(pa, MakeRecord(pa, "f3", "/u/6/ts/f3", "beta",
                                          "part:dataset--File.Other:1.0.0")),
               "Create beta");

    const auto all = ContractOk(repo.List(pa, domain::MetadataQuery{}), "List 全量");
    REQUIRE(all.total == 3);
    REQUIRE(all.records.size() == 3);
    // 排序：created_at 升序（分页必须稳定，否则 offset 会漏/重）
    REQUIRE(*all.records[0].data.name == "alpha");
    REQUIRE(*all.records[1].data.name == "alphabet");
    REQUIRE(*all.records[2].data.name == "beta");

    domain::MetadataQuery by_kind;
    by_kind.kind = "part:dataset--File.Generic:1.0.0";
    const auto kind_page = ContractOk(repo.List(pa, by_kind), "List kind 过滤");
    REQUIRE(kind_page.total == 2);

    domain::MetadataQuery by_prefix;
    by_prefix.name_prefix = "alpha";
    const auto prefix_page = ContractOk(repo.List(pa, by_prefix), "List name_prefix=alpha");
    REQUIRE(prefix_page.total == 2);
    domain::MetadataQuery by_prefix2;
    by_prefix2.name_prefix = "alphab";
    const auto prefix_page2 = ContractOk(repo.List(pa, by_prefix2), "List name_prefix=alphab");
    REQUIRE(prefix_page2.total == 1);

    domain::MetadataQuery after;
    after.created_after_epoch_seconds = t0 + 1;  // 含下界
    const auto after_page = ContractOk(repo.List(pa, after), "List created_after");
    REQUIRE(after_page.total == 2);

    domain::MetadataQuery before;
    before.created_before_epoch_seconds = t0 + 1;
    const auto before_page = ContractOk(repo.List(pa, before), "List created_before");
    REQUIRE(before_page.total == 2);

    domain::MetadataQuery window;
    window.created_after_epoch_seconds = t0 + 1;
    window.created_before_epoch_seconds = t0 + 1;
    const auto window_page = ContractOk(repo.List(pa, window), "List 单点时间窗口");
    REQUIRE(window_page.total == 1);
    REQUIRE(*window_page.records[0].data.name == "alphabet");

    domain::MetadataQuery page1;
    page1.limit = 2;
    page1.offset = 0;
    const auto p1 = ContractOk(repo.List(pa, page1), "List limit=2 offset=0");
    REQUIRE(p1.records.size() == 2);
    REQUIRE(p1.total == 3);  // total 是"过滤后的总数"，不是本页条数
    domain::MetadataQuery page2 = page1;
    page2.offset = 2;
    const auto p2 = ContractOk(repo.List(pa, page2), "List limit=2 offset=2");
    REQUIRE(p2.records.size() == 1);
    REQUIRE(p2.total == 3);
    REQUIRE(*p2.records[0].data.name == "beta");

    // 参数校验（正例对照已在上面）
    domain::MetadataQuery bad_limit;
    bad_limit.limit = 0;
    ContractError(repo.List(pa, bad_limit), fss::ErrorKind::kInvalidArgument, "limit<=0");
    domain::MetadataQuery bad_offset;
    bad_offset.offset = -1;
    ContractError(repo.List(pa, bad_offset), fss::ErrorKind::kInvalidArgument, "offset<0");
  }

  SECTION("★ partition 严格隔离：同 file_source 在不同租户各自独立") {
    ContractOk(repo.Create(pa, MakeRecord(pa, "g1", "/shared/fs", "in-a")), "Create A");
    ContractOk(repo.Create(pb, MakeRecord(pb, "g2", "/shared/fs", "in-b")), "Create B");
    const auto a = ContractOk(repo.GetLatestByFileSource(pa, "/shared/fs"), "A");
    const auto b = ContractOk(repo.GetLatestByFileSource(pb, "/shared/fs"), "B");
    REQUIRE(*a.data.name == "in-a");
    REQUIRE(*b.data.name == "in-b");

    // 跨租户按 id 也查不到
    ContractError(repo.GetById(pb, pa + ":dataset--File.Generic:g1"), fss::ErrorKind::kNotFound,
                  "跨租户 GetById 必须 miss");
    const auto a_page = ContractOk(repo.List(pa, domain::MetadataQuery{}), "List A");
    REQUIRE(a_page.total == 1);
    const auto b_page = ContractOk(repo.List(pb, domain::MetadataQuery{}), "List B");
    REQUIRE(b_page.total == 1);

    ContractOk(repo.Delete(pa, pa + ":dataset--File.Generic:g1"), "Delete A");
    ContractOk(repo.GetLatestByFileSource(pb, "/shared/fs"), "删除 A 不影响 B");
  }

  // ===========================================================================
  //  ★ C1（ADR-009 §4.2）：claiming → ready 状态机（memory / SQLite / PG 共用同一套断言）
  // ===========================================================================
  //  语义登记：
  //    · `ClaimForWrite` 原子领取：无活动记录 → 插入 `claiming` v1 并 claimed=true；
  //      已有活动记录（claiming/ready）→ claimed=false + 既有记录 + 它的状态，**不报错、不复制**。
  //    · `claiming` 行对**所有**客户端读取路径不可见（GetById / GetLatestByFileSource / List）。
  //    · `MarkReady(partition, id, version, record)`：claiming→ready，并把**最终** record
  //      （含 copy 后才算出的 checksum）一次落库；版本/行不存在 → kNotFound；
  //      改 `file_source`（幂等键）→ kInvalidArgument 且**保持 claiming**。
  //    · `ReleaseClaim(partition, id, version)`：只删 claiming 行，删不到 → kNotFound；
  //      释放后同一 file_source 可被重新领取。
  SECTION("★ C1 claim：ClaimForWrite 赢得领取权；claiming 行对三条读取路径不可见（含正控）") {
    const std::string src = "/u/claim/visible";
    const auto rec = MakeRecord(pa, "clm1", src, "claim-name");
    const auto claim = ContractOk(repo.ClaimForWrite(pa, rec), "ClaimForWrite");
    REQUIRE(claim.claimed);
    REQUIRE(claim.state == domain::MetadataState::kClaiming);
    REQUIRE(claim.record.id == rec.id);
    REQUIRE(claim.record.version == 1);

    //  负断言：claiming 行绝不能被当成 ready 交给客户端
    ContractError(repo.GetById(pa, rec.id), fss::ErrorKind::kNotFound, "claiming → GetById 不可见");
    ContractError(repo.GetLatestByFileSource(pa, src), fss::ErrorKind::kNotFound,
                  "claiming → GetLatestByFileSource 不可见");
    {
      const auto page = ContractOk(repo.List(pa, domain::MetadataQuery{}), "claiming → List");
      REQUIRE(page.total == 0);
      REQUIRE(page.records.empty());
    }

    //  ★ 正控：同一条查询路径在 MarkReady 之后**必须**能看到
    //    （否则上面的 kNotFound 可能只是"路径写错所以恒真"）
    auto final_rec = rec;
    final_rec.data.checksum = "feedface";
    const auto ready = ContractOk(repo.MarkReady(pa, rec.id, 1, final_rec), "MarkReady");
    REQUIRE(ready.id == rec.id);
    REQUIRE(ready.version == 1);
    const auto by_id = ContractOk(repo.GetById(pa, rec.id), "ready → GetById");
    REQUIRE(by_id.data.name.has_value());
    REQUIRE(*by_id.data.name == "claim-name");
    const auto by_source = ContractOk(repo.GetLatestByFileSource(pa, src), "ready → GetLatest");
    REQUIRE(by_source.id == rec.id);
    const auto page = ContractOk(repo.List(pa, domain::MetadataQuery{}), "ready → List");
    REQUIRE(page.total == 1);
    REQUIRE(page.records.front().id == rec.id);
  }

  SECTION("★ C1 claim：第二次 ClaimForWrite 返回 claimed=false + 既有状态（claiming → ready）") {
    const std::string src = "/u/claim/second";
    const auto first = MakeRecord(pa, "clm2a", src, "first");
    const auto claim1 = ContractOk(repo.ClaimForWrite(pa, first), "claim1");
    REQUIRE(claim1.claimed);

    const auto second = MakeRecord(pa, "clm2b", src, "second");
    const auto claim2 = ContractOk(repo.ClaimForWrite(pa, second), "claim2（同 file_source）");
    REQUIRE_FALSE(claim2.claimed);
    REQUIRE(claim2.state == domain::MetadataState::kClaiming);
    REQUIRE(claim2.record.id == first.id);
    //  第二个 id 从未被创建
    ContractError(repo.GetById(pa, second.id), fss::ErrorKind::kNotFound, "第二个 id 不得存在");

    ContractOk(repo.MarkReady(pa, first.id, 1, first), "MarkReady");
    const auto claim3 = ContractOk(repo.ClaimForWrite(pa, second), "claim3（ready 后）");
    REQUIRE_FALSE(claim3.claimed);
    REQUIRE(claim3.state == domain::MetadataState::kReady);
    REQUIRE(claim3.record.id == first.id);
  }

  SECTION("★ C1 claim：MarkReady 落库**最终** record（含 copy 后才算出的 checksum）") {
    const std::string src = "/u/claim/final";
    const auto rec = MakeRecord(pa, "clm3", src, "final-name");
    ContractOk(repo.ClaimForWrite(pa, rec), "ClaimForWrite");
    auto final_rec = rec;
    final_rec.data.checksum = "feedface";
    final_rec.data.checksum_algorithm = "MD5";
    final_rec.data.dataset_properties.file_source_info.checksum = "feedface";
    final_rec.data.dataset_properties.file_source_info.checksum_algorithm = "MD5";

    const auto ready = ContractOk(repo.MarkReady(pa, rec.id, 1, final_rec), "MarkReady");
    REQUIRE(ready.data.checksum.has_value());
    REQUIRE(*ready.data.checksum == "feedface");
    //  ★ 关键：不是只在返回值里，而是真的落库了（读回来仍是最终数据）
    const auto got = ContractOk(repo.GetById(pa, rec.id), "GetById");
    REQUIRE(got.data.checksum.has_value());
    REQUIRE(*got.data.checksum == "feedface");
    REQUIRE(got.data.dataset_properties.file_source_info.checksum_algorithm == "MD5");
  }

  SECTION("★ C1 claim：ReleaseClaim 后可重新领取；wrong version / missing row → kNotFound") {
    const std::string src = "/u/claim/release";
    const auto rec = MakeRecord(pa, "clm4", src, "release");
    const auto claim = ContractOk(repo.ClaimForWrite(pa, rec), "ClaimForWrite");
    REQUIRE(claim.claimed);

    //  两侧都断言：错误版本 / 缺失行 → kNotFound
    ContractError(repo.MarkReady(pa, rec.id, 99, rec), fss::ErrorKind::kNotFound,
                  "MarkReady 版本错 → kNotFound");
    ContractError(repo.ReleaseClaim(pa, rec.id, 99), fss::ErrorKind::kNotFound,
                  "ReleaseClaim 版本错 → kNotFound");
    const auto ghost = MakeRecord(pa, "clm4ghost", "/u/claim/ghost", "ghost");
    ContractError(repo.MarkReady(pa, ghost.id, 1, ghost), fss::ErrorKind::kNotFound,
                  "MarkReady 缺失行 → kNotFound");
    ContractError(repo.ReleaseClaim(pa, ghost.id, 1), fss::ErrorKind::kNotFound,
                  "ReleaseClaim 缺失行 → kNotFound");

    //  正控：正确版本 ReleaseClaim 成功 → 释放后同一 file_source 可被重新领取
    ContractOk(repo.ReleaseClaim(pa, rec.id, 1), "ReleaseClaim");
    const auto reclaim = ContractOk(repo.ClaimForWrite(pa, rec), "ReleaseClaim 后重新领取");
    REQUIRE(reclaim.claimed);
    ContractOk(repo.MarkReady(pa, rec.id, 1, rec), "重新领取后 MarkReady");
    REQUIRE(repo.GetById(pa, rec.id).ok());
    //  反向正控：ReleaseClaim 绝不能删 ready 行
    ContractError(repo.ReleaseClaim(pa, rec.id, 1), fss::ErrorKind::kNotFound,
                  "ReleaseClaim 不得删 ready 行");
    REQUIRE(repo.GetById(pa, rec.id).ok());
  }

  SECTION("★ C1 claim：MarkReady 不得改写 (partition, file_source) 幂等键（拒绝且行仍 claiming）") {
    const std::string src = "/u/claim/guard";
    const auto rec = MakeRecord(pa, "clm5", src, "guard");
    ContractOk(repo.ClaimForWrite(pa, rec), "ClaimForWrite");
    auto hostile = rec;
    hostile.data.dataset_properties.file_source_info.file_source = "/u/claim/hijack";
    ContractError(repo.MarkReady(pa, rec.id, 1, hostile), fss::ErrorKind::kInvalidArgument,
                  "MarkReady 改写幂等键必须被拒");

    //  ★ 被拒后行**仍是 claiming**：不可见、hostile source 不存在、可用正确记录 mark ready（正控）
    ContractError(repo.GetById(pa, rec.id), fss::ErrorKind::kNotFound, "被拒后仍 claiming（不可见）");
    ContractError(repo.GetLatestByFileSource(pa, "/u/claim/hijack"), fss::ErrorKind::kNotFound,
                  "hostile file_source 不得出现");
    const auto ready =
        ContractOk(repo.MarkReady(pa, rec.id, 1, rec), "正控：正确 file_source 必须成功");
    REQUIRE(ready.id == rec.id);
    REQUIRE(repo.GetById(pa, rec.id).ok());
    //  正控②：拒绝时行仍在（因此可以 ReleaseClaim 掉它）
    auto rec2 = MakeRecord(pa, "clm5b", "/u/claim/guard2", "guard2");
    ContractOk(repo.ClaimForWrite(pa, rec2), "ClaimForWrite 2");
    auto hostile2 = rec2;
    hostile2.data.dataset_properties.file_source_info.file_source = "/u/claim/hijack2";
    ContractError(repo.MarkReady(pa, rec2.id, 1, hostile2), fss::ErrorKind::kInvalidArgument,
                  "改写幂等键必须被拒");
    ContractOk(repo.ReleaseClaim(pa, rec2.id, 1), "被拒的行仍可 ReleaseClaim");
  }

  SECTION("★ C1 claim：claiming 行本身是完整可解析的记录（无损往返 + 状态翻转不丢数据）") {
    const std::string src = "/u/claim/wellformed";
    auto rec = MakeRecord(pa, "clm6", src, "well-formed");
    rec.extra["CustomClaimField"] = "keep";
    const auto claim = ContractOk(repo.ClaimForWrite(pa, rec), "ClaimForWrite");
    REQUIRE(claim.claimed);
    REQUIRE(claim.record.data.name.has_value());
    REQUIRE(*claim.record.data.name == "well-formed");
    REQUIRE(claim.record.kind == rec.kind);
    REQUIRE(claim.record.version == 1);
    REQUIRE(json::Dump(claim.record.extra) == json::Dump(rec.extra));

    //  正控：状态翻转之后同一份内容仍然在（data 没有在 claiming→ready 时丢失）
    ContractOk(repo.MarkReady(pa, rec.id, 1, claim.record), "MarkReady");
    const auto got = ContractOk(repo.GetById(pa, rec.id), "GetById");
    REQUIRE(*got.data.name == "well-formed");
    REQUIRE(json::Dump(got.extra) == json::Dump(rec.extra));
  }

  // ===========================================================================
  //  ★ C2（ADR-009 §4.2/§4.3）：ReclaimStaleClaiming —— **租约驱动**的 claiming 回收
  // ===========================================================================
  //  语义登记（memory / SQLite / PG 必须逐字一致，且必须有正控）：
  //    · 只回收 `file_source ∈ live_expired_sources` **且** `created_at <= older_than` 的行；
  //    · 空集合 ⇒ 0（调用方没领到任何过期租约时的安全方向）；
  //    · 阈值早于 `created_at` ⇒ 0（二级年龄护栏）；
  //    · `limit` 逐条生效；回收后同一 file_source 可被重新领取（旧行真的没了）。
  SECTION("★ C2 reclaim：只回收 (file_source ∈ 集合 ∧ created_at <= 阈值) 的 claiming 行") {
    const std::string s1 = "/u/reclaim/one";
    const std::string s2 = "/u/reclaim/two";
    const auto r1 = MakeRecord(pa, "rcl1", s1, "reclaim-1");
    const auto r2 = MakeRecord(pa, "rcl2", s2, "reclaim-2");
    ContractOk(repo.ClaimForWrite(pa, r1), "claim r1");
    ContractOk(repo.ClaimForWrite(pa, r2), "claim r2");
    const std::int64_t created = clock.NowEpochSeconds();

    //  负断言①：空集合 → 一条都不回收（绝不凭年龄误删活 claim）
    REQUIRE(ContractOk(repo.ReclaimStaleClaiming(pa, created, 10, {}), "空集合") == 0);
    //  负断言②：年龄护栏 —— 阈值早于 created_at
    REQUIRE(ContractOk(repo.ReclaimStaleClaiming(pa, created - 1, 10, {s1, s2}),
                       "年龄护栏") == 0);
    //  负断言③：集合里只有**别的** file_source
    REQUIRE(ContractOk(repo.ReclaimStaleClaiming(pa, created, 10, {"/u/reclaim/other"}),
                       "集合不匹配") == 0);
    //  ★ 正控：三条负断言之后两行**确实还在 claiming**（否则"回收 0 条"可能只是"行不存在"）
    {
      const auto probe =
          ContractOk(repo.ClaimForWrite(pa, MakeRecord(pa, "rcl1b", s1, "probe")), "正控：行仍在");
      REQUIRE_FALSE(probe.claimed);
      REQUIRE(probe.state == domain::MetadataState::kClaiming);
      REQUIRE(probe.record.id == r1.id);
    }

    //  正例：limit 逐条生效
    REQUIRE(ContractOk(repo.ReclaimStaleClaiming(pa, created, 1, {s1, s2}), "limit=1") == 1);
    REQUIRE(ContractOk(repo.ReclaimStaleClaiming(pa, created, 10, {s1, s2}), "回收剩余") == 1);
    //  正控：两行都没了 → 同一 file_source 可被**重新领取**
    const auto reclaim1 = ContractOk(repo.ClaimForWrite(pa, r1), "回收后可重新领取 r1");
    REQUIRE(reclaim1.claimed);
    const auto reclaim2 = ContractOk(repo.ClaimForWrite(pa, r2), "回收后可重新领取 r2");
    REQUIRE(reclaim2.claimed);
    //  本 section 收尾：释放，避免影响同一 TEST_CASE 的后续 section
    ContractOk(repo.ReleaseClaim(pa, r1.id, 1), "cleanup r1");
    ContractOk(repo.ReleaseClaim(pa, r2.id, 1), "cleanup r2");
  }
}

// =============================================================================
//  四、ILeaseRepository 契约（memory / postgres 共用）
// =============================================================================
//  语义登记（依据 ADR-009 §4.3 与 `ports.h` 的端口注释；实现方必须遵守）：
//    Acquire(partition, lease_key, owner, ttl)
//        · 键不存在 或 **已过期** → 插入/接管，`expires_at` = now() + ttl
//        · 键存在且**未过期**     → kUnavailable（"租约已被占用"）
//        · ttl <= 0               → 立即过期（因此可被立刻重新 Acquire / 被 ClaimExpired 领走）
//    Renew / Release
//        · 不存在                 → kNotFound
//        · 是别人的租约           → kPermissionDenied（**不是** kNotFound —— R16：
//                                    "不存在"与"不是你的"必须可区分，否则调用方无法排障）
//    ClaimExpired(partition, limit, claimant)
//        · limit <= 0             → 空结果（不是错误）
//        · 只领取 `expires_at <= now()` 的行；原子地把 owner 改成 claimant 并把
//          expiry 推后（内存实现与 PG 实现都是 60 s），因此不会被立即重复领取
//        · 并发领取者之间**不得重复领取同一条**
//
//  ★ 移植性约定（否则同一套断言无法同时跑内存与 PG 实现）
//    ① `Lease::file_id` / `Lease::file_source` 的取值范围各实现不同：内存实现把端口
//       参数当成 `file_id`（`file_source` 留空），PG 实现的表身份列是 `file_source`
//       （`file_id` 由 `LEFT JOIN file_locations` 反解，可能为空）。因此本套件只用
//       辅助函数 `LeaseContractKey()` 判断"领到的是哪一条租约"，**不直接对这两个
//       字段断言**。
//    ② 时间源不同：内存实现用注入的 `IClock`，PG 实现用数据库 `now()`。本套件用传入的
//       `clock` 只做"expiry 落在 now + ttl 附近"的上下界断言（±5 s 容差），
//       不假设能拨动数据库时钟。
inline std::string LeaseContractKey(const domain::ILeaseRepository::Lease& lease) {
  //  两个字段里哪个是"租约键"由实现决定（见上）；至少一个是键。
  return lease.file_source.empty() ? lease.file_id : lease.file_source;
}

inline void CheckLeaseContract(domain::ILeaseRepository& repo, const fss::IClock& clock,
                               const std::string& partition_prefix = "contract-lease") {
  using domain::ILeaseRepository;
  const std::string owner = partition_prefix + "-owner";
  const std::string other = partition_prefix + "-other";
  const auto contains = [](const std::vector<ILeaseRepository::Lease>& leases,
                           const std::string& key) {
    for (const auto& lease : leases) {
      if (LeaseContractKey(lease) == key) return true;
    }
    return false;
  };

  SECTION("Acquire：返回租约键与 owner；expires_at 落在 now + ttl 附近（正例对照）") {
    const std::string p = partition_prefix + "-acquire";
    const auto lease = ContractOk(repo.Acquire(p, "lease-basic", owner, 60000), "Acquire");
    REQUIRE(LeaseContractKey(lease) == "lease-basic");
    REQUIRE(lease.owner_instance_id == owner);
    const std::int64_t now = clock.NowEpochMillis();
    CAPTURE(lease.expires_at_epoch_millis, now);
    //  下界挡"未初始化 / 秒当毫秒"，上界挡"时基错误 / ttl 没生效"
    REQUIRE(lease.expires_at_epoch_millis > now);
    REQUIRE(lease.expires_at_epoch_millis <= now + 60000 + 5000);
  }

  SECTION("Acquire：未过期不可被抢；过期后可被接管（正反两侧）") {
    const std::string p = partition_prefix + "-occupied";
    ContractOk(repo.Acquire(p, "held", owner, 60000), "首次 Acquire");
    ContractError(repo.Acquire(p, "held", other, 60000), fss::ErrorKind::kUnavailable,
                  "未过期的租约不能被第二个实例抢走");
    //  正控：ttl=0 → 立即过期，**必须**能被他人接管（证明上面的 kUnavailable 不是恒真）
    ContractOk(repo.Acquire(p, "expired", owner, 0), "ttl=0 建租约");
    ContractOk(repo.Acquire(p, "expired", other, 60000), "已过期的租约必须可被接管");
  }

  SECTION("ClaimExpired：只领过期的；未过期的**绝不在结果里**（负断言 + 同调用正控）") {
    const std::string p = partition_prefix + "-claim";
    ContractOk(repo.Acquire(p, "claim-live", owner, 60000), "有效租约（负断言对象）");
    ContractOk(repo.Acquire(p, "claim-dead", owner, 0), "过期租约（同一次调用的正控）");
    const auto claimed = ContractOk(repo.ClaimExpired(p, 10, other), "ClaimExpired");
    //  正控 ①：已过期的必须被领到 —— 证明这条查询/领取路径真的工作
    REQUIRE(contains(claimed, "claim-dead"));
    //  正控 ②：有效租约这一行确实存在（Renew 成功 = 行在、且 owner 匹配）
    ContractOk(repo.Renew(p, "claim-live", owner, 60000), "有效租约到期前 Renew 成功");
    //  负断言：有效期内的租约绝不能被领走
    REQUIRE_FALSE(contains(claimed, "claim-live"));
    for (const auto& lease : claimed) {
      REQUIRE(lease.owner_instance_id == other);
    }
  }

  SECTION("ClaimExpired：limit 生效；limit<=0 → 空；领取过的不会被重复领取") {
    const std::string p = partition_prefix + "-limit";
    for (int i = 0; i < 3; ++i) {
      ContractOk(repo.Acquire(p, "expired-" + std::to_string(i), owner, 0), "建过期租约");
    }
    const auto two = ContractOk(repo.ClaimExpired(p, 2, other), "limit=2");
    REQUIRE(two.size() == 2);  // 正控
    const auto none = ContractOk(repo.ClaimExpired(p, 0, other), "limit=0");
    REQUIRE(none.empty());
    const auto negative = ContractOk(repo.ClaimExpired(p, -1, other), "limit=-1");
    REQUIRE(negative.empty());
    //  正控：limit<=0 只是"不领"，不是"把数据丢了" —— 剩下的 1 条仍必须能领到
    const auto one = ContractOk(repo.ClaimExpired(p, 10, other), "limit=10");
    REQUIRE(one.size() == 1);
  }

  SECTION("Renew：延长过期；非本人 → kPermissionDenied；不存在 → kNotFound") {
    const std::string p = partition_prefix + "-renew";
    ContractOk(repo.Acquire(p, "renew-me", owner, 0), "ttl=0（本来立刻可领）");
    ContractOk(repo.Acquire(p, "renew-control", owner, 0), "正控：另一条过期租约");
    ContractOk(repo.Renew(p, "renew-me", owner, 60000), "Renew 延长到 60 s");
    const auto claimed = ContractOk(repo.ClaimExpired(p, 10, other), "ClaimExpired");
    //  正控：control 被领到 → "过期可领"这条路径有效
    REQUIRE(contains(claimed, "renew-control"));
    //  负断言：已续到未来的 renew-me 不得被领走（证明 Renew 真的改了 expiry）
    REQUIRE_FALSE(contains(claimed, "renew-me"));
    //  非本人续租 → kPermissionDenied（R1 注入点：去掉 owner 条件后这条必须红）
    ContractError(repo.Renew(p, "renew-me", other, 60000), fss::ErrorKind::kPermissionDenied,
                  "非本人 Renew 必须 kPermissionDenied");
    ContractError(repo.Renew(p, "no-such-lease", owner, 1000), fss::ErrorKind::kNotFound,
                  "Renew 不存在的租约 → kNotFound");
    //  正控：本人续租仍然成功（证明上面的 kPermissionDenied 不是"Renew 一律失败"）
    ContractOk(repo.Renew(p, "renew-me", owner, 60000), "本人 Renew 成功");
  }

  SECTION("Release：本人释放后即消失；非本人 → kPermissionDenied；不存在 → kNotFound") {
    const std::string p = partition_prefix + "-release";
    ContractOk(repo.Acquire(p, "release-me", owner, 0), "过期租约");
    ContractOk(repo.Acquire(p, "release-control", owner, 0), "正控：另一条过期租约");
    //  非本人不能释放（且必须**不删**）
    ContractError(repo.Release(p, "release-me", other), fss::ErrorKind::kPermissionDenied,
                  "非本人 Release 必须 kPermissionDenied");
    ContractError(repo.Release(p, "no-such-lease", owner), fss::ErrorKind::kNotFound,
                  "Release 不存在的租约 → kNotFound");
    ContractOk(repo.Release(p, "release-me", owner), "本人 Release");
    const auto claimed = ContractOk(repo.ClaimExpired(p, 10, other), "ClaimExpired");
    //  正控：control 必须被领到（证明"过期 → 可领"路径有效）
    REQUIRE(contains(claimed, "release-control"));
    //  负断言：已释放的租约不得再出现（Release 是**删除**，不是改 owner）
    REQUIRE_FALSE(contains(claimed, "release-me"));
    //  正控：释放后同一个键可以重新 Acquire
    ContractOk(repo.Acquire(p, "release-me", owner, 60000), "释放后可重新 Acquire");
  }

  SECTION("ClaimExpired：两个并发领取者不得拿到同一条租约（不重不漏）") {
    const std::string p = partition_prefix + "-concurrent";
    constexpr int kCount = 40;
    for (int i = 0; i < kCount; ++i) {
      ContractOk(repo.Acquire(p, "c-" + std::to_string(i), owner, 0), "建过期租约");
    }
    std::vector<ILeaseRepository::Lease> first;
    std::vector<ILeaseRepository::Lease> second;
    std::mutex gate_mutex;
    std::condition_variable gate;
    bool go = false;
    auto worker = [&](std::vector<ILeaseRepository::Lease>* out, const std::string& claimant) {
      {
        std::unique_lock<std::mutex> lock(gate_mutex);
        gate.wait(lock, [&] { return go; });
      }
      auto claimed = repo.ClaimExpired(p, kCount, claimant);
      if (claimed.ok()) *out = std::move(claimed).value();
    };
    std::thread one(worker, &first, partition_prefix + "-c1");
    std::thread two(worker, &second, partition_prefix + "-c2");
    {
      std::lock_guard<std::mutex> lock(gate_mutex);
      go = true;
    }
    gate.notify_all();
    one.join();
    two.join();

    std::set<std::string> keys;
    for (const auto& lease : first) keys.insert(LeaseContractKey(lease));
    for (const auto& lease : second) keys.insert(LeaseContractKey(lease));
    //  原子领取的两个判据：**不重复**（并集大小 == 总数）与**不遗漏**（并集覆盖全部）。
    //  任何一条被两个领取者同时拿到 → 并集 < 总数 → 失败。
    //  ★ 这一条是"真实并发"用例，但重复领取是否出现仍可能受调度影响；不加锁也能
    //    确定性判定的版本在 PG 侧（`test_postgres_repositories.cpp` 的"锁住行再领"用例）。
    CAPTURE(first.size(), second.size(), keys.size());
    REQUIRE(keys.size() == static_cast<std::size_t>(kCount));
  }
}

}  // namespace fss::test
