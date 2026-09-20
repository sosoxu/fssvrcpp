// Router 实现。职责与横切入口见头文件。
#include "adapters/http/router.h"

#include "app/usecases/caller_context.h"
#include "app/usecases/wire_shapes.h"
#include "common/json/json.h"
#include "common/time/time_format.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace fss::adapters::http {

namespace {

constexpr std::int64_t kJsonBodyLimit = 10 * 1024 * 1024;
constexpr std::int64_t kSmallBodyLimit = 256 * 1024;

fss::http::RouteOptions MakeRoute(std::string name, std::int64_t max_body_bytes = kJsonBodyLimit) {
  fss::http::RouteOptions options;
  options.name = std::move(name);
  options.max_body_bytes = max_body_bytes;
  return options;
}

//  ---- 每个路由的角色要求（契约 §1.3 的端点表；与用例入口的第二道完全一致）----
const RouteAuth kEditors{{"service.file.editors"}, true};
const RouteAuth kViewers{{"service.file.viewers"}, true};
const RouteAuth kEditorsOrAdmin{{"service.file.editors", "service.file.admin"}, true};
const RouteAuth kDatasetEditors{{"service.dataset.editors"}, true};
const RouteAuth kDatasetViewers{{"service.dataset.viewers"}, true};
const RouteAuth kStorageCreatorOrAdmin{{"service.storage.creator", "service.storage.admin"}, true};
const RouteAuth kDeliveryViewer{{"service.delivery.viewer"}, true};
const RouteAuth kAdminNoPartition{{"service.file.admin"}, false};

//  路由名 → 角色要求（契约 §1.3 的**唯一表**；与用例入口的第二道判据一致）
//  ★ 未登记的路由返回 `nullopt` → 调用方 fail-closed（不是"默认放行"）
std::optional<RouteAuth> RouteAuthTable(std::string_view route) {
  static const std::map<std::string_view, RouteAuth> kTable = {
      //  免鉴权（运维面 + 数据面：数据面用自签 token 自证，不经 JWT）
      {"ops.liveness", {}},
      {"ops.readiness", {}},
      {"ops.info", {}},
      {"ops.metrics", {}},
      {"transfer.put", {}},
      {"transfer.get", {}},
      //  契约 §1.3 的端点表
      {"location.upload_url", kEditors},
      {"location.get_location", kEditors},
      {"location.get_file_location", kEditors},
      {"location.download_url", kViewers},
      {"location.list", kEditors},
      {"metadata.create", kEditors},
      {"metadata.get", kViewers},
      {"metadata.delete", kEditorsOrAdmin},
      {"dms.storage_instructions", kDatasetEditors},
      {"dms.retrieval_instructions", kDatasetViewers},
      {"dms.copy", kStorageCreatorOrAdmin},
      {"delivery.get_file_signed_url", kDeliveryViewer},
      {"file.revoke_url", kAdminNoPartition},
      //  ★ C9.31 / ADR-013 §10：按需 GC 是**运维动作**（会删数据），因此要
      //    `service.file.admin`；与 `revokeURL` 一样**不要求** `data-partition-id`
      //    （数据分区由组合根的单租户注册表决定，不是调用方说了算）。
      {"ops.gc_run", kAdminNoPartition},
  };
  const auto it = kTable.find(route);
  if (it == kTable.end()) return std::nullopt;
  return it->second;
}

//  `POST getFileLocation` 空体 / 缺 FileID 时的**固定消息**（契约 §1.6）
constexpr std::string_view kInvalidFileLocationRequest =
    "ConstraintViolationException: Invalid FileLocationRequest";

}  // namespace

