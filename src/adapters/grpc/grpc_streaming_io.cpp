// grpc_streaming_io 实现。职责划分见头文件。
#include "adapters/grpc/grpc_streaming_io.h"

#include <algorithm>
#include <cstring>

namespace fss::adapters::grpc {

fss::Result<std::size_t> GrpcUploadSource::Read(char* out, std::size_t capacity) {
  if (capacity == 0) return static_cast<std::size_t>(0);
  while (cursor_ >= buffer_.size()) {
    osdu::file::v1::UploadFileRequest message;
    if (!reader_.Read(&message)) {
      //  ★★ 判据是**显式结束标记**，不是 `IsCancelled()`：
      //    `Read()` 返回 false 在"客户端正常半关闭"与"流被中断/取消"两种情况下
      //    完全一样，而 `IsCancelled()` 的置位时机比 `Read()` 的失败**晚**（实测：
      //    ASan 构建下取消时的 `Read()==false` 时它仍是 false）→ 曾经把 512 KiB 的
      //    截断对象 rename 成了正式对象（P8-D05，普通构建碰不到、sanitizer 下必现）。
      //    没有结束标记 = 流不完整 = 报错，绝不提交。
      if (!saw_end_of_stream_) {
        return Err(fss::ErrorKind::kUnavailable,
                   context_.IsCancelled()
                       ? "上传已被客户端取消（字节流不完整）"
                       : "上传流在收到 end_of_stream 之前就结束了（客户端中断或协议错误）");
      }
      return static_cast<std::size_t>(0);  // EOF：客户端已按协议 WritesDone
    }
    //  ★ `chunk` 是 oneof 里的 `bytes`（标量），没有 `has_chunk()`：
    //    必须用 `payload_case()` 判断，否则"未设置 payload"会被当成"空的 chunk"静默放行。
    switch (message.payload_case()) {
      case osdu::file::v1::UploadFileRequest::kInfo:
        return Err(fss::ErrorKind::kInvalidArgument, "UploadFile 的 info 只能出现在第一个分片");
      case osdu::file::v1::UploadFileRequest::kChunk:
        break;
      case osdu::file::v1::UploadFileRequest::kEndOfStream:
        //  结束标记之后不允许再有分片（有的话下一轮会读到，这里直接拒绝）
        saw_end_of_stream_ = true;
        buffer_.clear();
        cursor_ = 0;
        continue;
      case osdu::file::v1::UploadFileRequest::PAYLOAD_NOT_SET:
      default:
        return Err(fss::ErrorKind::kInvalidArgument,
                   "UploadFile 的分片必须携带 chunk（payload 未设置）");
    }
    if (saw_end_of_stream_) {
      return Err(fss::ErrorKind::kInvalidArgument, "end_of_stream 之后不允许再有数据分片");
    }
    buffer_ = message.chunk();
    cursor_ = 0;
    ++chunks_;
    //  ★ 空 chunk 是合法的（客户端可以发空分片），继续读下一片而不是当成 EOF
  }
  const std::size_t available = buffer_.size() - cursor_;
  const std::size_t count = std::min(capacity, available);
  std::memcpy(out, buffer_.data() + cursor_, count);
  cursor_ += count;
  if (cursor_ >= buffer_.size()) buffer_.clear();
  return count;
}

fss::Result<void> GrpcDownloadSink::Write(std::string_view data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t count = std::min(kChunkBytes, data.size() - offset);
    osdu::file::v1::DownloadFileResponse message;
    message.set_chunk(data.data() + offset, count);
    //  ★ `Write` 返回 false = 客户端已取消/断开。必须**立刻**把它变成错误，
    //    否则存储层会继续把整个对象读完（1 GiB 的读盘白干，且句柄占用到结束）。
    if (!writer_.Write(message)) {
      return Err(fss::ErrorKind::kUnavailable, "下载流已断开（gRPC 写失败）");
    }
    ++messages_sent_;
    offset += count;
  }
  return Ok();
}

}  // namespace fss::adapters::grpc
