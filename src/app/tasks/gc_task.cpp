// GcTask 实现。语义与铁律见头文件与 `docs/02-design.md` §9.3。
#include "app/tasks/gc_task.h"

#include "app/services/object_key_policy.h"

#include <algorithm>
#include <string>
#include <utility>

namespace fss::app {

namespace {

//  位置记录里的物理引用键（与 LocationIssuer 使用同一对键名）
constexpr std::string_view kExtraContainer = "container";
constexpr std::string_view kExtraObjectKey = "object_key";

std::string PairKey(std::string_view container, std::string_view key) {
  return std::string(container) + "\x1f" + std::string(key);
}

}  // namespace

bool GcTask::HasMetadataRecord(std::string_view partition, std::string_view file_source) {
  if (file_source.empty()) return false;
  return ports_.metadata.GetLatestByFileSource(partition, file_source).ok();
}

void GcTask::DeleteCandidate(const GcOptions& options, domain::IBlobStore& store,
                             const domain::ObjectRef& ref, std::string_view file_id,
                             bool delete_location, std::string_view partition, GcReport& report) {
  if (options.dry_run) {
    //  ★ dry-run 也要把候选报出来（否则"只记录不删除"无从审计）
    ++report.deleted_objects;
    report.deleted_file_ids.emplace_back(file_id);
    return;
  }
  const auto removed = store.remove(ref);
  if (!removed.ok()) {
    ++report.errors;
    return;
  }
  ++report.deleted_objects;
  report.deleted_file_ids.emplace_back(file_id);
  if (delete_location) {
    if (ports_.locations.Delete(partition, file_id).ok()) ++report.deleted_locations;
  }
}

void GcTask::CollectExpiredLeases(std::string_view partition, const GcOptions& options,
                                  domain::IBlobStore& staging, domain::IBlobStore& persistent,
                                  std::int64_t now_seconds, GcReport& report) {
  (void)now_seconds;
  //  ★ 原子领取：并发 GC 下同一条租约只会被一个实例领走（`FOR UPDATE SKIP LOCKED` 等价的语义）。
  //    领到之后**仍然**要再看一次元数据记录 —— 纵深防御，别把"领到"当成"可以删"。
  const auto claimed = leases_.ClaimExpired(partition, options.claim_limit, instance_id_);
  if (!claimed.ok()) {
    ++report.errors;
    return;
  }
  report.expired_leases_claimed = static_cast<std::int64_t>(claimed.value().size());

  for (const auto& lease : claimed.value()) {
    const auto location = ports_.locations.Find(partition, lease.file_id);
    if (!location.ok()) {
      //  位置记录已经不存在：对象引用无从谈起（可能上一轮已经清过）→ 什么都不做
      ++report.skipped_no_location;
      continue;
    }
    if (HasMetadataRecord(partition, location.value().file_source)) {
      ++report.skipped_has_record;  // ★ 有记录 → 永不删
      continue;
    }
    const auto ref = ObjectRefFromLocation(location.value());
    if (!ref.ok()) {
      ++report.errors;
      continue;
    }
    domain::IBlobStore& store =
        location.value().zone == domain::StorageZone::kPersistent ? persistent : staging;
    DeleteCandidate(options, store, ref.value(), lease.file_id, /*delete_location=*/true,
                    partition, report);
  }
}

Result<std::map<std::string, std::pair<std::string, std::string>>> GcTask::ReferencedRefs(
    std::string_view partition) {
  std::map<std::string, std::pair<std::string, std::string>> refs;
  int offset = 0;
  while (true) {
    domain::LocationQuery query;
    query.limit = 500;
    query.offset = offset;
    const auto page = ports_.locations.List(partition, query);
    if (!page.ok()) return page.error();
    for (const auto& location : page.value().records) {
      if (!location.extra.is_object()) continue;
      const auto c = location.extra.find(std::string(kExtraContainer));
      const auto k = location.extra.find(std::string(kExtraObjectKey));
      if (c == location.extra.end() || k == location.extra.end()) continue;
      if (!c->is_string() || !k->is_string()) continue;
      refs[PairKey(c->get<std::string>(), k->get<std::string>())] = {location.file_id,
                                                                    location.file_source};
    }
    offset += static_cast<int>(page.value().records.size());
    if (page.value().records.empty() || offset >= static_cast<int>(page.value().total)) break;
  }
  return refs;
}

void GcTask::CollectExpiredStaging(std::string_view partition, const GcOptions& options,
                                   domain::IBlobStore& staging, std::int64_t now_seconds,
                                   GcReport& report) {
  //  ★ 单实例降级路径（`require_lease_expiry=false`）。多实例下这条路径被启动校验禁止：
  //    它的判据是"对象够旧"，而"够旧"无法区分"在途上传卡住了"与"上传者已经放弃"。
  const auto container = ObjectKeyPolicy::ContainerFor(partition, domain::StorageZone::kStaging);
  if (!container.ok()) {
    ++report.errors;
    return;
  }
  const auto refs = ReferencedRefs(partition);
  if (!refs.ok()) {
    ++report.errors;
    return;
  }
  const std::int64_t cutoff = now_seconds - options.staging_ttl_hours * 3600;

  std::string token;
  do {
    const auto page = staging.list(container.value(), "", token, 500);
    if (!page.ok()) {
      ++report.errors;
      return;
    }
    for (const auto& entry : page.value().entries) {
      if (entry.last_modified_epoch_seconds > cutoff) {
        ++report.skipped_too_young;
        continue;
      }
      const auto it = refs.value().find(PairKey(container.value(), entry.key));
      if (it == refs.value().end()) {
        //  没有任何位置记录引用它 → 直接是残留对象（`.tmp_*` 之类由 P9 C9.25 处理）
        domain::ObjectRef ref;
        ref.container = container.value();
        ref.key = entry.key;
        DeleteCandidate(options, staging, ref, entry.key, /*delete_location=*/false, partition,
                        report);
        continue;
      }
      if (HasMetadataRecord(partition, it->second.second)) {
        ++report.skipped_has_record;
        continue;
      }
      domain::ObjectRef ref;
      ref.container = container.value();
      ref.key = entry.key;
      DeleteCandidate(options, staging, ref, it->second.first, /*delete_location=*/true, partition,
                      report);
    }
    token = page.value().continuation_token;
  } while (!token.empty());
}

void GcTask::CollectOrphanObjects(std::string_view partition, const GcOptions& options,
                                  domain::IBlobStore& persistent, std::int64_t now_seconds,
                                  GcReport& report) {
  const auto container = ObjectKeyPolicy::ContainerFor(partition, domain::StorageZone::kPersistent);
  if (!container.ok()) {
    ++report.errors;
    return;
  }
  const auto refs = ReferencedRefs(partition);
  if (!refs.ok()) {
    ++report.errors;
    return;
  }
  const std::int64_t cutoff = now_seconds - options.orphan_grace_hours * 3600;

  std::string token;
  do {
    const auto page = persistent.list(container.value(), "", token, 500);
    if (!page.ok()) {
      ++report.errors;
      return;
    }
    for (const auto& entry : page.value().entries) {
      if (refs.value().count(PairKey(container.value(), entry.key)) != 0) continue;  // 有位置记录
      if (entry.last_modified_epoch_seconds > cutoff) {
        ++report.skipped_too_young;
        continue;
      }
      domain::ObjectRef ref;
      ref.container = container.value();
      ref.key = entry.key;
      //  ★ 孤儿：**没有**任何位置记录引用它（记录与位置记录是成对删除的，
      //    所以"无位置记录"等价于"没有元数据指向它"）。有记录的对象在上一步就被排除了。
      DeleteCandidate(options, persistent, ref, entry.key, /*delete_location=*/false, partition,
                      report);
    }
    token = page.value().continuation_token;
  } while (!token.empty());
}

Result<GcReport> GcTask::Run(std::string_view partition, const GcOptions& options) {
  if (partition.empty()) {
    return Err(fss::ErrorKind::kInvalidArgument, "GC 必须指定 partition");
  }
  if (options.claim_limit <= 0) {
    return Err(fss::ErrorKind::kInvalidArgument, "claim_limit 必须 > 0");
  }
  GcReport report;
  report.dry_run = options.dry_run;

  const auto staging = ports_.blobs.ForPartition(partition, domain::StorageZone::kStaging);
  if (!staging.ok()) {
    return Err(fss::ErrorKind::kUnavailable, "无法解析 staging 存储：" + staging.error().message());
  }
  const auto persistent = ports_.blobs.ForPartition(partition, domain::StorageZone::kPersistent);
  if (!persistent.ok()) {
    return Err(fss::ErrorKind::kUnavailable,
               "无法解析 persistent 存储：" + persistent.error().message());
  }
  const std::int64_t now_seconds = ports_.clock.NowEpochSeconds();

  //  ① 租约过期路径（多实例的正路，永远执行）
  CollectExpiredLeases(partition, options, *staging.value(), *persistent.value(), now_seconds,
                       report);
  //  ② 单实例降级路径（只有在显式关掉"必须租约到期"时才按 TTL 扫描 staging）
  if (!options.require_lease_expiry) {
    CollectExpiredStaging(partition, options, *staging.value(), now_seconds, report);
  }
  //  ③ persistent 孤儿（与租约无关：它没有位置记录、也没有元数据指向它）
  CollectOrphanObjects(partition, options, *persistent.value(), now_seconds, report);

  return report;
}

}  // namespace fss::app
