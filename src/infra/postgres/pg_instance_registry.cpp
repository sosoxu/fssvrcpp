// instance_registry 实现。设计、版本兼容规则与 SQL 纪律见头文件。
#include "infra/postgres/pg_instance_registry.h"

#include "infra/postgres/pg_connection.h"

#include <cstddef>
#include <cstdlib>
#include <string>
#include <utility>

namespace fss::infra {

namespace {

//  `major.minor` 解析：先砍掉 `-rc1` / `+build` 之类的后缀，再取前两段并要求都是
//  十进制数字。任一步不成立 → false（调用方退化为逐字相等比较，fail-closed）。
bool ParseMajorMinor(std::string_view version, long* major, long* minor) {
  const std::size_t cut = version.find_first_of("-+");
  const std::string_view core = version.substr(0, cut == std::string_view::npos ? version.size() : cut);
  const std::size_t first_dot = core.find('.');
  if (first_dot == std::string_view::npos) return false;
  const std::size_t second_dot = core.find('.', first_dot + 1);
  const std::string_view major_text = core.substr(0, first_dot);
  const std::string_view minor_text =
      core.substr(first_dot + 1, second_dot == std::string_view::npos
                                     ? std::string_view::npos
                                     : second_dot - first_dot - 1);
  if (major_text.empty() || minor_text.empty()) return false;
  for (const std::string_view text : {major_text, minor_text}) {
    for (const char ch : text) {
      if (ch < '0' || ch > '9') return false;
    }
  }
  char* end = nullptr;
  const std::string major_str(major_text);
  const std::string minor_str(minor_text);
  const long parsed_major = std::strtol(major_str.c_str(), &end, 10);
  if (end == major_str.c_str() || *end != '\0') return false;
  const long parsed_minor = std::strtol(minor_str.c_str(), &end, 10);
  if (end == minor_str.c_str() || *end != '\0') return false;
  *major = parsed_major;
  *minor = parsed_minor;
  return true;
}

//  可读缩写：长版本/hash 只展示前 12 个字符（原因面向运维，不需要全量）。
std::string Abbreviate(const std::string& text) {
  constexpr std::size_t kKeep = 12;
  if (text.size() <= kKeep) return text;
  return text.substr(0, kKeep) + "…";
}

std::string RenderVersion(const std::string& version) {
  return version.empty() ? std::string("（空）") : version;
}

std::string RenderHash(const std::string& hash) {
  return hash.empty() ? std::string("（空）") : Abbreviate(hash);
}

}  // namespace

bool ServiceVersionCompatible(std::string_view self, std::string_view peer) {
  long self_major = 0;
  long self_minor = 0;
  long peer_major = 0;
  long peer_minor = 0;
  if (ParseMajorMinor(self, &self_major, &self_minor) &&
      ParseMajorMinor(peer, &peer_major, &peer_minor)) {
    return self_major == peer_major && self_minor == peer_minor;
  }
  return self == peer;
}

std::string PeerConsistencyReason(const InstancePeer& peer,
                                  std::string_view self_service_version,
                                  std::string_view self_config_hash) {
  if (!ServiceVersionCompatible(self_service_version, peer.service_version)) {
    return "实例 " + peer.instance_id + " 的服务版本与本实例不兼容（对端 " +
           RenderVersion(peer.service_version) + "，本实例 " +
           RenderVersion(std::string(self_service_version)) +
           "）：滚动升级期间新旧 major/minor 不得同时服务（readiness fail-closed）。";
  }
  if (peer.config_hash != self_config_hash) {
    return "实例 " + peer.instance_id + " 的 config_hash 与本实例不一致（对端 " +
           RenderHash(peer.config_hash) + "，本实例 " +
           RenderHash(std::string(self_config_hash)) +
           "）：配置不同的实例不得同时服务（滚动升级/误配），readiness fail-closed。";
  }
  return {};
}

PgInstanceRegistry::PgInstanceRegistry(PgPool& pool, std::string instance_id,
                                       std::string service_version, std::string config_hash)
    : pool_(pool),
      instance_id_(std::move(instance_id)),
      service_version_(std::move(service_version)),
      config_hash_(std::move(config_hash)) {}

PgInstanceRegistry::~PgInstanceRegistry() {
  //  尽力而为：失败不抛（析构可能在栈展开里跑）。真正的错误已在启动/心跳路径上报。
  (void)RemoveSelf();
}

fss::Result<void> PgInstanceRegistry::UpsertSelf() {
  if (instance_id_.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument,
               "instance_registry 注册失败：本实例的 instance_id 为空"
               "（组合根必须先确定实例标识）");
  }
  FSS_TRY(handle, pool_.Borrow());
  //  不访问分区表 → 独立定界符（见头文件的 SQL 纪律说明）。
  const auto result = handle->ExecParams(
      R"pgsql(INSERT INTO instance_registry
                (instance_id, service_version, config_hash, started_at, heartbeat_at)
              VALUES ($1, $2, $3, now(), now())
              ON CONFLICT (instance_id) DO UPDATE
                SET service_version = EXCLUDED.service_version,
                    config_hash     = EXCLUDED.config_hash,
                    heartbeat_at    = now())pgsql",
      {instance_id_, service_version_, config_hash_});
  if (!result.ok()) {
    return Annotate(result.error(),
                    "instance_registry 注册失败（实例 " + instance_id_ +
                        "）：请确认已执行 db/migrations/001_init.sql");
  }
  return Ok();
}

