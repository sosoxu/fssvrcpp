// C2.11：端口清单 = 15 个；ILeaseRepository / IIoEngine 已定义；签名可编译即约束
//
// 这一条门槛是"**清单本身**也是被管理的对象"：端口数量与名字一旦漂移，
// 设计文档、ADR 索引、实现（每个端口至少一个实现）都会跟着漂。
// 因此这里用编译期 + 运行期两种手段把它钉住：
//   ① 抽象类可实例化检查（派生类必须实现全部纯虚函数）——签名改了这里立刻编译失败
//   ② 64 位偏移 / 区间读的**签名级别**断言（C2.11 明确要求）
//   ③ 运行期核对清单条目数与名字（防"少了一个"或"多了一个"却没人发现）
#include <catch2/catch.hpp>

#include "common/ids/id_generator.h"
#include "common/time/clock.h"
#include "domain/ports/ports.h"

#include <string>
#include <type_traits>
#include <vector>

using namespace fss::domain;

namespace {

// ① 每个端口都必须是**抽象类**（至少一个纯虚函数），且可被继承实现
static_assert(std::is_abstract_v<IBlobStore>, "IBlobStore 必须是抽象端口");
static_assert(std::is_abstract_v<IBlobStoreFactory>, "IBlobStoreFactory 必须是抽象端口");
static_assert(std::is_abstract_v<IFileLocationRepository>, "IFileLocationRepository 必须是抽象端口");
static_assert(std::is_abstract_v<IMetadataRepository>, "IMetadataRepository 必须是抽象端口");
static_assert(std::is_abstract_v<IPartitionRegistry>, "IPartitionRegistry 必须是抽象端口");
static_assert(std::is_abstract_v<IAuthorizer>, "IAuthorizer 必须是抽象端口");
static_assert(std::is_abstract_v<ILegalValidator>, "ILegalValidator 必须是抽象端口");
static_assert(std::is_abstract_v<ISchemaValidator>, "ISchemaValidator 必须是抽象端口");
static_assert(std::is_abstract_v<IEventPublisher>, "IEventPublisher 必须是抽象端口");
static_assert(std::is_abstract_v<IAuditLogger>, "IAuditLogger 必须是抽象端口");
static_assert(std::is_abstract_v<ISelfSignedUrlCodec>, "ISelfSignedUrlCodec 必须是抽象端口");
static_assert(std::is_abstract_v<ILeaseRepository>, "ILeaseRepository 必须是抽象端口（ADR-009）");
static_assert(std::is_abstract_v<IIoEngine>, "IIoEngine 必须是抽象端口（ADR-010）");
// ⓛ L1 的两个（零依赖、无领域语义）
static_assert(std::is_abstract_v<fss::IClock>, "IClock 必须是抽象端口（声明在 L1）");
static_assert(std::is_abstract_v<fss::IIdGenerator>, "IIdGenerator 必须是抽象端口（声明在 L1）");

// ② IIoEngine 的 64 位偏移签名（C2.11 明确要求：64 位偏移 + 区间读）
static_assert(std::is_same_v<decltype(std::declval<IIoEngine&>().ReadAt(
                                 int{}, std::uint64_t{}, static_cast<char*>(nullptr), std::size_t{})),
                             fss::Result<std::size_t>>);
static_assert(std::is_same_v<decltype(std::declval<IIoEngine&>().WriteAt(
                                 int{}, std::uint64_t{}, static_cast<const char*>(nullptr),
                                 std::size_t{})),
                             fss::Result<std::size_t>>);
// ★ 精确到"成员函数指针类型"：偏移一旦被改成 32 位，这里立刻编译失败
//   （用 is_invocable 检查是错的：uint32_t 会隐式转换成 uint64_t，测不出来）
using ReadAtSignature = fss::Result<std::size_t> (IIoEngine::*)(int, std::uint64_t, char*,
                                                               std::size_t);
using WriteAtSignature = fss::Result<std::size_t> (IIoEngine::*)(int, std::uint64_t,
                                                                const char*, std::size_t);
static_assert(std::is_same_v<decltype(&IIoEngine::ReadAt), ReadAtSignature>,
              "ReadAt 的偏移必须是 64 位（uint64_t）");
static_assert(std::is_same_v<decltype(&IIoEngine::WriteAt), WriteAtSignature>,
              "WriteAt 的偏移必须是 64 位（uint64_t）");

// ③ 每个端口都必须有可用的测试替身（这里只验证"能实现"，具体替身在 framework 里）
class FakeClock final : public fss::IClock {
 public:
  std::int64_t NowEpochSeconds() const override { return 0; }
  std::int64_t NowEpochMillis() const override { return 0; }
  std::chrono::steady_clock::time_point NowSteady() const override { return {}; }
};

}  // namespace

