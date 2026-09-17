// =============================================================================
//  tests/framework/app_fixture.h —— 应用层用例的完整内存装配
// =============================================================================
//  13 个用例只依赖端口，因此可以用"三个内存适配器 + 端口替身"把它们完整驱动起来，
//  不需要真实存储/DB/网络，也不会有真实 sleep（时间来自 `ManualClock`，C2.8）。
//
//  ⚠️ 这里的数据面是**同一个** InMemoryBlobStore 同时承担 staging / persistent 两个
//     zone（工厂对两个 zone 返回同一指针），因此复制走 `IBlobStore::copy`（服务端复制）。
//     若要覆盖"跨 store 复制"的兜底分支，用 `factory.SetZoneStore(...)` 替换任一 zone。
#pragma once

#include "fake_ports.h"

#include "app/services/location_issuer.h"
#include "app/usecases/usecases.h"
#include "common/ids/id_generator.h"
#include "common/time/clock.h"
#include "domain/model/file_metadata.h"
#include "infra/blob/memory/memory_blob_store.h"
#include "infra/location/memory/memory_location_repository.h"
#include "infra/metadata/memory/memory_metadata_repository.h"

#include <memory>
#include <string>

namespace fss::test {

struct AppFixture {
  fss::ManualClock clock{1700000000};
  fss::SequentialIdGenerator ids{1};
  fss::infra::InMemoryBlobStore blob{clock};
  fss::infra::InMemoryLocationRepository locations;
  fss::infra::InMemoryMetadataRepository metadata{clock};
  RecordingSelfSignedCodec codec;
  AllowAllAuthorizer authorizer;
  RecordingEventPublisher events;
  RecordingAuditLogger audit;
  FakePartitionRegistry partitions;
  NoopLegalValidator legal;
  NoopSchemaValidator schema;
  FakeBlobStoreFactory factory{blob};
  std::unique_ptr<fss::app::LocationIssuer> issuer;
  std::unique_ptr<fss::app::UseCasePorts> ports;
  fss::app::CallerContext caller{"opendes", "osdu-user", "Bearer test-token"};

  AppFixture() {
    issuer = std::make_unique<fss::app::LocationIssuer>(factory, locations, codec, clock, ids,
                                                        "https://self.invalid");
    ports = std::make_unique<fss::app::UseCasePorts>(fss::app::UseCasePorts{
        factory, locations, metadata, authorizer, events, audit, partitions, legal, schema,
        *issuer, clock, ids});
  }

  //  构造一条合法的 `dataset--File.Generic` 记录（只填必需字段）
  //  ★ ACL 主体必须满足契约 §1.5 的 `^data\.[...]@<域名>$`，legal 必须带
  //    `otherRelevantDataCountries` —— 这两条以前没被任何测试碰到，因为
  //    `CreateFileMetadata` 当时**根本没有调用**领域校验（P4-D07）。
  //    生成"契约合法"的记录是本 fixture 的职责，否则每个用例都要自己纠正它。
  static fss::domain::FileMetadataRecord MakeRecord(const std::string& file_source,
                                                    const std::string& name = "sample.txt") {
    fss::domain::FileMetadataRecord record;
    record.kind = "opendes:wks:dataset--File.Generic:1.0.0";
    record.acl.viewers = {"data.default.viewers@opendes.example.com"};
    record.acl.owners = {"data.default.owners@opendes.example.com"};
    record.legal.legaltags = {"opendes-public-1"};
    record.legal.other_relevant_data_countries = {"US"};
    record.legal.status = fss::domain::LegalStatus::kCompliant;
    record.data.name = name;
    record.data.endian = "LITTLE";
    record.data.dataset_properties.file_source_info.file_source = file_source;
    return record;
  }
};

}  // namespace fss::test