fss::Result<void> PgInstanceRegistry::TouchHeartbeat() {
  FSS_TRY(handle, pool_.Borrow());
  const auto result = handle->ExecParams(
      R"pgsql(UPDATE instance_registry SET heartbeat_at = now() WHERE instance_id = $1)pgsql",
      {instance_id_});
  if (!result.ok()) {
    return Annotate(result.error(), "instance_registry 心跳写入失败（实例 " + instance_id_ + "）");
  }
  if (result.value().AffectedRows() <= 0) {
    //  行被清理掉了（例如本实例心跳长时间停滞被别的实例判为过老）——
    //  重新注册，避免"心跳一直失败 → 永久 not ready"。
    return UpsertSelf();
  }
  return Ok();
}

fss::Result<std::vector<InstancePeer>> PgInstanceRegistry::ListLivePeers(
    int liveness_seconds) const {
  if (liveness_seconds <= 0) {
    return Err(fss::ErrorKind::kInvalidArgument,
               "instance_registry 读取失败：liveness_seconds 必须为正");
  }
  FSS_TRY(handle, pool_.Borrow());
  const auto result = handle->ExecParams(
      R"pgsql(SELECT instance_id, service_version, config_hash
              FROM instance_registry
              WHERE instance_id <> $1
                AND heartbeat_at > now() - ($2::int * INTERVAL '1 second')
              ORDER BY instance_id)pgsql",
      {instance_id_, std::to_string(liveness_seconds)});
  if (!result.ok()) {
    return Annotate(result.error(), "instance_registry 读取 live peer 失败");
  }
  std::vector<InstancePeer> peers;
  peers.reserve(static_cast<std::size_t>(result.value().RowCount()));
  for (int row = 0; row < result.value().RowCount(); ++row) {
    InstancePeer peer;
    peer.instance_id = result.value().Value(row, 0);
    peer.service_version = result.value().Value(row, 1);
    peer.config_hash = result.value().Value(row, 2);
    peers.push_back(std::move(peer));
  }
  return peers;
}

fss::Result<void> PgInstanceRegistry::RemoveSelf() {
  FSS_TRY(handle, pool_.Borrow());
  const auto result = handle->ExecParams(
      R"pgsql(DELETE FROM instance_registry WHERE instance_id = $1)pgsql", {instance_id_});
  if (!result.ok()) {
    return Annotate(result.error(), "instance_registry 注销失败（实例 " + instance_id_ + "）");
  }
  return Ok();
}

fss::Result<int> PgInstanceRegistry::CleanupStale(int stale_seconds) {
  if (stale_seconds <= 0) {
    return Err(fss::ErrorKind::kInvalidArgument,
               "instance_registry 清理失败：stale_seconds 必须为正");
  }
  FSS_TRY(handle, pool_.Borrow());
  //  只删"心跳远早于现在"的行；本实例的行不删（由 RemoveSelf 负责）。
  const auto result = handle->ExecParams(
      R"pgsql(DELETE FROM instance_registry
              WHERE instance_id <> $1
                AND heartbeat_at < now() - ($2::int * INTERVAL '1 second'))pgsql",
      {instance_id_, std::to_string(stale_seconds)});
  if (!result.ok()) {
    return Annotate(result.error(), "instance_registry 清理过老行失败");
  }
  return static_cast<int>(result.value().AffectedRows());
}

}  // namespace fss::infra
