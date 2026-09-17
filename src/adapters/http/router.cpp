// Router 实现。职责与横切入口见头文件。
#include "adapters/http/router.h"

#include "app/version.h"
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

//  `POST getFileLocation` 空体 / 缺 FileID 时的**固定消息**（契约 §1.6）
constexpr std::string_view kInvalidFileLocationRequest =
    "ConstraintViolationException: Invalid FileLocationRequest";

}  // namespace

fss::http::Handler Router::Wrap(Action action) {
  return [this, action = std::move(action)](fss::http::Request& request) -> fss::http::Response {
    const auto started = std::chrono::steady_clock::now();
    const std::string route = request.route_name;
    const std::string method(fss::http::MethodName(request.method));

    app::CallerContext caller;
    if (auto value = request.Header("data-partition-id"); value.has_value()) {
      caller.partition = *value;
    }
    if (auto value = request.Header("authorization"); value.has_value()) {
      caller.bearer_token = *value;
    }
    if (auto value = request.Header("x-user-id"); value.has_value()) {
      caller.user_id = *value;
    }
    if (caller.user_id.empty()) caller.user_id = options_.default_user_id;
    caller.correlation_id = request.correlation_id;

    //  ★ 指标要在**所有**出口上记一次（成功 / 契约错误 / 异常），所以这里用
    //    局部 lambda + 统一 return：三个出口各写一遍必然漏（见 Wrap 的异常分支）
    const auto finish = [&](fss::http::Response response) -> fss::http::Response {
      const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
      metrics_.Observe(route, method, response.status, elapsed.count());
      return response;
    };

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
               //  依赖就绪的**最小探针**：仓储可达即可（存储可达性在 P9 的 readiness 细化）
               const auto probe = ports_.metadata.List("__readiness__", domain::MetadataQuery{});
               if (!probe.ok()) {
                 return fss::http::Response::Text(503, "File service is not ready");
               }
               return fss::http::Response::Text(200, "File service is ready");
             }));

  server.Get(base + "/v2/info", MakeRoute("ops.info", kSmallBodyLimit),
             Wrap([](fss::http::Request&, const app::CallerContext&) -> fss::Result<fss::http::Response> {
               VersionInfoResponse info;
               info.version = "v2";
               info.build_version = fss::app::BuildVersion();
               //  内置 SQLite 时记录不进 Storage Service，因此这里如实声明只有 storage
               info.connected_outer_services = {"storage"};
               return fss::http::Response::Json(200, fss::json::Dump(ToJson(info)));
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
                 const std::string body = metrics_.RenderPrometheus(server.stats());
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
  const auto storage_instructions = [this](fss::http::Request& request,
                                          const app::CallerContext& caller)
      -> fss::Result<fss::http::Response> {
    app::GetStorageInstructions usecase(ports_);
    FSS_TRY(result, usecase.Execute(caller, request.Query("expiryTime")));
    StorageInstructionsResponse response;
    response.provider_key = result.provider_key;
    response.storage_location.signed_url = result.signed_url;
    response.storage_location.file_source = result.file_source;
    response.storage_location.created_by = result.created_by;
    response.storage_location.expires_at_epoch_seconds = result.expires_at_epoch_seconds;
    return fss::http::Response::Json(200, fss::json::Dump(ToJson(response)));
  };

  const auto retrieval_instructions = [this](fss::http::Request& request,
                                            const app::CallerContext& caller)
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
      dto.retrieval_properties.signed_url = instruction.signed_url;
      dto.provider_key = instruction.provider_key;
      response.datasets.push_back(std::move(dto));
    }
    return fss::http::Response::Json(200, fss::json::Dump(ToJson(response)));
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
      //  元素是记录（契约 §2.9：`{"datasetSources": [ ...records... ]}`）；也接受
      //  直接给路径字符串（部分客户端这样调）。取不到 FileSource 时**传空串**，
      //  让用例给出契约 §5 的固定消息，而不是在这里静默跳过。
      std::string file_source;
      if (item.is_string()) {
        file_source = item.get<std::string>();
      } else if (item.is_object()) {
        if (const auto top = item.find("FileSource");
            top != item.end() && top->is_string()) {
          file_source = top->get<std::string>();
        } else if (const auto data = item.find("data"); data != item.end()) {
          if (const auto props = data->find("DatasetProperties"); props != data->end()) {
            if (const auto info = props->find("FileSourceInfo"); info != props->end()) {
              if (const auto src = info->find("FileSource");
                  src != info->end() && src->is_string()) {
                file_source = src->get<std::string>();
              }
            }
          }
        }
      }
      sources.push_back(app::CopyFileSource{std::move(file_source)});
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

  for (const std::string& prefix : {"/v2/files", "/v2/file-collections"}) {
    server.Post(base + prefix + "/storageInstructions", MakeRoute("dms.storage_instructions"),
                Wrap(storage_instructions));
    server.Post(base + prefix + "/retrievalInstructions",
                MakeRoute("dms.retrieval_instructions"), Wrap(retrieval_instructions));
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
  //  §7 扩展数据面 `/v1/transfer/{token}`（集中存储的字节通道）
  //  · 只做 token 校验 + 字节转发；**对象键来自 token 载荷**，不来自 URL
  //  · 请求体不限大小，因此 `max_body_bytes = 0`；请求体走流式（不整块驻留）
  // ---------------------------------------------------------------------------
  if (transfers_.put) {
    fss::http::RouteOptions stream_options = MakeRoute("transfer.put", 0);
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
