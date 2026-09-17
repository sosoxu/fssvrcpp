// =============================================================================
//  grpc_server（L5）—— gRPC 服务器的**生命周期包装**
// =============================================================================
//  为什么需要这一层（不是过度设计）
//    C7.7 的护栏（`test_layering_guard.cpp`）规定：**除了 `adapters/grpc/` 之外，
//    `src/` 全树不得出现 `<grpcpp/` / proto 头**。组合根（`src/main/`）按 R12 必须
//    **创建** gRPC 服务，但"创建"不等于"直接抓住库类型" —— 与 `fss_http` 把
//    httplib 挡在 `common/http/` 内是同一条纪律（`src/main` 只 include 它的包装头）。
//
//  因此这里把 `grpc::ServerBuilder` / `grpc::Server` 收进 L5，只对外暴露：
//    · 启动（绑定端口、注册服务）
//    · 实际端口（`0`/负数 = 由系统分配，供测试与运维发现）
//    · `Shutdown()`（退出路径必须显式关闭，见 AGENTS.md §4.3 的 joinable 纪律）
//  组合根持有的是 `unique_ptr<GrpcServerHandle>`，看不到任何 grpc 类型。
// =============================================================================
#pragma once

#include "adapters/grpc/file_service_adapter.h"

#include <memory>
#include <string>

namespace fss::adapters::grpc {

class GrpcServerHandle {
 public:
  virtual ~GrpcServerHandle() = default;

  //  幂等；可重复调用（退出路径与异常路径都要能安全调用）
  virtual void Shutdown() = 0;
  //  实际绑定的端口（`StartGrpcServer` 传 0/负数时由系统分配）
  virtual int port() const = 0;
  //  启动是否成功（失败时 `port() == 0`，`last_error()` 给出原因）
  virtual bool ok() const = 0;
  virtual std::string last_error() const = 0;
};

//  注册 `service` 并开始服务。`port < 0` = 由系统分配端口（等价于 `:0`）。
//  ★ `service` 必须比返回的句柄活得久（与 Router/Server 的关系同理）。
std::unique_ptr<GrpcServerHandle> StartGrpcServer(FileServiceAdapter& service,
                                                 const std::string& bind_address, int port);

}  // namespace fss::adapters::grpc
