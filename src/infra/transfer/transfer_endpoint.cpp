// TransferEndpoint 实现。职责划分见头文件。
#include "infra/transfer/transfer_endpoint.h"

#include <string>

namespace fss::infra {

namespace {

domain::ObjectRef ObjectRefFromToken(const domain::TransferToken& token) {
  domain::ObjectRef ref;
  ref.container = token.container;
  ref.key = token.object_key;
  return ref;
}

}  // namespace

fss::Result<domain::TransferToken> TransferEndpoint::Resolve(const TransferRequest& request,
                                                             std::string_view expected_op) {
  FSS_TRY(token, codec_.Decode(request.token, request.expires, request.signature));

  //  ① 操作绑定：`get` token 不能用来 PUT（反之亦然）
  if (token.op != expected_op) {
    return Err(fss::ErrorKind::kPermissionDenied,
               "transfer token 的操作类型不匹配（期望 " + std::string(expected_op) + "，实际 " +
                   token.op + "）");
  }

  //  ② 租户绑定：带 `data-partition-id` 时必须一致（跨租户重放拒绝）
  if (!request.partition.empty() && request.partition != token.partition) {
    return Err(fss::ErrorKind::kPermissionDenied, "transfer token 与请求的 partition 不一致");
  }

  //  ③ 目标对象来自**载荷**（不接受任何来自查询串/头的键，避免"参数覆盖"类漏洞）
  if (token.container.empty() || token.object_key.empty()) {
    return Err(fss::ErrorKind::kUnauthenticated, "transfer token 缺少 container/object_key");
  }
  return token;
}

fss::Result<void> TransferEndpoint::Put(const TransferRequest& request, bytes::ByteSource& body) {
  FSS_TRY(token, Resolve(request, "put"));
  const domain::ObjectRef ref = ObjectRefFromToken(token);
  FSS_TRY(store, blobs_.ForPartition(token.partition, token.zone));
  //  容器由签发方（LocationIssuer）在签发时保证存在；这里再 ensure 一次是幂等的纵深防御。
  FSS_TRY(store->ensure_container(ref.container));
  return store->put(ref, body, domain::PutOptions{});
}

fss::Result<domain::ObjectStat> TransferEndpoint::Stat(const TransferRequest& request) {
  FSS_TRY(token, Resolve(request, "get"));
  const domain::ObjectRef ref = ObjectRefFromToken(token);
  FSS_TRY(store, blobs_.ForPartition(token.partition, token.zone));
  FSS_TRY(stat, store->stat(ref));
  //  ★ 对象不存在必须报 404，**不能**返回 size=0：否则调用方把它当 0 字节对象，
  //    给客户端一个 200 + 空体的"静默空文件"（P4-D06）。
  if (!stat.exists) {
    return Err(fss::ErrorKind::kNotFound, "传输目标对象不存在：" + ref.key);
  }
  return stat;
}

fss::Result<void> TransferEndpoint::Get(const TransferRequest& request, bytes::ByteSink& sink,
                                        const domain::ByteRange& range) {
  FSS_TRY(token, Resolve(request, "get"));
  const domain::ObjectRef ref = ObjectRefFromToken(token);
  FSS_TRY(store, blobs_.ForPartition(token.partition, token.zone));
  return store->get(ref, sink, range);
}

}  // namespace fss::infra
