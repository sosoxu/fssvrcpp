// LocationIssuer 实现。设计约束与"为什么只有这一处能力分支"见头文件。
#include "app/services/location_issuer.h"

#include "app/services/object_key_policy.h"

#include <utility>

namespace fss::app {

namespace {

fss::Error Invalid(const std::string& message) {
  return Err(fss::ErrorKind::kInvalidArgument, message);
}

}  // namespace

fss::Result<LocationResult> LocationIssuer::IssueUploadLocation(
    std::string_view partition, std::string_view user_id,
    const std::optional<std::string>& requested_file_id,
    const std::optional<std::string>& expiry_time) {
  if (partition.empty()) return Invalid("partition 不能为空");
  if (user_id.empty()) return Invalid("user_id 不能为空");

  //  ① 有效期：缺省 1H / 超限静默截断 7D / 非法 → 固定消息（ExpiryPolicy，C2.3）
  FSS_TRY(ttl_seconds, ExpiryPolicy::Parse(expiry_time, expiry_));

  //  ② fileID：客户端可指定（旧接口 getLocation），否则服务端生成
  std::string file_id;
  if (requested_file_id.has_value() && !requested_file_id->empty()) {
    if (!ObjectKeyPolicy::IsValidFileId(*requested_file_id)) {
      return Invalid("Invalid FileID");
    }
    //  契约 §2.2：fileID 已存在 → 400 ALREADY_EXISTS（不是 409）
    const auto existing = locations_.Find(partition, *requested_file_id);
    if (existing.ok()) {
      return Err(fss::ErrorKind::kLocationAlreadyExists,
                 "Location for fileID = " + *requested_file_id + " already exists");
    }
    file_id = *requested_file_id;
  } else {
    file_id = ids_.NewUuidNoDash();
  }

  //  ③ FileSource / 容器 / 对象键（ObjectKeyPolicy 是安全边界，逐段白名单）
  const std::int64_t now_ms = clock_.NowEpochMillis();
  ObjectKeyPolicy::SourcePath parts;
  parts.user_id = std::string(user_id);
  parts.epoch_millis = now_ms;
  parts.timestamp = ObjectKeyPolicy::FormatTimestamp(now_ms / 1000, static_cast<int>(now_ms % 1000));
  parts.file_id = file_id;

  FSS_TRY(file_source, ObjectKeyPolicy::MakeFileSource(parts));
  const auto zone = domain::StorageZone::kStaging;
  //  ★ 阶段 10：容器名走租户注册表（分区级覆盖）；没有注册表时退回默认命名。
  FSS_TRY(container, partitions_ != nullptr
                         ? ObjectKeyPolicy::ContainerFor(*partitions_, partition, zone)
                         : ObjectKeyPolicy::ContainerFor(partition, zone));

  domain::ObjectRef ref;
  ref.container = container;
  ref.key = ObjectKeyPolicy::MakePosixKey(parts);

  //  ④ 保证容器存在（S3 的桶不在 PutObject 里创建，因此这一步是契约要求的前置）
  FSS_TRY(store, blobs_.ForPartition(partition, zone));
  FSS_TRY(store->ensure_container(ref.container));
  //  契约 §2.1 的副作用：在 staging 区创建**空对象**（客户端随后按签名 URL PUT 覆盖）。
  //  这也让"先 uploadURL、再 CreateFileMetadata"的流程在服务端有对象可复制。
  {
    bytes::StringSource empty_source("");
    FSS_TRY(store->put(ref, empty_source, domain::PutOptions{}));
  }

  //  ⑤ 位置记录（先登记，后续 metadata 才能查到；物理引用见头文件的说明）
  const auto caps = store->capabilities();   // ★ 能力分支点的输入（C2.7 白名单调用点）
  domain::FileLocation location;
  location.file_id = file_id;
  location.zone = zone;
  location.file_source = file_source;
  location.user_id = std::string(user_id);   // getFileList 的 UserID 过滤依据（映射为响应的 CreatedBy）
  location.created_at_epoch_seconds = clock_.NowEpochSeconds();
  location.updated_at_epoch_seconds = location.created_at_epoch_seconds;
  //  驱动枚举只是记录用；真正的驱动**名**是能力声明里的字符串（"memory" 等映射不到枚举时也不丢）
  location.driver = domain::ParseStorageDriver(caps.driver_name).value_or(location.driver);
  location.extra = json::Value::object();
  location.extra[std::string(kExtraContainer)] = ref.container;
  location.extra[std::string(kExtraObjectKey)] = ref.key;
  location.extra[std::string(kExtraDriverName)] = caps.driver_name;
  FSS_TRY(locations_.Save(partition, location));

  //  ⑥ 生成地址 —— 唯一的能力分支
  return SignAndShape(*store, ref, location, caps, partition, ttl_seconds, /*upload=*/true);
}

fss::Result<LocationResult> LocationIssuer::IssueDownloadLocation(
    std::string_view partition, std::string_view file_id,
    const std::optional<std::string>& expiry_time) {
  if (partition.empty()) return Invalid("partition 不能为空");
  if (file_id.empty()) return Invalid("file_id 不能为空");

  FSS_TRY(ttl_seconds, ExpiryPolicy::Parse(expiry_time, expiry_));

  //  ① 位置记录是权威映射；缺失 → 404（固定消息 "Not found location for fileID : <id>"）
  FSS_TRY(location, locations_.Find(partition, file_id));

  //  ② 物理引用来自记录本身（避免为了取 key 而按驱动类型分支）
  if (!location.extra.is_object() || !location.extra.contains(std::string(kExtraContainer)) ||
      !location.extra.contains(std::string(kExtraObjectKey))) {
    return Err(fss::ErrorKind::kInternal,
               "位置记录缺少物理引用（extra.container / extra.object_key）");
  }
  domain::ObjectRef ref;
  ref.container = location.extra[std::string(kExtraContainer)].get<std::string>();
  ref.key = location.extra[std::string(kExtraObjectKey)].get<std::string>();

  FSS_TRY(store, blobs_.ForPartition(partition, location.zone));
  const auto caps = store->capabilities();   // ★ 白名单调用点
  return SignAndShape(*store, ref, location, caps, partition, ttl_seconds, /*upload=*/false);
}

fss::Result<LocationResult> LocationIssuer::SignAndShape(
    domain::IBlobStore& store, const domain::ObjectRef& ref,
    const domain::FileLocation& location, const domain::BlobCapabilities& caps,
    std::string_view partition, std::int64_t ttl_seconds, bool upload) {
  domain::PresignOptions options;
  options.expires_in_seconds = ttl_seconds;
  options.method = upload ? "PUT" : "GET";

  domain::SignedLocation signed_loc;
  if (caps.native_presign) {
    //  分支 ①：对象存储的原生预签名，客户端直连（服务带宽零消耗）
    //  ⚠️ `FSS_TRY(var, expr)` 是**声明** var（不是赋值）—— 直接写 `FSS_TRY(signed_loc, ...)`
    //     会在内层块里遮蔽外层同名变量，值丢了还不报错。因此这里用不同名字再显式赋值。
    if (upload) {
      FSS_TRY(presigned, store.presign_put(ref, options));
      signed_loc = std::move(presigned);
    } else {
      FSS_TRY(presigned, store.presign_get(ref, options));
      signed_loc = std::move(presigned);
    }
  } else {
    //  分支 ②：集中存储没有原生签名 → 本服务自签传输 token，由 /v1/transfer 代理字节
    domain::TransferToken token;
    token.partition = std::string(partition);
    token.file_id = location.file_id;
    token.container = ref.container;
    token.object_key = ref.key;
    token.zone = location.zone;
    token.op = upload ? "put" : "get";
    token.expires_at_epoch_seconds = clock_.NowEpochSeconds() + ttl_seconds;

    FSS_TRY(url, self_signed_.Encode(token, self_base_url_));
    signed_loc.url = std::move(url);
    signed_loc.method = options.method;
    signed_loc.file_source = location.file_source;
    signed_loc.expires_at_epoch_seconds = token.expires_at_epoch_seconds;
    signed_loc.native = false;
  }

  LocationResult out;
  out.file_id = location.file_id;
  out.file_source = location.file_source;
  out.signed_url = signed_loc.url;
  //  ★ 领域结果里的驱动来自**能力声明的字符串**，而不是 StorageDriver 枚举 ——
  //    这是 C2.6"结果不含驱动类型分支痕迹"的可观察形式。
  out.driver = caps.driver_name;
  out.zone = location.zone;
  out.expires_at_epoch_seconds = signed_loc.expires_at_epoch_seconds > 0
                                     ? signed_loc.expires_at_epoch_seconds
                                     : clock_.NowEpochSeconds() + ttl_seconds;
  out.native_presign = caps.native_presign;
  return out;
}

}  // namespace fss::app