fss::http::Handler Router::Wrap(Action action) {
  return [this, action = std::move(action)](fss::http::Request& request) -> fss::http::Response {
    const auto started = std::chrono::steady_clock::now();
    const std::string route = request.route_name;
    const std::string method(fss::http::MethodName(request.method));

    //  ★ 与 gRPC 适配层**共用**同一套解析规则（`app::CallerFromHeaders`）：
    //    两条链路的租户/token/用户/correlation 行为必须完全一致（契约 §4.3）。
    const auto caller = app::CallerFromHeaders(
        [&request](std::string_view name) { return request.Header(name); },
        request.correlation_id, options_.default_user_id);

    //  ★ 指标要在**所有**出口上记一次（成功 / 契约错误 / 异常），所以这里用
    //    局部 lambda + 统一 return：三个出口各写一遍必然漏（见 Wrap 的异常分支）
    const auto finish = [&](fss::http::Response response) -> fss::http::Response {
      const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
      metrics_.Observe(route, method, response.status, elapsed.count());
      return response;
    };

    //  ★ 鉴权**先于**任何 DTO 解析（契约 §1.3 末段：未授权者不得靠 400 的差异探测）
    //    要求按路由名查表；未登记的路由 **fail-closed**（绝不"默认放行"）
    const auto auth = RouteAuthTable(route);
    if (!auth.has_value()) {
      return finish(ErrorToResponse(
          Err(fss::ErrorKind::kInternal, "路由未登记角色要求：" + route), options_.error_format));
    }
    if (!auth->roles.empty()) {
      //  ★ 顺序必须与用例入口（`AuthorizeCaller`）**逐字一致**：先 partition、后 token。
      //    两层给出不同的 message 比"顺序本身"更糟 —— 同一个请求会在不同深度
      //    报出不同的固定文案（phase4 的契约用例已经固化了 partition 优先）。
      if (auth->require_partition && caller.partition.empty()) {
        return finish(ErrorToResponse(Err(fss::ErrorKind::kUnauthenticated, "Missing partitionID"),
                                      options_.error_format));
      }
      if (caller.bearer_token.empty()) {
        return finish(ErrorToResponse(Err(fss::ErrorKind::kUnauthenticated,
                                         "Missing authorization token"),
                                      options_.error_format));
      }
      if (const auto allowed = ports_.authorizer.AuthorizeAny(auth->roles, caller.partition,
                                                             caller.bearer_token);
          !allowed.ok()) {
        return finish(ErrorToResponse(allowed.error(), options_.error_format));
      }
    }

    try {
      auto result = action(request, caller);
      if (!result.ok()) {
        return finish(ErrorToResponse(result.error(), options_.error_format));
      }
      return finish(std::move(result).value());
    } catch (const std::exception& error) {
      return finish(ErrorToResponse(Err(fss::ErrorKind::kInternal,
                                        std::string("未预期异常：") + error.what()),
                                    options_.error_format));
    } catch (...) {
      return finish(
          ErrorToResponse(Err(fss::ErrorKind::kInternal, "未预期异常"), options_.error_format));
    }
  };
}

void Router::RecordTransferRejection(const fss::Error& error) {
  switch (error.kind()) {
    case fss::ErrorKind::kUnauthenticated:   // 签名错 / 过期 / exp 与载荷不一致
    case fss::ErrorKind::kPermissionDenied:  // 用途（op）或租户不符
      metrics_.RecordTransferTokenRejected();
      break;
    default:
      break;
  }
}

