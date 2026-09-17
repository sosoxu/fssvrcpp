// =============================================================================
//  TransferEndpoint（L2）—— `/v1/transfer/{token}` 的**内核**
// =============================================================================
//  这是集中存储的数据面入口：客户端拿自签 URL 直连本服务，服务端校验 token 后
//  代理字节。P3 只交付**内核**（token 校验 + 字节转发）；HTTP 路由注册属 P4。
//
//  为什么"跨操作/跨租户"的判断在这里而不在 codec：
//    codec 只回答"载荷有没有被改"（密码学问题）；"这个 token 能不能用来做这次 PUT /
//    访问这个 partition"是对**请求上下文**（HTTP 方法、`data-partition-id`）的判断，
//    属于内核。两者分开后，codec 可以独立测试，内核也能对"合法签名的越权使用"说不。
//
//  ★ 分层：本文件是 L2，只依赖端口与领域类型。容器名来自 token 载荷
//    （`TransferToken.container`），因此**不需要**依赖 L4 的 `ObjectKeyPolicy`。
#pragma once

#include "common/bytes/bytes.h"
#include "common/result/result.h"
#include "domain/ports/ports.h"

#include <string>
#include <string_view>

namespace fss::infra {

//  一次传输请求的上下文（由 P4 的 HTTP 适配层从路径 + 查询串 + 头组装）
struct TransferRequest {
  std::string token;
  std::string expires;    // `?exp=`（epoch 秒，字符串形态）
  std::string signature;  // `?sig=`
  std::string op;         // 来自 HTTP 方法："put" / "get"
  std::string partition;  // 来自 `data-partition-id`；空串 = 不做租户绑定的二次校验
};

class TransferEndpoint {
 public:
  TransferEndpoint(domain::ISelfSignedUrlCodec& codec, domain::IBlobStoreFactory& blobs)
      : codec_(codec), blobs_(blobs) {}

  //  上传：token 必须是 `put`，字节从 body 流入存储
  fss::Result<void> Put(const TransferRequest& request, bytes::ByteSource& body);

  //  下载：token 必须是 `get`，字节从存储流向 sink（支持 Range）
  fss::Result<void> Get(const TransferRequest& request, bytes::ByteSink& sink,
                        const domain::ByteRange& range);

  //  只校验 + 解析（不碰 IO）。组合根用它实现"一次校验、多次定位读"的流式下载
  //  （避免每个 64 KiB 分块都重算一次 HMAC）。
  fss::Result<domain::TransferToken> Resolve(const TransferRequest& request,
                                             std::string_view expected_op);

  //  目标对象的 stat（校验 `get` 权限后），用于 Content-Length
  fss::Result<domain::ObjectStat> Stat(const TransferRequest& request);

 private:
  domain::ISelfSignedUrlCodec& codec_;
  domain::IBlobStoreFactory& blobs_;
};

}  // namespace fss::infra
