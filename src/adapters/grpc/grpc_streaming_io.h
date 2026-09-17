// =============================================================================
//  grpc_streaming_io（L5）—— gRPC 流 ↔ `ByteSource`/`ByteSink` 的翻译层
// =============================================================================
//  职责边界（只做翻译，不做判定）
//    · `GrpcUploadSource`：把客户端流的分片（`chunk`）暴露成 `bytes::ByteSource`，
//      交给 L4 用例与 `IBlobStore` 驱动**边收边写**（不整块驻留）。
//    · `GrpcDownloadSink`：把 `IBlobStore` 吐出的字节切成 ≤ `kChunkBytes` 的
//      服务端流分片（避免单个巨型 gRPC 帧）。
//
//  为什么不做"先收完再处理"
//    C7.6 要求 1 GiB 上传/下载的 RSS 峰值 < 64 MiB。任何"先攒起来"的实现都会立刻
//    变成 1 GiB 常驻（P6-D13 同类教训：把流式路径换成整块读回，RSS 直接抬到对象大小）。
//
//  协议约束（proto）：UploadFile 的**首个分片必须携带 `info`**；后续分片是 `chunk`；
//  **最后一个数据分片之后必须有 `end_of_stream = true`**。这里对"重复 info"
//  "未设置 payload""流没有结束标记就断了"都明确报错，而不是静默跳过——
//  静默跳过会让"客户端中断"表现为"上传了一个截断的对象"（P7-D07/P8-D05）。
// =============================================================================
#pragma once

#include "common/bytes/bytes.h"
#include "common/result/result.h"

#include <osdu/file/v1/file_service.grpc.pb.h>

#include <cstddef>
#include <string>

namespace fss::adapters::grpc {

class GrpcUploadSource final : public fss::bytes::ByteSource {
 public:
  //  ★ 调用方必须**已经**消费掉首个 `info` 分片，再把 reader 交进来。
  //  ★ 必须同时传 `ServerContext`：客户端**中途取消**时 `Read()` 也返回 false，
  //    与"正常读完（WritesDone）"无法区分。把取消当成 EOF 会让存储层把**截断的**
  //    字节当成完整对象提交（静默数据损坏）。这里显式区分，取消失败 → 报错。
  GrpcUploadSource(::grpc::ServerReader<osdu::file::v1::UploadFileRequest>& reader,
                   const ::grpc::ServerContext& context)
      : reader_(reader), context_(context) {}

  fss::Result<std::size_t> Read(char* out, std::size_t capacity) override;

  //  未知总长度（客户端流没有长度承诺）
  std::optional<std::int64_t> Size() const override { return std::nullopt; }

 private:
  ::grpc::ServerReader<osdu::file::v1::UploadFileRequest>& reader_;
  const ::grpc::ServerContext& context_;
  std::string buffer_;          // 当前分片剩余未消费的字节
  std::size_t cursor_ = 0;
  std::uint64_t chunks_ = 0;
  bool saw_end_of_stream_ = false;  // 客户端发过 `end_of_stream` 标记
};

class GrpcDownloadSink final : public fss::bytes::ByteSink {
 public:
  //  分片大小：64 KiB。太小会放大 gRPC 帧开销，太大则失去"边读边发"的意义。
  static constexpr std::size_t kChunkBytes = 64 * 1024;

  explicit GrpcDownloadSink(
      ::grpc::ServerWriter<osdu::file::v1::DownloadFileResponse>& writer)
      : writer_(writer) {}

  //  `data` 会被切成 ≤ kChunkBytes 的多个分片依次发出
  fss::Result<void> Write(std::string_view data) override;

  std::uint64_t messages_sent() const { return messages_sent_; }

 private:
  ::grpc::ServerWriter<osdu::file::v1::DownloadFileResponse>& writer_;
  std::uint64_t messages_sent_ = 0;
};

}  // namespace fss::adapters::grpc