void Router::Register(fss::http::Server& server) {
  const std::string base = options_.base_path;

  // ---------------------------------------------------------------------------
  //  §2.12 运维端点：**纯文本**、免鉴权（C4.6）
  // ---------------------------------------------------------------------------
  server.Get(base + "/v2/liveness_check", MakeRoute("ops.liveness", kSmallBodyLimit),
             Wrap([](fss::http::Request&, const app::CallerContext&) -> fss::Result<fss::http::Response> {
               return fss::http::Response::Text(200, "File service is alive");
             }));

  server.Get(base + "/v2/readiness_check", MakeRoute("ops.readiness", kSmallBodyLimit),
             Wrap([this](fss::http::Request&, const app::CallerContext&) -> fss::Result<fss::http::Response> {
               //  ★ B2a（ADR-009 §5.3）：readiness 必须验证**共享状态**真的可用。
               //    组合根注入的 `shared_state_probe` = PG 存活（SELECT 1）+ 迁移版本
               //    （schema_migrations.max(version) == kExpectedSchemaVersion，可在
               //    `metadata.postgres.schema_version_check=false` 时跳过版本比对）。
               //    未装配（单实例 SQLite/内存）→ 退回下面的"仓储可达"最小探针（逐字不变）。
               //    ★ 失败原因必须**可读**：REST 的 503 文本带上它（gRPC 侧同源）。
               if (ports_.shared_state_probe) {
                 const auto shared = ports_.shared_state_probe();
                 if (!shared.ok()) {
                   return fss::http::Response::Text(
                       503, "File service is not ready: " + shared.error().message());
                 }
               }
               //  依赖就绪的**最小探针**：仓储可达即可
               //  （共享状态探针为空时它是唯一判据，与接线前逐字一致；
               //   失败体**保持**固定文本 —— 未经控制的仓储错误不往外抛，避免
               //   无鉴权的运维端点泄漏内部细节）
               const auto probe = ports_.metadata.List("__readiness__", domain::MetadataQuery{});
               if (!probe.ok()) {
                 return fss::http::Response::Text(503, "File service is not ready");
               }
               return fss::http::Response::Text(200, "File service is ready");
             }));

  server.Get(base + "/v2/info", MakeRoute("ops.info", kSmallBodyLimit),
             Wrap([this](fss::http::Request&, const app::CallerContext&)
                      -> fss::Result<fss::http::Response> {
               //  ★ C8.5 / C9.30：**唯一来源**是 `app::GetInfo` 用例（REST 与 gRPC
               //    从同一处取值 ⇒ 一致性按构造保证）。此前这里是就地拼 DTO 的，
               //    于是"新增一个字段"必须改两遍适配器 —— 正是 C7.3 要避免的形态。
               app::GetInfo usecase(ports_);
               const auto result = usecase.Execute();
               if (!result.ok()) return result.error();
               const auto& info = result.value();
               VersionInfoResponse response;
               response.version = info.version;
               response.build_version = info.build_version;
               response.connected_outer_services = info.connected_outer_services;
               //  C8.5：鉴权模式必须在 `/v2/info` 可见（与 gRPC 同源）
               response.auth_mode = info.auth_mode;
               //  C9.30（ADR-010 R11）：当前引擎 + 宿主能力探测结果
               response.io_engine = info.io_engine;
               response.io_uring_available = info.io_uring_available;
               //  E1a：实例身份（与 gRPC 同源，都来自 `app::GetInfo`）
               response.instance_id = info.instance_id;
               return fss::http::Response::Json(200, fss::json::Dump(ToJson(response)));
             }));

  // ---------------------------------------------------------------------------
  //  §7 扩展：`/metrics`（**不在** base path 下，契约 §7 的命名空间隔离要求）
  //  · 未鉴权：与 liveness/readiness 同级的运维面；P9 会加上绑定/令牌选项
  //  · 计数由 `Wrap()` 统一做，这里只渲染（含服务器自身的背压计数）
  // ---------------------------------------------------------------------------
  if (options_.metrics_enabled) {
    server.Get(options_.metrics_path, MakeRoute("ops.metrics", kSmallBodyLimit),
               [this, &server](fss::http::Request& request) -> fss::http::Response {
                 const auto started = std::chrono::steady_clock::now();
                 //  HTTP 指标 + L1 注册表（存储/GC）拼成同一份抓取文本
                 std::string body = metrics_.RenderPrometheus(server.stats());
                 if (options_.metrics_registry != nullptr) {
                   body += options_.metrics_registry->RenderPrometheus();
                 }
                 fss::http::Response response;
                 response.status = 200;
                 //  ★ Prometheus 的 Content-Type 必须带 version 参数，抓取器据此选解析器
                 response.headers.Set("Content-Type", "text/plain; version=0.0.4; charset=utf-8");
                 response.body = body;
                 const auto elapsed =
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
                 metrics_.Observe("ops.metrics", std::string(fss::http::MethodName(request.method)), 200,
                                  elapsed.count());
                 return response;
               });
  }

  // ---------------------------------------------------------------------------
  //  §2.1 uploadURL / §2.2 getLocation（已废弃但实现）
  // ---------------------------------------------------------------------------
  server.Get(base + "/v2/files/uploadURL", MakeRoute("location.upload_url"),
             Wrap([this](fss::http::Request& request,
                         const app::CallerContext& caller) -> fss::Result<fss::http::Response> {
               app::GetUploadLocation usecase(ports_);
               FSS_TRY(result, usecase.Execute(caller, std::nullopt, request.Query("expiryTime")));
               LocationResponse response;
               response.file_id = result.file_id;
               response.location.signed_url = result.signed_url;
               response.location.file_source = result.file_source;
               return fss::http::Response::Json(200, fss::json::Dump(ToJson(response)));
             }));

  server.Post(base + "/v2/getLocation", MakeRoute("location.get_location"),
              Wrap([this](fss::http::Request& request,
                          const app::CallerContext& caller) -> fss::Result<fss::http::Response> {
                std::optional<std::string> requested_file_id;
                if (!request.body.empty()) {
                  FSS_TRY(value, fss::json::ParseObject(request.body));
                  if (const auto it = value.find("FileID");
                      it != value.end() && it->is_string()) {
                    requested_file_id = it->get<std::string>();
                  }
                }
                app::GetUploadLocation usecase(ports_);
                FSS_TRY(result, usecase.Execute(caller, requested_file_id,
                                                request.Query("expiryTime")));
                LocationResponse response;
                response.file_id = result.file_id;
                response.location.signed_url = result.signed_url;
                response.location.file_source = result.file_source;
                return fss::http::Response::Json(200, fss::json::Dump(ToJson(response)));
              }));

  // ---------------------------------------------------------------------------
  //  §2.3 getFileLocation（已废弃；空体 → 固定消息）
  // ---------------------------------------------------------------------------
  server.Post(base + "/v2/getFileLocation", MakeRoute("location.get_file_location"),
              Wrap([this](fss::http::Request& request,
                          const app::CallerContext& caller) -> fss::Result<fss::http::Response> {
                if (request.body.empty()) {
                  return Err(fss::ErrorKind::kInvalidArgument, std::string(kInvalidFileLocationRequest));
                }
                FSS_TRY(value, fss::json::ParseObject(request.body));
                const auto it = value.find("FileID");
                if (it == value.end() || !it->is_string()) {
                  return Err(fss::ErrorKind::kInvalidArgument, std::string(kInvalidFileLocationRequest));
                }
                app::GetFileLocation usecase(ports_);
                FSS_TRY(view, usecase.Execute(caller, it->get<std::string>()));
                FileLocationResponse response;
                response.driver = view.driver;
                response.location = view.location;
                return fss::http::Response::Json(200, fss::json::Dump(ToJson(response)));
              }));

  // ---------------------------------------------------------------------------
  //  §2.4 downloadURL
  // ---------------------------------------------------------------------------
  server.Get(base + "/v2/files/:id/downloadURL", MakeRoute("location.download_url"),
             Wrap([this](fss::http::Request& request,
                         const app::CallerContext& caller) -> fss::Result<fss::http::Response> {
               const auto id = request.Param("id");
               if (!id.has_value()) {
                 return Err(fss::ErrorKind::kInvalidArgument, "缺少路径参数 id");
               }
               app::GetDownloadLocation usecase(ports_);
               FSS_TRY(result, usecase.Execute(caller, *id, request.Query("expiryTime")));
               DownloadUrlResponse response;
               response.signed_url = result.signed_url;
               return fss::http::Response::Json(200, fss::json::Dump(ToJson(response)));
             }));

  // ---------------------------------------------------------------------------
  //  §2.5 getFileList（Spring Page）
  // ---------------------------------------------------------------------------
  server.Post(base + "/v2/getFileList", MakeRoute("location.list", kSmallBodyLimit),
              Wrap([this](fss::http::Request& request,
                          const app::CallerContext& caller) -> fss::Result<fss::http::Response> {
                app::FileListRequest list_request;
                if (!request.body.empty()) {
                  FSS_TRY(value, fss::json::ParseObject(request.body));
                  if (const auto it = value.find("UserID"); it != value.end() && it->is_string()) {
                    list_request.user_id = it->get<std::string>();
                  }
                  if (const auto it = value.find("PageNum");
                      it != value.end() && it->is_number_integer()) {
                    list_request.page_num = it->get<int>();
                  }
                  if (const auto it = value.find("Items");
                      it != value.end() && it->is_number_integer()) {
                    list_request.items = it->get<int>();
                  }
                  const auto parse_time = [&](const char* key, std::int64_t& target) {
                    const auto it = value.find(key);
                    if (it == value.end() || !it->is_string()) return true;
                    const auto parsed = fss::time::ParseIso8601(it->get<std::string>());
                    if (!parsed.ok()) return false;
                    target = parsed.value();
                    return true;
                  };
                  if (!parse_time("TimeFrom", list_request.time_from_epoch_seconds) ||
                      !parse_time("TimeTo", list_request.time_to_epoch_seconds)) {
                    return Err(fss::ErrorKind::kInvalidArgument, "TimeFrom/TimeTo 不是合法的 ISO-8601");
                  }
                }
                app::GetFileList usecase(ports_);
                FSS_TRY(page, usecase.Execute(caller, list_request));
                FileListResponse response;
                response.number = page.number;
                response.number_of_elements = page.number_of_elements;
                response.size = page.size;
                for (const auto& entry : page.content) {
                  FileListEntryDto dto;
                  dto.file_id = entry.file_id;
                  dto.driver = entry.driver;
                  dto.location = entry.location;
                  dto.created_at_epoch_seconds = entry.created_at_epoch_seconds;
                  dto.created_by = entry.created_by;
                  response.content.push_back(std::move(dto));
                }
                return fss::http::Response::Json(200, fss::json::Dump(ToJson(response)));
              }));

  // ---------------------------------------------------------------------------
  //  §2.6/§2.7/§2.8 元数据
  // ---------------------------------------------------------------------------
  server.Post(base + "/v2/files/metadata", MakeRoute("metadata.create"),
              Wrap([this](fss::http::Request& request,
                          const app::CallerContext& caller) -> fss::Result<fss::http::Response> {
                FSS_TRY(value, fss::json::ParseObject(request.body));
                FSS_TRY(record, domain::ParseFileMetadataRecord(value));
                app::CreateFileMetadata usecase(ports_);
                FSS_TRY(id, usecase.Execute(caller, record));
                fss::json::Value body = fss::json::Value::object();
                body["id"] = id;
                return fss::http::Response::Json(201, fss::json::Dump(body));
              }));

  server.Get(base + "/v2/files/:id/metadata", MakeRoute("metadata.get"),
             Wrap([this](fss::http::Request& request,
                         const app::CallerContext& caller) -> fss::Result<fss::http::Response> {
               const auto id = request.Param("id");
               if (!id.has_value()) {
                 return Err(fss::ErrorKind::kInvalidArgument, "缺少路径参数 id");
               }
               app::GetFileMetadata usecase(ports_);
               FSS_TRY(record, usecase.Execute(caller, *id));
               return fss::http::Response::Json(200, fss::json::Dump(RecordToJson(record)));
             }));

  server.Delete(base + "/v2/files/:id/metadata", MakeRoute("metadata.delete"),
                Wrap([this](fss::http::Request& request,
                            const app::CallerContext& caller) -> fss::Result<fss::http::Response> {
                  const auto id = request.Param("id");
                  if (!id.has_value()) {
                    return Err(fss::ErrorKind::kInvalidArgument, "缺少路径参数 id");
                  }
                  app::DeleteFileMetadata usecase(ports_);
                  FSS_TRY(usecase.Execute(caller, *id));
                  return fss::http::Response::Empty(204);
                }));

  // ---------------------------------------------------------------------------
  //  §2.9 DMS（6 条）+ §2.10 delivery（1 条）+ §2.11 revokeURL（1 条）
  //  · `files/*` 与 `file-collections/*` 是**同一套语义的两套路径**（上游同时保留），
  //    因此共用同一个 lambda —— 抄一遍必然漂移。
  //  · 字段大小写：DMS/delivery 是 **camelCase**（`providerKey`/`signedUrl`），
  //    与 §2.1 的 `FileID`/`SignedURL` 不同（见 dto.h 的说明）。
  // ---------------------------------------------------------------------------
  //  ★ `files/*` 与 `file-collections/*` 的上传/下载位置**键名不同**（集合版是
  //    `fileCollectionSource` + `fileCount`/`fileNames`，见 dto.h）→ 用参数化工厂而不是
  //    "共用一个 lambda"（共享会把集合版写成 `fileSource`，P6-D11）。
  const auto make_storage_instructions = [this](bool collection) {
    return [this, collection](fss::http::Request& request, const app::CallerContext& caller)
               -> fss::Result<fss::http::Response> {
      app::GetStorageInstructions usecase(ports_);
      FSS_TRY(result, usecase.Execute(caller, request.Query("expiryTime")));
      StorageInstructionsResponse response;
      response.provider_key = result.provider_key;
      response.storage_location.signed_url = result.signed_url;
      response.storage_location.file_source = result.file_source;
      response.storage_location.created_by = result.created_by;
      response.storage_location.expires_at_epoch_seconds = result.expires_at_epoch_seconds;
      response.storage_location.collection = collection;
      return fss::http::Response::Json(200, fss::json::Dump(ToJson(response)));
    };
  };

  const auto make_retrieval_instructions = [this](bool collection) {
    return [this, collection](fss::http::Request& request, const app::CallerContext& caller)
               -> fss::Result<fss::http::Response> {
    FSS_TRY(value, fss::json::ParseObject(request.body));
    std::vector<std::string> registry_ids;
    if (const auto it = value.find("datasetRegistryIds"); it != value.end()) {
      if (!it->is_array()) {
        return Err(fss::ErrorKind::kInvalidArgument, "datasetRegistryIds 必须是字符串数组");
      }
      for (const auto& item : *it) {
        if (!item.is_string()) {
          return Err(fss::ErrorKind::kInvalidArgument, "datasetRegistryIds 必须是字符串数组");
        }
        registry_ids.push_back(item.get<std::string>());
      }
    }
      app::GetRetrievalInstructions usecase(ports_);
      FSS_TRY(result, usecase.Execute(caller, registry_ids, request.Query("expiryTime")));
      RetrievalInstructionsResponse response;
      for (const auto& instruction : result) {
        RetrievalInstructionDto dto;
        dto.dataset_registry_id = instruction.dataset_registry_id;
        dto.provider_key = instruction.provider_key;
        auto& properties = dto.retrieval_properties;
        properties.signed_url = instruction.signed_url;
        properties.file_source = instruction.file_source;
        properties.created_by = instruction.created_by;
        properties.expires_at_epoch_seconds = instruction.expires_at_epoch_seconds;
        properties.collection = collection;
        response.datasets.push_back(std::move(dto));
      }
      return fss::http::Response::Json(200, fss::json::Dump(ToJson(response)));
    };
  };

  const auto copy_files = [this](fss::http::Request& request,
                                 const app::CallerContext& caller)
      -> fss::Result<fss::http::Response> {
    FSS_TRY(value, fss::json::ParseObject(request.body));
    const auto it = value.find("datasetSources");
    if (it == value.end() || !it->is_array()) {
      return Err(fss::ErrorKind::kInvalidArgument, "datasetSources 必须是数组");
    }
    std::vector<app::CopyFileSource> sources;
    for (const auto& item : *it) {
      //  取值规则与 gRPC 侧共用（`app::FileSourceFromRecordNode`）：同一份输入在两条
      //  协议上必须得到同一个 FileSource（C7.3）。取不到时传空串，让用例给出固定消息。
      sources.push_back(app::CopyFileSource{app::FileSourceFromRecordNode(item)});
    }
    app::CopyFiles usecase(ports_);
    FSS_TRY(outcomes, usecase.Execute(caller, sources));
    std::vector<CopyDmsResponse> results;
    for (const auto& outcome : outcomes) {
      CopyDmsResponse dto;
      dto.success = outcome.success;
      dto.dataset_blob_storage_path = outcome.dataset_blob_storage_path;
      results.push_back(std::move(dto));
    }
    return fss::http::Response::Json(200, fss::json::Dump(ToJson(results)));
  };

  for (std::string prefix : {"/v2/files", "/v2/file-collections"}) {
    const bool collection = prefix == "/v2/file-collections";
    server.Post(base + prefix + "/storageInstructions", MakeRoute("dms.storage_instructions"),
                Wrap(make_storage_instructions(collection)));
    server.Post(base + prefix + "/retrievalInstructions",
                MakeRoute("dms.retrieval_instructions"),
                Wrap(make_retrieval_instructions(collection)));
    server.Post(base + prefix + "/copy", MakeRoute("dms.copy"), Wrap(copy_files));
  }

  server.Post(base + "/v2/delivery/GetFileSignedUrl", MakeRoute("delivery.get_file_signed_url"),
              Wrap([this](fss::http::Request& request,
                          const app::CallerContext& caller) -> fss::Result<fss::http::Response> {
                FSS_TRY(value, fss::json::ParseObject(request.body));
                std::vector<std::string> srns;
                if (const auto it = value.find("srns"); it != value.end()) {
                  if (!it->is_array()) {
                    return Err(fss::ErrorKind::kInvalidArgument, "srns 必须是字符串数组");
                  }
                  for (const auto& item : *it) {
                    if (!item.is_string()) {
                      return Err(fss::ErrorKind::kInvalidArgument, "srns 必须是字符串数组");
                    }
                    srns.push_back(item.get<std::string>());
                  }
                }
                app::GetFileSignedUrl usecase(ports_);
                FSS_TRY(result, usecase.Execute(caller, srns, request.Query("expiryTime")));
                UrlSigningResponse response;
                for (const auto& [srn, entry] : result.processed) {
                  SignedUrlDto dto;
                  dto.signed_url = entry.signed_url;
                  dto.unsigned_url = entry.unsigned_url;
                  dto.kind = entry.kind;
                  response.processed.emplace(srn, std::move(dto));
                }
                response.unprocessed = result.unprocessed;
                return fss::http::Response::Json(200, fss::json::Dump(ToJson(response)));
              }));

  //  §2.11 revokeURL：**不要求 `data-partition-id`**，恒定 204；集中存储没有
  //  "用户委派密钥"可吊销，因此用响应头如实说明（契约 §2.11 的 POSIX 分支）。
  server.Post(base + "/v2/files/revokeURL", MakeRoute("file.revoke_url", kSmallBodyLimit),
              Wrap([this](fss::http::Request&, const app::CallerContext& caller)
                       -> fss::Result<fss::http::Response> {
                app::RevokeUrl usecase(ports_);
                FSS_TRY(usecase.Execute(caller));
                auto response = fss::http::Response::Empty(204);
                response.headers.Set(
                    "X-FSS-Notice",
                    "revokeURL 在当前集中存储驱动下无委派密钥可吊销；已记录审计事件");
                return response;
              }));

  // ---------------------------------------------------------------------------
  //  §7.3 扩展：按需 GC `POST {base_path}/v2/gc:run`（C9.31 / ADR-013 §10）
  //  · **上游 OSDU 没有** GC 端点 —— 这是本服务的运维扩展（契约 §7.3），
  //    因此字段名用 snake_case（与 `gc.*` 配置一致），不套 OSDU 的大小写约定。
  //  · 授权 `service.file.admin`，**不要求** `data-partition-id`（见 RouteAuthTable）。
  //  · `?dryRun=true` 只能把这一轮**降级成预览**；**没有**能让它变成真删的查询参数。
  //  · 与周期调度共享 `GcTask`（连带单飞护栏）：已在跑时 `kUnavailable` → **503**。
  // ---------------------------------------------------------------------------
  if (gc_.run) {
    server.Post(base + "/v2/gc:run", MakeRoute("ops.gc_run", kSmallBodyLimit),
                Wrap([this](fss::http::Request& request, const app::CallerContext& caller)
                         -> fss::Result<fss::http::Response> {
                  //  ★ dry-run 只能**更保守**：`配置 gc.dry_run || 请求 dryRun`。
                  //    只认显式 `true`/`1`；其它值（含 `false`）不改变配置的取值。
                  const auto raw_dry_run = request.Query("dryRun");
                  const bool force_dry_run =
                      raw_dry_run.has_value() && (*raw_dry_run == "true" || *raw_dry_run == "1");
                  FSS_TRY(report, gc_.run(caller, force_dry_run));
                  GcRunResponse response;
                  //  `report.dry_run` 是**有效值**（由组合根的 `GcTask` 回填）：
                  //  调用方据此判断这一轮到底删没删，而不是去猜配置。
                  response.dry_run = report.dry_run;
                  response.partition = gc_.partition;
                  response.scheduled = gc_.scheduled;
                  response.expired_leases_claimed = report.expired_leases_claimed;
                  response.deleted_objects = report.deleted_objects;
                  response.deleted_locations = report.deleted_locations;
                  response.skipped_has_record = report.skipped_has_record;
                  response.skipped_no_location = report.skipped_no_location;
                  response.skipped_too_young = report.skipped_too_young;
                  response.tmp_removed = report.tmp_removed;
                  response.tmp_skipped_too_young = report.tmp_skipped_too_young;
                  response.tmp_skipped_unknown_mtime = report.tmp_skipped_unknown_mtime;
                  response.errors = report.errors;
                  return fss::http::Response::Json(200, fss::json::Dump(ToJson(response)));
                }));
  }

  // ---------------------------------------------------------------------------
  //  §7 扩展数据面 `/v1/transfer/{token}`（集中存储的字节通道）
  //  · 只做 token 校验 + 字节转发；**对象键来自 token 载荷**，不来自 URL
  //  · 请求体默认不限大小（`max_body_bytes = 0`）；C10.16 起可由
  //    `partition.file.<partition>.max_file_bytes` 收紧（>0 时超限 → 413/400）。
  // ---------------------------------------------------------------------------
  if (transfers_.put) {
    fss::http::RouteOptions stream_options =
        MakeRoute("transfer.put", options_.transfer_put_max_body_bytes);
    stream_options.stream_body = true;
    server.Put(base + "/v1/transfer/:token", stream_options,
               Wrap([this](fss::http::Request& request, const app::CallerContext& caller)
                        -> fss::Result<fss::http::Response> {
                 const auto token = request.Param("token");
                 if (!token.has_value()) {
                   return Err(fss::ErrorKind::kInvalidArgument, "缺少路径参数 token");
                 }
                 if (!request.body_stream) {
                   return Err(fss::ErrorKind::kInvalidArgument, "PUT 缺少请求体流");
                 }
                 const auto put = transfers_.put(*token, request.Query("exp").value_or(""),
                                                 request.Query("sig").value_or(""),
                                                 caller.partition, *request.body_stream);
                 if (!put.ok()) {
                   RecordTransferRejection(put.error());
                   return put.error();
                 }
                 return fss::http::Response::Empty(200);
               }));
  }
  if (transfers_.open_get) {
    server.Get(base + "/v1/transfer/:token", MakeRoute("transfer.get", 0),
               Wrap([this](fss::http::Request& request, const app::CallerContext& caller)
                        -> fss::Result<fss::http::Response> {
                 const auto token = request.Param("token");
                 if (!token.has_value()) {
                   return Err(fss::ErrorKind::kInvalidArgument, "缺少路径参数 token");
                 }
                 const auto opened =
                     transfers_.open_get(*token, request.Query("exp").value_or(""),
                                         request.Query("sig").value_or(""), caller.partition);
                 if (!opened.ok()) {
                   RecordTransferRejection(opened.error());
                   return opened.error();
                 }
                 auto source = opened.value();
                 const std::int64_t length =
                     source != nullptr ? source->Size().value_or(-1) : 0;
                 return fss::http::Response::Stream(200, "application/octet-stream",
                                                    std::move(source), length);
               }));
  }
}

}  // namespace fss::adapters::http
