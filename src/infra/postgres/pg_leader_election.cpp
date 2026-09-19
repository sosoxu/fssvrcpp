// PgLeaderElection 实现。设计、锁连接生命周期与 PG 12.6 的实测依据见头文件。
#include "infra/postgres/pg_leader_election.h"

#include <utility>

namespace fss::infra {

namespace {

//  单参数 bigint 版本 = 会话级 advisory lock（见头文件）。
constexpr const char* kTryLockSql = R"pglock(SELECT pg_try_advisory_lock($1::bigint))pglock";
constexpr const char* kUnlockSql = R"pglock(SELECT pg_advisory_unlock($1::bigint))pglock";
//  廉价往返：把"锁连接真的活着"变成一次真实 I/O（PQstatus 可能过时，见头文件）。
constexpr const char* kPingSql = R"pglock(SELECT 1)pglock";

}  // namespace

fss::Result<std::unique_ptr<PgLeaderElection>> PgLeaderElection::Open(
    PgLeaderElectionOptions options) {
  //  fail-closed：没有 DSN 就没有共享数据库，也就没有"跨实例互斥"这回事。
  if (options.pg.dsn.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument,
               "leader_election.enabled=true 需要 metadata.postgres.dsn"
               "（会话级 PG advisory lock 必须连到所有实例共享的数据库）");
  }
  //  `max_connections` 对本类无意义（只用一条专用连接），但 `ValidatePgOptions`
  //  要求 > 0；缺省补 1。
  if (options.pg.max_connections <= 0) options.pg.max_connections = 1;
  if (const auto valid = ValidatePgOptions(options.pg); !valid.ok()) return valid.error();

  auto connection = PgConnection::Connect(options.pg);
  if (!connection.ok()) {
    return Annotate(connection.error(), "建立 leader election 锁连接失败");
  }

  auto election = std::unique_ptr<PgLeaderElection>(new PgLeaderElection());
  election->options_ = std::move(options);
  election->connection_ = std::move(connection).value();
  return election;
}

PgLeaderElection::~PgLeaderElection() {
  //  析构 = 会话结束 → PG 自动释放本会话持有的全部 advisory lock。
  //  （进程崩溃时同理：这正是"锁连接必须与数据连接分离 + 只跑短语句"的原因。）
}

fss::Result<bool> PgLeaderElection::TryAcquire() {
  if (!connection_) {
    return Err(fss::ErrorKind::kUnavailable,
               "leader election 锁连接不可用：" +
                   (last_error_.empty() ? std::string("连接未建立") : last_error_));
  }
  const auto result =
      connection_->ExecParams(kTryLockSql, {std::to_string(options_.lock_key)});
  if (!result.ok()) {
    last_error_ = result.error().message();
    return Annotate(result.error(), "获取 PG advisory lock 失败（lock_key=" +
                                        std::to_string(options_.lock_key) + "）");
  }
  if (result.value().RowCount() < 1) {
    return Err(fss::ErrorKind::kInternal,
               "pg_try_advisory_lock 没有返回行（lock_key=" +
                   std::to_string(options_.lock_key) + "）");
  }
  const std::string value = result.value().Value(0, 0);
  leader_ = (value == "t" || value == "true");
  return leader_;
}

fss::Result<void> PgLeaderElection::Release() {
  if (!connection_) {
    leader_ = false;
    return Ok();
  }
  leader_ = false;
  const auto result =
      connection_->ExecParams(kUnlockSql, {std::to_string(options_.lock_key)});
  if (!result.ok()) {
    last_error_ = result.error().message();
    return Annotate(result.error(), "释放 PG advisory lock 失败");
  }
  return Ok();
}

bool PgLeaderElection::IsLeader() {
  if (!connection_) return false;

  //  ---- 不是 leader：尝试接管（短语句，一次往返）----
  //  ★ 这一步让"选举"具备 failover：当前 leader 崩溃、PG 结束其会话并释放锁之后，
  //    其它实例会在下一轮 GC 时拿到锁。没有它，选举只是启动期的一次性动作。
  if (!leader_) {
    const auto acquired = TryAcquire();
    if (!acquired.ok()) {
      last_error_ = acquired.error().message();
      return false;
    }
    return acquired.value();
  }

  //  ---- 已是 leader：验证锁连接**真的**还活着（不能只信缓存的 bool）----
  if (!connection_->Healthy()) {
    //  PQstatus 已明确说连接坏了：丢弃连接（结束会话 → PG 释放锁），本实例降级。
    last_error_ = connection_->LastError();
    connection_.reset();
    leader_ = false;
    return false;
  }
  const auto ping = connection_->ExecSimple(kPingSql);
  if (!ping.ok()) {
    //  往返失败（例如连接已被对端关闭、或 statement_timeout 取消了这条最短语句）：
    //  丢弃连接是**安全方向** —— 我们无法确认锁还在，就绝不能再以 leader 身份跑单例任务；
    //  结束会话同时保证 PG 释放锁，其它实例随后可以接管。
    last_error_ = ping.error().message();
    connection_.reset();
    leader_ = false;
    return false;
  }
  return true;
}

bool PgLeaderElection::Ready() const {
  return connection_ != nullptr && connection_->Healthy();
}

std::string PgLeaderElection::NotReadyReason() const {
  if (connection_ == nullptr) {
    return last_error_.empty() ? std::string("leader election 锁连接未建立") : last_error_;
  }
  if (!connection_->Healthy()) {
    return "leader election 锁连接不健康（PQstatus != CONNECTION_OK）：" +
           connection_->LastError();
  }
  return {};
}

}  // namespace fss::infra
