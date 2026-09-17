// grpc_server 实现。职责与理由见头文件。
#include "adapters/grpc/grpc_server.h"

#include <grpcpp/grpcpp.h>

#include <utility>

namespace fss::adapters::grpc {

namespace {

class GrpcServerHandleImpl final : public GrpcServerHandle {
 public:
  GrpcServerHandleImpl(std::unique_ptr<::grpc::Server> server, int port, std::string error)
      : server_(std::move(server)), port_(port), error_(std::move(error)) {}

  void Shutdown() override {
    if (server_) {
      server_->Shutdown();
      server_.reset();
    }
  }
  int port() const override { return port_; }
  bool ok() const override { return server_ != nullptr; }
  std::string last_error() const override { return error_; }

 private:
  std::unique_ptr<::grpc::Server> server_;
  int port_ = 0;
  std::string error_;
};

}  // namespace

std::unique_ptr<GrpcServerHandle> StartGrpcServer(FileServiceAdapter& service,
                                                 const std::string& bind_address, int port) {
  const int requested = port < 0 ? 0 : port;  // 负数 = 让系统分配
  ::grpc::ServerBuilder builder;
  int bound_port = 0;
  builder.AddListeningPort(bind_address + ":" + std::to_string(requested),
                           ::grpc::InsecureServerCredentials(), &bound_port);
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  if (server == nullptr || bound_port == 0) {
    return std::make_unique<GrpcServerHandleImpl>(
        nullptr, 0,
        "gRPC 监听失败：" + bind_address + ":" + std::to_string(requested) +
            "（端口被占用或地址不可用）");
  }
  return std::make_unique<GrpcServerHandleImpl>(std::move(server), bound_port, "");
}

}  // namespace fss::adapters::grpc
