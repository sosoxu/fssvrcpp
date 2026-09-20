// LeaseRenewer 实现。语义与失败处理见头文件。
#include "app/services/lease_renewer.h"

#include <algorithm>
#include <system_error>
#include <utility>

namespace fss::app {

LeaseRenewer::LeaseRenewer(domain::ILeaseRepository& leases, std::string partition,
                           std::string lease_key, std::string instance_id, Options options)
    : leases_(leases),
      partition_(std::move(partition)),
      lease_key_(std::move(lease_key)),
      instance_id_(std::move(instance_id)),
      options_(options) {
  if (!options_.enabled) return;  // ★ 不启用 → 不创建线程（单实例行为逐字不变）
  //  续租间隔必须有下界：0 / 负值会让后台线程变成忙循环（schema 已限制 ≥1s，
  //  但本类是 L4 可复用组件，不能假设调用方永远来自配置面）。
  if (options_.renew_interval_millis <= 0) options_.renew_interval_millis = 1000;
  if (options_.ttl_millis <= 0) options_.ttl_millis = 1000;
  try {
    thread_ = std::thread([this] { Loop(); });
  } catch (const std::system_error& error) {
    //  ★ 资源耗尽时线程创建会抛异常；绝不让它逃出用例（AGENTS：逃出 main = 139）。
    //    记录成"续租失败" → 调用方 fail-closed。
    std::lock_guard<std::mutex> guard(mutex_);
    last_error_ = Err(fss::ErrorKind::kUnavailable,
                      std::string("续租线程创建失败：") + error.what());
  }
}

LeaseRenewer::~LeaseRenewer() { Stop(); }

bool LeaseRenewer::active() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return thread_.joinable();
}

std::optional<fss::Error> LeaseRenewer::last_error() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return last_error_;
}

bool LeaseRenewer::failed() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return last_error_.has_value();
}

void LeaseRenewer::Stop() {
  {
    std::lock_guard<std::mutex> guard(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void LeaseRenewer::Loop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stop_) {
    const auto interval = std::chrono::milliseconds(options_.renew_interval_millis);
    if (cv_.wait_for(lock, interval, [this] { return stop_; })) break;
    lock.unlock();
    const auto renewed =
        leases_.Renew(partition_, lease_key_, instance_id_, options_.ttl_millis);
    lock.lock();
    if (renewed.ok()) {
      last_error_.reset();  // 成功了 → 之前的瞬时错误自愈
    } else {
      last_error_ = renewed.error();  // ★ 记录最近一次失败（绝不静默忽略）
    }
  }
}

}  // namespace fss::app