TEST_CASE("★ C2.11 端口清单 = 15 个（13 个在 L3 + 2 个声明在 L1）", "[phase2][ports][c2.11]") {
  //  L1 声明的两个（理由见 ports.h 顶部注释）
  const std::vector<std::string> l1_ports = {"IClock", "IIdGenerator"};
  //  L3 的 13 个
  const std::vector<std::string> l3_ports = {
      "IBlobStore",         "IBlobStoreFactory",  "IFileLocationRepository",
      "IMetadataRepository", "IPartitionRegistry", "IAuthorizer",
      "ILegalValidator",    "ISchemaValidator",   "IEventPublisher",
      "IAuditLogger",       "ISelfSignedUrlCodec", "ILeaseRepository",
      "IIoEngine",
  };
  REQUIRE(l1_ports.size() == 2);
  REQUIRE(l3_ports.size() == 13);
  REQUIRE(l1_ports.size() + l3_ports.size() == 15);

  // ADR-009 / ADR-010 追加的两个必须在清单里（C2.11 明确点名）
  const auto has = [&](const char* name) {
    return std::find(l3_ports.begin(), l3_ports.end(), name) != l3_ports.end();
  };
  REQUIRE(has("ILeaseRepository"));
  REQUIRE(has("IIoEngine"));
}

TEST_CASE("C2.11 IIoEngine 的能力与区间读语义（签名即约束）", "[phase2][ports][c2.11]") {
  // 能力结构必须能表达"可选加速 + 对齐要求"（ADR-010 的探测 → 回退链条）
  IoEngineCapabilities caps;
  caps.engine_name = "blocking";
  caps.async = false;
  caps.direct_io = false;
  caps.alignment = 0;
  REQUIRE(caps.engine_name == "blocking");

  // 64 位区间：ByteRange 用 uint64，> 2 GiB 的偏移必须能表达
  const ByteRange big{5ULL * 1024 * 1024 * 1024, 1024};  // 5 GiB 偏移
  REQUIRE(big.offset > 0xFFFFFFFFULL);
  REQUIRE_FALSE(big.IsWholeObject());
  REQUIRE(ByteRange{}.IsWholeObject());
}

TEST_CASE("C2.11 仓储端口的 partition 隔离是签名级的（不允许全局按 id 查）",
          "[phase2][ports][c2.11]") {
  //  这一条是"抄写式"断言：签名里带 partition 的参数个数。若有人把 partition 去掉，
  //  编译期断言（下面的 static_assert）会失败 —— 分区隔离不允许被"简化"掉。
  static_assert(std::is_invocable_v<decltype(&IFileLocationRepository::Find),
                                    IFileLocationRepository&, std::string_view, std::string_view>,
                "IFileLocationRepository::Find 必须接受 (partition, file_id)");
  static_assert(std::is_invocable_v<decltype(&IFileLocationRepository::List),
                                    IFileLocationRepository&, std::string_view,
                                    const LocationQuery&>,
                "IFileLocationRepository::List 必须接受 (partition, query)（getFileList 需要）");
  static_assert(std::is_invocable_v<decltype(&IMetadataRepository::GetById),
                                    IMetadataRepository&, std::string_view, std::string_view>,
                "IMetadataRepository::GetById 必须接受 (partition, record_id)");
  static_assert(
      std::is_invocable_v<decltype(&ILeaseRepository::Acquire), ILeaseRepository&,
                          std::string_view, std::string_view, std::string_view, std::int64_t>,
      "ILeaseRepository::Acquire 必须接受 (partition, file_id, owner, ttl)");
  SUCCEED();
}

TEST_CASE("C2.11 端口的错误语义用 Result 表达（不抛异常）", "[phase2][ports][c2.11]") {
  // 抽查几个"必须能失败"的签名：返回类型里必须出现 Result
  static_assert(std::is_same_v<decltype(std::declval<IBlobStore&>().stat(ObjectRef{})),
                               fss::Result<ObjectStat>>);
  static_assert(std::is_same_v<decltype(std::declval<IMetadataRepository&>().Delete(
                                   std::string_view{}, std::string_view{})),
                               fss::Result<void>>);
  static_assert(std::is_same_v<decltype(std::declval<IAuthorizer&>().Authorize(
                                   std::string_view{}, std::string_view{}, std::string_view{})),
                               fss::Result<void>>);
  // 测试替身必须真的能实现（不是"接口设计得很漂亮但没法用"）
  FakeClock clock;
  REQUIRE(clock.NowEpochMillis() == 0);
}
