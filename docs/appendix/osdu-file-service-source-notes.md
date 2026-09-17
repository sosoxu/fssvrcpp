# OSDU File Service — Reference Research for a C++ Reimplementation

**Source of truth used:** a full shallow clone of `https://community.opengroup.org/osdu/platform/system/file.git`
(project id **90**, branch **master**, HEAD **`d7c25c2d7f5d2f42bed901c68a407098195389bb`**, 2026-09-11,
*"Merge branch 'fix_schemathesis_api_check_tests'"*), plus tag-pinned raw fetches for historical behaviour and
cross-repo fetches from `os-core-common` (project 67), `data-definitions`, and `os-core-lib-azure` (project 77).

Every path/URL quoted below was fetched successfully (HTTP 200) unless explicitly marked otherwise.
Claims I could not confirm are marked **UNVERIFIED**.

---

## 0. TL;DR — the 12 things that matter most for a reimplementation

1. The service is deployed under context path **`/api/file/`** (`server.servlet.contextPath=/api/file/` in every
   provider's `application.properties`). All paths below are shown **with** that prefix.
2. There is **no `/v1` API in current master**. Everything is `/v2/...`. A partial `/v1` surface existed only in
   tags **≤ v0.5.0** (≈2021).
3. There is **no `PUT` mapping anywhere** and **no `/v2/files/{id}/versions` endpoint** in the File Service repo
   (master or any tag). Those must come from another service (Storage has `/records/{id}/{version}`).
4. File bytes are **never proxied**. The service returns a **pre-signed URL** (Azure SAS, GCS/S3 signed URL) that
   the client uses directly against blob storage.
5. Metadata lives in the **Storage service** (`os-core-common` DataLake client → `PUT /records`), *not* in the File
   Service's own DB. The File Service's own DB (Cosmos / Cloudant / Datastore / DynamoDB) stores only
   **`FileLocation`** rows (fileID → driver → location), used by the legacy landing-zone endpoints.
6. `POST /v2/files/metadata` performs the **staging→persistent copy**, computes the checksum, upserts the record into
   Storage, publishes status/datasetDetails events, and then **deletes the staging object** — all synchronously.
7. The **`Driver`** field is a `DriverType` enum in `os-core-common` whose **only value is `GCS`**; the generic
   `LocationServiceImpl` hardcodes `DriverType.GCS` for every cloud. So `getFileLocation` returns `"GCS"` even on Azure/AWS/IBM.
8. Inter-service clients are plain HTTP/JSON: **Storage** (`storage.api`), **Entitlements** (`osdu.entitlements.url`),
   **Legal** and **Schema** are reached *through the Storage service*, not directly.
9. Only two metadata-persistence technologies appear in this repo for OSDU-native storage: none — the File Service
   itself never writes to Elasticsearch or Postgres. Elasticsearch/Postgres indexing is the **Storage service's** job.
10. **No OpenAPI code generation.** WADL/Spring annotations generate the spec at runtime via **springdoc**
    (`/v2/api-docs`, `/v2/api-docs.yaml`, `/v2/swagger`). One hand-maintained spec exists at
    `docs/api/community/v2/openapi.yaml` and is used only by a CI "OpenAPI spec check" job.
11. Auth = `data-partition-id` header + `Authorization: Bearer` + Entitlements `authorizeAny(...)` against
    `service.file.viewers` / `service.file.editors` / `service.file.admin` (plus `service.dataset.*`,
    `service.storage.*`, `service.delivery.viewer` for the DMS/Delivery APIs).
12. Metadata POST is validated only structurally (kind shape, non-empty ACL/owners/viewers, non-empty
    `FileSource`, legal tags present). **Legal-tag compliance and ACL/group existence are enforced by the
    Storage service**, not by the File Service.

---

## 1. HTTP API surface

### 1.1 Current master (v2) — extracted from `@GetMapping/@PostMapping/@DeleteMapping` in `file-core/src/main/java/org/opengroup/osdu/file/api/*.java`

Verification command used on the clone:

```
grep -rnE "@(Get|Post|Put|Delete|Patch|Request)Mapping" --include=*.java */src/main/java
```

| # | Method | Full path | Controller method | Required role (`@PreAuthorize`) | Success |
|---|--------|-----------|-------------------|--------------------------------|---------|
| 1 | `POST` | `/api/file/v2/getLocation` | `FileLocationApi.getLocation(LocationRequest)` | `service.file.editors` | `200` `LocationResponse` |
| 2 | `POST` | `/api/file/v2/getFileLocation` | `FileLocationApi.getFileLocation(FileLocationRequest)` | `service.file.editors` | `200` `FileLocationResponse` |
| 3 | `GET`  | `/api/file/v2/files/uploadURL` | `FileLocationApi.getLocationFile(expiryTime)` | `service.file.editors` | `200` `LocationResponse` |
| 4 | `POST` | `/api/file/v2/getFileList` | `FileListApi.getFileList(FileListRequest)` | `service.file.editors` | `200` `FileListResponse` |
| 5 | `POST` | `/api/file/v2/files/metadata` | `FileMetadataApi.postFilesMetadata(FileMetadata)` | `service.file.editors` | `201` `FileMetadataResponse` |
| 6 | `GET`  | `/api/file/v2/files/{id}/metadata` | `FileMetadataApi.getFileMetadataById(id)` | `service.file.viewers` | `200` `RecordVersion` |
| 7 | `DELETE` | `/api/file/v2/files/{id}/metadata` | `FileMetadataApi.deleteFileMetadataById(id)` | `service.file.editors`, `service.file.admin` | `204` (no body) |
| 8 | `GET`  | `/api/file/v2/files/{id}/downloadURL` | `FileDeliveryApi.downloadURL(id, expiryTime)` | `service.file.viewers` | `200` `DownloadUrlResponse` |
| 9 | `POST` | `/api/file/v2/files/revokeURL` | `FileAdminApi.revokeURL(Map<String,String>)` | `service.file.admin` | `204` (no body) |
| 10 | `POST` | `/api/file/v2/files/storageInstructions` | `FileDmsApi.getStorageInstructions(expiryTime)` | `service.dataset.editors` | `200` `StorageInstructionsResponse` |
| 11 | `POST` | `/api/file/v2/files/retrievalInstructions` | `FileDmsApi.getRetrievalInstructions(body, expiryTime)` | `service.dataset.viewers` | `200` `RetrievalInstructionsResponse` |
| 12 | `POST` | `/api/file/v2/files/copy` | `FileDmsApi.copyDms(CopyDmsRequest)` | `service.storage.creator`, `service.storage.admin` | `200` `List<CopyDmsResponse>` |
| 13 | `POST` | `/api/file/v2/file-collections/storageInstructions` | `FileCollectionDmsApi` | `service.dataset.editors` | `200` |
| 14 | `POST` | `/api/file/v2/file-collections/retrievalInstructions` | `FileCollectionDmsApi` | `service.dataset.viewers` | `200` |
| 15 | `POST` | `/api/file/v2/file-collections/copy` | `FileCollectionDmsApi` | `service.storage.creator`, `service.storage.admin` | `200` |
| 16 | `POST` | `/api/file/v2/delivery/GetFileSignedUrl` | `DeliveryApi.getFileSignedURL(UrlSigningRequest)` | `service.delivery.viewer` | `200` `UrlSigningResponse` |
| 17 | `GET`  | `/api/file/v2/info` | `InfoApi.info()` | none (`@RequestMapping` only) | `200` `VersionInfo` |
| 18 | `GET`  | `/api/file/v2/liveness_check` | `HealthCheckApi.livenessCheck()` | `@PermitAll` | `200` text `"File service is alive"` |
| 19 | `GET`  | `/api/file/v2/readiness_check` | `HealthCheckApi.readinessCheck()` | `@PermitAll` | `200` text `"File service is ready"` |
| 20 | `GET`  | `/api/file/v2/api-docs`, `/api/file/v2/api-docs.yaml`, `/api/file/v2/api-docs/swagger-config` | springdoc | — | `200` |
| 21 | `GET`  | `/api/file/v2/swagger` → Swagger UI (`/v2/swagger-ui/index.html`) | springdoc | — | `200` HTML |

Notes verified in source:

* `FileLocationApi`, `FileListApi`, `FileDmsApi`, `FileCollectionDmsApi`, `DeliveryApi` are annotated `@Hidden`, so
  they do **not** appear in the generated OpenAPI document (they are internal/DMS endpoints).
* `HealthCheckApi` methods are `jakarta.annotation.security.PermitAll`; `InfoApi` has no auth annotation, and the
  deployment authz policies explicitly allow `/api/file/v2/info` unauthenticated
  (e.g. `devops/azure/chart/templates/azure-istio-auth-policy.yaml` line 39, `devops/ibm/ibm-file-deploy/templates/istio-authzpolicy.yaml`).
* There is **no** `/api/file/health`, no `/api/file/v2/health`, no `/v2/files/{id}/versions`, no `PUT` on
  `/{id}/metadata`. Confirmed by exhaustive mapping grep and by the CI-exposed spec route being `/v2/api-docs.yaml`.
* GCP (`file-core-plus`) additionally exposes Spring Boot actuator health on a **separate management port**:
  `management.server.port=${MANAGEMENT_SERVER_PORT:8081}`, `management.endpoints.web.base-path=/`,
  `management.endpoints.web.exposure.include=health` (`file-core-plus/src/main/resources/application.properties`).

### 1.2 Exact query parameters

The only query parameter on any endpoint is **`expiryTime`** (optional, `String`):

```
The Time for which Signed URL to be valid. Accepted Regex patterns are "^[0-9]+M$", "^[0-9]+H$", "^[0-9]+D$"
denoting Integer values in Minutes, Hours, Days respectively. In absence of this parameter the URL would be
valid for 1 Hour.
```

Present on: `/v2/files/uploadURL` (GET), `/v2/files/{id}/downloadURL` (GET), `/v2/files/storageInstructions`,
`/v2/files/retrievalInstructions`, `/v2/file-collections/storageInstructions`,
`/v2/file-collections/retrievalInstructions`.

Semantics from `file-core/src/main/java/org/opengroup/osdu/file/util/ExpiryTimeUtil.java`:

* default TTL = **1 hour** (`DEFAULT_TTL = 1L`, `TimeUnit.HOURS`);
* capped TTL = **7 days** (`CAPPED_DEFAULT_TTL = 7L`, `TimeUnit.DAYS`) — a request larger than 7 days is silently
  clamped to 7 days;
* an `expiryTime` that does not match `^[0-9]+M$|^[0-9]+H$|^[0-9]+D$` throws
  `OsduBadRequestException(ErrorMessages.INVALID_EXPIRY_TIME_PATTERN)` → HTTP 400, message:
  `expiryTime pattern isn't supported. Value should be one of these regex patterns ^[0-9]+M$ , ^[0-9]+H$ , ^[0-9]+D$`.

> **Doc/code divergence (verified):** `docs/docs/File-Service.md` says the download URL defaults to **7 days** when
> `expiryTime` is absent. The code (`ExpiryTimeUtil`) defaults to **1 hour**, capped at 7 days. The provider
> overrides in `swagger.properties` also mix "1 Hour" and "7 days maximum". Trust the code.

### 1.3 Request / response bodies

**`LocationRequest`** (`os-core-common`, `org.opengroup.osdu.core.common.model.file.LocationRequest`)
```json
{ "FileID": "optional-existing-file-id" }
```
**`LocationResponse`** — note the capitalisation and that `Location` is `Map<String,String>`:
```json
{ "FileID": "da92f52401dc4d1cb93515f159c110d4",
  "Location": { "SignedURL": "https://...&sig=...", "FileSource": "/osdu-user/1614784413120-2021-03-03-15-13-33-120/da92f52401dc4d1cb93515f159c110d4" } }
```
Key names `SignedURL` and `FileSource` verified in `AzureLocationMapper`, GCP `LocationMapper`, IBM `IBMLocationMapper`
(all three define `SIGNED_URL_KEY = "SignedURL"` and `FILE_SOURCE(_KEY) = "FileSource"`).

**`FileLocationRequest`** `{ "FileID": "..." }` →
**`FileLocationResponse`**:
```json
{ "Driver": "GCS", "Location": "https://<account>/<container>/<path>" }
```
(`FileLocationResponse.driver` is typed `Object` in `os-core-common`; serialised value comes from `DriverType`.)

**`FileListRequest`** / **`FileListResponse`** (`os-core-common`):
```json
{ "TimeFrom": "2021-03-01T00:00:00", "TimeTo": "2021-03-31T00:00:00", "PageNum": 0, "Items": 10, "UserID": "user@x.com" }
```
```json
{ "Content": [ { "FileID": "...", "Driver": "GCS", "Location": "...", "CreatedAt": "2021-03-03T15:13:33.120+0000", "CreatedBy": "osdu-user" } ],
  "Number": 0, "NumberOfElements": 1, "Size": 10 }
```
`FileLocation.Fields` are exactly `FileID`, `Driver`, `Location`, `CreatedAt` (pattern `yyyy-MM-dd'T'HH:mm:ss.SSSZ`),
`CreatedBy`.

**`DownloadUrlResponse`**: `{ "SignedUrl": "https://..." }`

**`FileMetadataResponse`**: `{ "id": "opendes:dataset--File.Generic:0e1d0e29..." }`

**`revokeURL` request body** — arbitrary string map; Azure (the only implementer found) requires:
```json
{ "resourceGroup": "<rg>", "storageAccount": "<account>" }
```
`StorageServiceImpl.validateInputFor` throws `OsduBadRequestException("Illegal argument for resourceGroup { x } or storageAccount { y }")` if either is blank.

**DMS bodies** (`os-core-common`, package `org.opengroup.osdu.core.common.dms.model`):
```java
class StorageInstructionsResponse { String providerKey; Map<String,Object> storageLocation; }
class RetrievalInstructionsResponse { List<DatasetRetrievalProperties> datasets; }
class DatasetRetrievalProperties { String datasetRegistryId; Map<String,Object> retrievalProperties; String providerKey; }
class CopyDmsRequest { List<Record> datasetSources; }        // request body: {"datasetSources":[ ...records... ]}
class CopyDmsResponse { boolean success; String datasetBlobStoragePath; }
```
For the **File** DMS path (`/v2/files/storageInstructions`) Azure returns `providerKey = "AZURE"` and
`storageLocation = { signedUrl, fileSource, createdBy, expiryTime }` (`AzureFileDmsUploadLocation`); AWS returns
`StorageInstructionsResponse.providerKey = fileLocationProvider.getProviderKey()` and a `FileDmsStorageLocation`
with `unsignedUrl, signedUrl, fileSource, createdAt, connectionString, credentials, createdBy, signedUploadFileName, region`.

For `/v2/delivery/GetFileSignedUrl`: request `UrlSigningRequest { srns: [...] }` → `UrlSigningResponse`
(`file-core/.../model/delivery/UrlSigningRequest.java`, `UrlSigningResponse.java`).

### 1.4 Error codes

Controller `@ApiResponses` declare the same matrix for essentially every endpoint:
`400` (bad input / validation), `401` (Unauthorized / missing token or partition), `403` (not authorized),
`404` (record not found), `500`, `502`, `503`; plus `201`/`204`/`200` for success. There is no `409` (an existing
FileID returns **400** via `LocationAlreadyExistsException`, handled by the `handleBadRequest` advice).

Two distinct error body shapes exist (both verified in `file-core/.../exception/handler/RestExceptionHandler.java`):

* `ErrorResponse` — wrapped as `{"error": { "code": 400, "message": "...", "errors": [ ... ] }}`
  (`@JsonTypeInfo(WRAPPER_OBJECT)` + `@JsonTypeName("error")`), used for `StorageException`,
  `ApplicationException`, `OsduBadRequestException`, `NotFoundException`, `OsduUnauthorizedException`,
  method-argument-not-valid.
* `ApiError` — `{ "status": "BAD_REQUEST", "message": "...", "errors": ["..."] }`, used for
  `ConstraintViolationException` (bean validation on `LocationRequest`/`FileLocationRequest`/`FileListRequest`),
  `JsonParseException`, `IllegalArgumentException`, `MismatchedInputException`, `HttpMessageNotReadableException`.
* `AppException` (thrown by provider code, e.g. AWS "Malformed URL") is serialised as the `AppError` triple
  `{ "code", "reason", "message" }`.

Common concrete messages (verified in source):
`"Invalid source file path to copy from <path>"`, `"FileSource can not be empty"`, `"Record Not Found"`,
`"Not found location for fileID : <id>"`, `"Location for fileID = <id> already exists"`,
`"Invalid kind"`, `"Invalid source in kind"`, `"Invalid entity in kind"`,
`"Missing authorization token"`, `"Missing partitionID"`.

Validation messages the acceptance tests assert on (from `file-acceptance-test/.../features/`):
`"ConstraintViolationException: Invalid FileLocationRequest"`, `"Not found location for fileID : test"`,
and the per-field messages for missing `kind`/`acl`/`legal`/`data`/`FileSource`/invalid `Endian`.

### 1.5 Legacy v1 surface (verified at tag **v0.5.0**)

At `v0.5.0` (`file-core/src/main/java/.../api/`) the mappings were:

| Method | Path (with `/api/file` context) | Class |
|--------|---------------------------------|-------|
| `POST` | `/api/file/v1/files/metadata` | `FileMetadataApi` (`@RequestMapping("/v1/files")`) |
| `GET`  | `/api/file/v1/files/{id}/metadata` | `FileMetadataApi` |
| `GET`  | `/api/file/v1/files/uploadURL` | `FileLocationApi` |
| `GET`  | `/api/file/v1/files/{id}/downloadURL` | `FileDeliveryApi` |
| `POST` | `/api/file/getLocation` | `FileLocationApi` |
| `POST` | `/api/file/getFileLocation` | `FileLocationApi` |
| `POST` | `/api/file/getFileList` | `FileListApi` |
| `POST` | `/api/file/getFile` | `FileApi` |

At `v0.4.0` the metadata paths did not exist; at `v0.7.0` everything had already moved to `/v2/...`
(`@RequestMapping(value = "/v2/files")`, `/v2/getLocation`, `/v2/getFileLocation`, `/v2/getFileList`,
`/v2/files/uploadURL`, `/v2/files/{id}/downloadURL`, `/v2/getFile`). There is **no tag** in which
`/v1/getLocation` or `/v1/getFileLocation` existed — those were always unversioned.

> A legacy, now-deleted spec file `docs/docs/file-service_openapi.yaml` (OpenAPI **3.0.0**, `info.version: 1.0.0`,
> 1518 lines) documented this generation. It is retrievable at the parent commit of the spec-check commit:
> `https://community.opengroup.org/osdu/platform/system/file/-/raw/fdf450efa0403b93f1bafe62b3e7ac7797103a25/docs/docs/file-service_openapi.yaml`
> (verified HTTP 200). Its path list: `/v2/getLocation`, `/v2/files/uploadURL`, `/v2/files/metadata`,
> `/v2/files/{Id}/metadata` (get + delete), `/v2/files/{Id}/downloadURL`, `/v2/getFileLocation`,
> `/v2/delivery/getFileSignedUrl`, `/v2/getFileList`, `/v2/info`,
> `/v2/file-collections/storageInstructions|retrievalInstructions|copy`.
> It contains **no** `PUT`, no `/{id}/versions`, and no `artifact` metadata field (the only "artifact" hits are the
> typo `actifactId` inside the `VersionInfo` schema).

---

## 2. The signed-URL / location model

### 2.1 Why pre-signed URLs instead of proxying bytes

Stated directly in the API descriptions (e.g. `file-core/.../api/FileLocationApi.java`, `swagger.properties`):

> *"Gets a temporary signed URL to upload a file (**Service does not upload the file by itself**, User needs to use
> this URL to upload the file). The generated URL is time bound and by default expires by 7 days maximum."*

Practical consequences encoded in the design:

* The service never touches payload bytes for upload or download. It creates an **empty blob** (Azure
  `StorageImpl.internalCreate` uploads a zero-length byte array, `EMPTY_BYTE_ARRAY`) and returns a
  write-permission SAS / signed URL. For download it signs an existing blob with read permission only.
* The signed URL is the contract boundary; the File Service only knows the **relative path** (`FileSource`), never
  the bytes. `docs/docs/File-Service.md`: *"File Service doesn't perform any verification whether a file upload
  happened"* and *"The File service doesn't look inside the file to validate the content within."*
* Checksums are computed server-side only at metadata-POST time, by reading blob properties
  (`BlobStore.readBlobProperties` → `getContentMd5()`) or by streaming the blob
  (`BlobStore.getBlobInputStream`) when provider MD5 is absent, with a size guard
  `CHECKSUM_CALCULATION_LIMIT` (Azure default `5368709120` = 5 GiB).

### 2.2 What the `Driver` field means

* It is the cloud/backend identifier for the stored object.
* Type: `org.opengroup.osdu.core.common.model.file.DriverType` in `os-core-common`, JSON name `Driver`
  (`FileLocation.Fields.DRIVER = "Driver"`). **Verified content of the enum is a single value:**
  ```java
  public enum DriverType { GCS }
  ```
* The generic `LocationServiceImpl` (`file-core/.../service/LocationServiceImpl.java`) hardcodes it:
  ```java
  FileLocation fileLocation = FileLocation.builder()
      .fileID(fileID)
      .driver(DriverType.GCS)                 // <-- hardcoded for all providers
      .location(signedUrl.getUri().toString())
      .createdBy(signedUrl.getCreatedBy())
      .createdAt(Date.from(signedUrl.getCreatedAt()))
      .build();
  ```
  So in the community reference, `POST /v2/getFileLocation` answers `"Driver": "GCS"` regardless of the deployed
  cloud. For a C++ reimplementation, treat `Driver` as an opaque enum string and *do not* rely on it to discriminate
  backends. **UNVERIFIED:** whether any vendor fork overrides `LocationServiceImpl` to emit `AZURE`/`S3`/`IBM`.
* `getFileLocation` is explicitly marked deprecated in the docs: *"This endpoint will be deprecated… Please refer to
  `/v2/files/{Id}/downloadURL`."* `getLocation` is likewise deprecated in favour of `/v2/files/uploadURL`.

### 2.3 Landing zone vs persistent zone workflow

Zones are **two separate containers/buckets per partition**. Property names verified per provider:

| Provider | Staging (landing) | Persistent | File |
|---|---|---|---|
| Azure (blob) | `azure.storage.staging-area=file-staging-area` | `azure.storage.persistent-area=file-persistent-area` | `provider/file-azure/src/main/resources/application.properties` |
| Azure (ADLS Gen2, collections) | `azure.datalake.staging-area=datalake-staging-area` | `azure.datalake.persistent-area=datalake-persistent-area` | same |
| GCP | partition property `file.staging.location` (default `staging-area`) | `file.persistent.location` (default `persistent-area`) | `provider/file-gc/src/main/resources/application.properties`, `PartitionPropertyNames` |
| IBM COS | `ibm.staging.bucket=staging-bucket` | `ibm.persistent.bucket=persistent-bucket` | `provider/file-ibm/src/main/resources/application.properties` |
| AWS (tag `v0.28.0-aws.1` only) | `ProviderConfigurationBag` | `ProviderConfigurationBag` | `provider/file-aws/.../config/ProviderConfigurationBag.java` |

Zone resolution is done by `IStorageUtilService`:
```java
default String getPersistentLocation(String relativePath, String partitionId) { return null; }
default String getStagingLocation(String relativePath, String partitionId) { return null; }
```
Azure's `StorageUtilServiceImpl` builds `String.format("%s/%s/%s", storageAccountURL, container, normalized)`.

**Upload → metadata → copy → delete (the full sequence), from
`file-core/src/main/java/org/opengroup/osdu/file/service/FileMetadataService.java#saveMetadata`:**

1. `fileStatusPublisher.publishInProgressStatus()` — publishes a `status` / `DATASET_SYNC` / `IN_PROGRESS` event.
2. `validateKind(kind)` — kind must split on `:` into 4 parts, `[1] == "wks"`, `[2] == "dataset--File.Generic"`.
3. `filePath = data.DatasetProperties.FileSourceInfo.FileSource` (the relative path returned by `uploadURL`).
4. `id = generateRecordId(partitionId, entityFromKind)` → `"<partition>:dataset--File.Generic:<uuid-without-dashes>"`.
5. `stagingLocation  = storageUtilService.getStagingLocation(filePath, partitionId)`
   `persistentLocation = storageUtilService.getPersistentLocation(filePath, partitionId)`
6. `cloudStorageOperation.copyFile(stagingLocation, persistentLocation)` — server-side copy (Azure
   `BlobStore.copyFile` → `BlockBlobClient.beginCopy`; GCP `obmDriver.copyBlob`; S3 copy).
7. `checksum = storageUtilService.getChecksum(persistentLocation)`; if non-blank, it is written back into
   `FileSourceInfo.Checksum` plus `ChecksumAlgorithm`.
8. `Record record = fileMetadataRecordMapper.fileMetadataToRecord(fileMetadata)` (id, acl, legal, kind, ancestry,
   data-as-map, meta, tags).
9. `dataLakeStorage.upsertRecord(record)` → `PUT {storage.api}/records` on the **Storage service**.
10. `fileStatusPublisher.publishSuccessStatus(recordId, version)` and
    `fileDatasetDetailsPublisher.publishDatasetDetails(recordId, version)`.
11. `cleanupStagingLocation(...)` — re-reads the record via `dataLakeStorage.getRecord(id)`; if found, deletes the
    staging object. Deletion failures are **caught and ignored** (issue #76) — they must not fail the request.
12. On `StorageException` or generic exception: `cloudStorageOperation.deleteFile(persistentLocation)` (rollback of
    the copy) + `publishFailureStatus`, then rethrow.

**Delete** (`deleteMetadataRecord`): `getMetadataById(id)` → `dataLakeStorage.deleteRecord(id)`
(`POST {storage.api}/records/{id}:delete`, must return `204`) → `cloudStorageOperation.deleteFile(persistentLocation)`.

**Landing-zone auto-expiry:** the docs promise the uploaded landing-zone object *"gets automatically deleted, if the
metadata is not posted within 24 hours of uploading the file."* This is a **bucket/container lifecycle rule in the
deployment**, not code in this repo. I found no lifecycle configuration in `devops/` — **UNVERIFIED** how it is
actually configured per cloud.

**Concrete path layout** (Azure, `StorageServiceImpl.getFileLocationPrefix`):
```
<file.location.userId>/<epochMillis>-<yyyy-MM-dd-HH-mm-ss-SSS>/<fileID>
e.g. osdu-user/1614784413120-2021-03-03-15-13-33-120/da92f52401dc4d1cb93515f159c110d4
```
and the returned `FileSource` is `"/" + filepath` (leading slash). Max Azure filepath length is 1024 chars
(`StorageConstant.AZURE_MAX_FILEPATH`), enforced in `StorageServiceImpl.createSignedUrl`.

---

## 3. `dataset--File.Generic` metadata schema

### 3.1 Where the schema lives

* Canonical file:
  `https://community.opengroup.org/osdu/data/data-definitions/-/raw/master/Generated/dataset/File.Generic.1.0.0.json`
  (verified 200, 146 lines) — `x-osdu-schema-source: osdu:wks:dataset--File.Generic:1.0.0`.
* Example record:
  `.../Examples/dataset/File.Generic.1.0.0.json` (verified 200).
* Current superseding version: `Generated/dataset/File.Generic.1.1.0.json` (verified 200) which composes
  `AbstractCommonResources.1.0.0` + `AbstractDataset.1.0.1` + `AbstractFile.1.0.1` + `ExtensionProperties`.
* Human-readable: `E-R/dataset/File.Generic.1.0.0.md` (verified 200), and the File Service's own
  `docs/docs/metadataPayload.json`.

### 3.2 Structure and mandatory fields

The record is a normal OSDU Storage record. `File.Generic.1.0.0` declares
`"required": ["kind","acl","legal"]` and `additionalProperties: false`; `data` is an `allOf` of
`AbstractCommonResources` + `AbstractDataset` + `AbstractFile` + `{ExtensionProperties}`.

Effective mandatory set for the File Service API (schema **plus** bean-validation in
`file-core/.../model/filemetadata/FileMetadata.java` and the composed abstracts):

| Field | Mandatory | Source of truth |
|---|---|---|
| `kind` | **yes** | `@NotNull` + `@ValidKind`; schema `required` |
| `acl.viewers`, `acl.owners` | **yes** | `@NotNull @ValidAcl`; `AclValidator` rejects empty arrays and non-`^data\.…@…$` group names |
| `legal.legaltags`, `legal.otherRelevantDataCountries` | **yes** | `@NotNull @Valid`; `Legal` (core-common) declares both `required`, `minItems: 1`, `uniqueItems: true` |
| `data` | **yes** | `@NotNull` |
| `data.DatasetProperties` | **yes** | `@NotNull @Valid`; `AbstractDataset`/`AbstractFile` `required: ["DatasetProperties"]` |
| `data.DatasetProperties.FileSourceInfo` | **yes** | `@NotNull @Valid` |
| `data.DatasetProperties.FileSourceInfo.FileSource` | **yes** | `@NotEmpty("FileSource can not be empty")`; `AbstractFileSourceInfo` `required: ["FileSource"]` |
| `data.Name`, `data.Description`, `data.TotalSize`, `data.EncodingFormatTypeID`, `data.SchemaFormatTypeID`, `data.Endian`, `data.Checksum`, `data.ExtensionProperties` | no | `AbstractDataset` / `AbstractFile` |
| `data.DatasetProperties.FileSourceInfo.{PreloadFilePath, PreloadFileCreateUser, PreloadFileCreateDate, PreloadFileModifyUser, PreloadFileModifyDate, Name, FileSize, EncodingFormatTypeID, Checksum, ChecksumAlgorithm}` | no | `AbstractFileSourceInfo.1.0.0` |
| `ancestry`, `meta`, `tags`, `id`, `version` | no | server sets `id`/`version` |
| `legal.status` | server-set (`readOnly`), enum `compliant`/`incompliant` | `Legal` |

`AclValidator` uses `ValidationDoc.EMAIL_REGEX`:
`^data\.[a-zA-Z0-9_+&*-]+(?:\.[a-zA-Z0-9_+&*-]+)*@(?:[a-zA-Z](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.)+[a-zA-Z](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?$`.
`KindValidator` uses `ValidationDoc.KIND_REGEX = ^[\w\-\.]+:[\w\-\.]+:[\w\-\.]+:[0-9]+.[0-9]+.[0-9]+$`.

### 3.3 `ExtensionProperties` / `FileContentsDetails` — where `fileType` lives

`data.ExtensionProperties` is `{"type":"object"}` in the schema (free-form), but the File Service and its docs
treat it as a container for a `FileContentsDetails` object. The Java model is
`file-core/.../model/filemetadata/filedetails/FileContentsDetails.java`:

```java
@Data @Builder @NoArgsConstructor @AllArgsConstructor
@Schema(description = "Details describing the contents and format of a file")
public class FileContentsDetails {
    private String kind;                                     // "Kind identifier for the file contents"
    @JsonProperty("TargetKind")     private String targetKind;
    @JsonProperty("FileType")       private String fileType; // e.g. SEG-Y, LAS, csv
    @Valid @JsonProperty("FrameOfReference") private List<MetaItem> frameOfReference = new ArrayList<>();
    @JsonProperty("ExtensionProperties") private Object extensionProperties;
    @JsonProperty("ParentReference")     private String parentReference;
}
```
`MetaItem` (frame of reference) = `{ kind: ForKind, name, persistableReference, propertyNames[], propertyValues[], uncertainty }`.
Related models present but not referenced from `FileData`: `Endian`, `ScalarIndicator`, `ValueWithUnit`,
`VectorHeaderMapping`, `Relationships`/`RelatedItems`/`ParentEntity`.

> **`artifact` does not exist.** I checked: (a) `Generated/dataset/File.Generic.1.0.0.json` at master;
> (b) the earliest committed `Generated/dataset/File.Generic.1.0.0.json` (commit `b34467666c`, 2021-01-31);
> (c) the 2020 `Generated/file/File.1.0.0.json` (`osdu:wks:file.File:1.0.0`, since deleted);
> (d) the deleted `docs/docs/file-service_openapi.yaml`. **No `artifact` property anywhere.** The only
> "artifact" string in the legacy spec is the typo `actifactId` in `VersionInfo`. If your upstream contract
> mentions `data.artifact` with `ResourceId`/`ResourceKind`/`ResourceVersion`, that is a **non-OSDU / vendor
> File-DMS legacy payload** — flag it back to the requester. **UNVERIFIED** as to its original source.
>
> Likewise `fileSource`/`fileType` in lowerCamelCase are **not** the OSDU property names. The OSDU names are
> **PascalCase**: `FileSource` and `FileType`.

### 3.4 Concrete valid metadata JSON

Taken from `docs/docs/metadataPayload.json` (repo-verified) with the schema-conformant `kind`/`id` from the
OSDU example:

```json
{
  "kind": "osdu:wks:dataset--File.Generic:1.0.0",
  "acl": {
    "viewers": ["data.default.viewers@opendes.contoso.com"],
    "owners":  ["data.default.owners@opendes.contoso.com"]
  },
  "legal": {
    "legaltags": ["opendes-public-abc-dataset-1"],
    "otherRelevantDataCountries": ["US"],
    "status": "compliant"
  },
  "data": {
    "Name": "File",
    "Description": "string",
    "TotalSize": "95463",
    "EncodingFormatTypeID": "namespace:reference-data--EncodingFormatType:text%2Fcsv:",
    "SchemaFormatTypeID": "namespace:reference-data--SchemaFormatType:CWLS%20LAS3:",
    "Endian": "BIG",
    "Checksum": "d41d8cd98f00b204e9800998ecf8427e",
    "DatasetProperties": {
      "FileSourceInfo": {
        "Name": "File",
        "PreloadFilePath": "s3://staging-area/r7/raw-data/provided/documents/1000.witsml",
        "FileSource": "/b40197d1-a0e8-49e3-96dd-gvubkjnonl/5vyug87gibun9hn0non",
        "PreloadFileCreateUser": "somebody@acme.org",
        "PreloadFileCreateDate": "2019-12-16T11:46:20.163Z",
        "PreloadFileModifyUser": "somebody.else@acme.org",
        "PreloadFileModifyDate": "2019-12-20T17:20:05.356Z",
        "FileSize": "95463",
        "EncodingFormatTypeID": "namespace:reference-data--EncodingFormatType:application%2Fgeo%2Bjson:",
        "Checksum": "d41d8cd98f00b204e9800998ecf8427e",
        "ChecksumAlgorithm": "MD5"
      }
    },
    "ExtensionProperties": {
      "Name": "File",
      "Classification": "Raw File",
      "Description": "A text further describing this file example.",
      "ExternalIds": ["string"],
      "FileDateCreated": {},
      "FileDateModified": {},
      "FileContentsDetails": {
        "TargetKind": "os:npd:wellbore:1:*.*",
        "FileType": "csv",
        "FrameOfReference": [
          {
            "kind": "CRS",
            "name": "[\"NAD27 * OGP-Usa Conus / North Dakota South [32021,15851]\",\"ft\"]",
            "persistableReference": "{\"scaleOffset\":{\"scale\":0.3048006096012192,\"offset\":0.0},\"symbol\":\"ftUS\",\"baseMeasurement\":{\"ancestry\":\"Length\",\"type\":\"UM\"},\"type\":\"USO\"}",
            "propertyNames": ["elevationFromMsl", "totalDepthMdDriller", "wellHeadProjected"],
            "propertyValues": ["F", "ftUS", "deg"],
            "uncertainty": 0
          }
        ],
        "ExtensionProperties": { "kind": "os:npd:csvFileExtDetails:1.0.0" },
        "ParentReference": "CSBE0417"
      },
      "relationships": {
        "parentEntity": { "confidence": 1, "id": "data_partition:namespace:entity_845934c40e8d922bc57b678990d55722", "name": "Survey ST2016", "version": 0 },
        "relatedItems": { "confidences": [0], "ids": ["string"], "names": ["string"], "versions": [0] }
      }
    }
  }
}
```

Notes:
* `FileSource` here is *relative* (leading `/`), not `s3://`/`gs://`. The File Service resolves it to an absolute
  staging/persistent URI itself. (`Examples/dataset/File.Generic.1.0.0.json` uses an `s3://` absolute value — that
  is a domain example, not what the File Service writes.)
* The File Service **overwrites** `FileSourceInfo.Checksum` and `ChecksumAlgorithm` with whatever the provider
  computes (`ChecksumAlgorithm.MD5` for Azure; `ChecksumAlgorithm.NONE` is the interface default).
* `id` is **ignored on POST for v2 `/files/metadata`** — the service generates
  `"<partition>:dataset--File.Generic:<uuid>"` itself. (`id` is only meaningful for `getLocation`, which accepts an
  optional `FileID`.)

---

## 4. Storage-backend abstraction in the reference implementations

### 4.1 The core SPI (all in `file-core/src/main/java/org/opengroup/osdu/file/provider/interfaces/`)

```java
public interface IStorageService {                                   // signing + DMS locations
  SignedUrl createSignedUrl(String fileID, String authorizationToken, String partitionID);
  default SignedUrl createSignedUrl(String fileID, String authorizationToken, String partitionID,
                                    SignedUrlParameters signedUrlParameters) { ... }
  default StorageInstructionsResponse createStorageInstructions(String datasetId, String partitionID) { return null; }
  default StorageInstructionsResponse createStorageInstructions(String datasetId, String partitionID,
                                                                SignedUrlParameters p) { ... }
  default SignedUrl createSignedUrlFileLocation(String unsignedUrl, String authorizationToken,
                                                SignedUrlParameters signedUrlParameters) { return null; }
  default RetrievalInstructionsResponse createRetrievalInstructions(List<FileRetrievalData> fileRetrievalData) { return null; }
  default RetrievalInstructionsResponse createRetrievalInstructions(List<FileRetrievalData> d, SignedUrlParameters p) { ... }
  default Boolean revokeUrl(Map<String,String> revokeURLRequest) { return false; }
}

public interface IStorageRepository {                                // blob creation + signing primitives
  SignedObject createSignedObject(String bucketName, String filepath);
  default SignedObject createSignedObjectBasedOnParams(String bucketName, String filepath, SignedUrlParameters p) { ... }
  default SignedObject getSignedObject(String bucketName, String filepath){ return null; }
  default SignedObject getSignedObjectBasedOnParams(String bucketName, String filepath, SignedUrlParameters p) { ... }
  default Boolean revokeUserDelegationKeys(Map<String,String> revokeURLRequest){ return false; }
}

public interface IStorageUtilService {                               // zone resolution + checksum
  default String getPersistentLocation(String relativePath, String partitionId) { return null; }
  default String getStagingLocation(String relativePath, String partitionId)    { return null; }
  default String getChecksum(final String filePath) { return null; }
  default ChecksumAlgorithm getChecksumAlgorithm() { return ChecksumAlgorithm.NONE; }
}

public interface ICloudStorageOperation {                            // server-side copy/delete
  default String copyFile(String sourceFilePath, String destinationFilePath) throws OsduBadRequestException { return null; }
  default List<FileCopyOperationResponse> copyFiles(List<FileCopyOperation> fileCopyOperationList) { return null; }
  default Boolean deleteFile(String filePath) { return false; }
  default List<DatasetCopyOperation> copyDirectories(List<FileCopyOperation> fileCopyOperationList) { return null; }
}

public interface IFileLocationRepository {                           // own metadata store
  FileLocation findByFileID(String fileID);
  FileLocation save(FileLocation fileLocation);
  FileListResponse findAll(FileListRequest request);
}

public interface ILocationMapper { LocationResponse buildLocationResponse(SignedUrl signedUrl, FileLocation fileLocation); }
public interface IValidationService { void validateLocationRequest(LocationRequest r);
                                      void validateFileLocationRequest(FileLocationRequest r);
                                      void validateFileListRequest(FileListRequest r); }
public interface IAuthenticationService { void checkAuthentication(String authorizationToken, String partitionID); }
public interface IFileListService { FileListResponse getFileList(FileListRequest request, DpsHeaders headers); }
public interface IFileCollectionStorageService { ... }   // ADLS/collection variants
public interface IFileCollectionStorageUtilService { ... }
public interface IDeliveryStorageService { SignedUrl createSignedUrl(String unsignedUrl, String authorizationToken);
                                           default SignedUrl createSignedUrl(String srn, String unsignedUrl, String authorizationToken) { ... } }
public interface IDeliveryUnsignedUrlLocationMapper { String getUnsignedURLFromSearchResponse(Map<String,Object> response); }
```
Domain carriers: `SignedObject { URL url; URI uri; }`, `SignedUrl { URI uri; URL url; String fileSource; String createdBy;
Instant createdAt; String connectionString; }`, `FileRetrievalData { String recordId; String unsignedUrl; }`,
`SignedUrlParameters { String expiryTime; String fileName; String contentType; }`,
`FileCopyOperation`/`FileCopyOperationResponse`, `DatasetCopyOperation`.

Provider selection is **Spring component scan + `@Primary` + `@Qualifier`**, not a factory-per-cloud lookup. There is
**no `IBlobStore`, no `BlobStoreFactory`, no `StorageAccount` class in the File Service repo.** Those names belong to
the SDK libraries the providers consume (see Azure below).

### 4.2 Azure (`provider/file-azure`) — `https://github.com/azure/osdu-file-azure`

Blob abstraction: a **local** `Storage` + `StorageImpl`, plus the shared **`BlobStore`** from `os-core-lib-azure`.

```java
// provider/file-azure/.../azure/storage/Storage.java
public interface Storage {
    Blob create(String dataPartitionId, BlobInfo blobInfo, byte[] content);
    URL  signUrl(BlobInfo blobInfo, long duration, TimeUnit unit);
}
// provider/file-azure/.../azure/storage/StorageImpl.java   (@Service)
//   - BlobContainerClientFactory.getClient(partition, container); creates container if absent
//   - BlockBlobClient.upload(ByteArrayInputStream(EMPTY_BYTE_ARRAY), 0) -- creates a 0-byte blob
//   - signUrl -> AzureTokenServiceImpl.sign(blobURL, duration, unit)
```
Azure model classes: `Blob`, `BlobId` (`BlobId.of(container, name)`), `BlobInfo` (`BlobInfo.newBuilder(blobId)
.setContentType(...).build()`, `BuilderImpl`, `getContainer()`, `getName()`, `getBlobId()`, `getGeneratedId()`).

**`org.opengroup.osdu.azure.blobstorage.BlobStore`** — a concrete class (not an interface) in
`os-core-lib-azure` (`osdu/platform/system/lib/cloud/azure/os-core-lib-azure`, project 77). Public methods verified
from source:

```java
public BlobStore(IBlobServiceClientFactory factory, ILogger loggerInstance, DependencyLogger depLogger)
public String readFromStorageContainer(String dataPartitionId, String filePath, String containerName)
public boolean deleteFromStorageContainer(String dataPartitionId, String filePath, String containerName)
public boolean undeleteFromStorageContainer(String dataPartitionId, String filePath, String containerName)
public void    writeToStorageContainer(String dataPartitionId, String filePath, String content, String containerName)
public boolean createBlobContainer(String dataPartitionId, String containerName)
public boolean checkIfBlobContainerExists(String dataPartitionId, String containerName)
public boolean deleteBlobContainer(String dataPartitionId, String containerName)
public String  getSasToken(String dataPartitionId, String filePath, String containerName,
                           OffsetDateTime expiryTime, BlobSasPermission permissions)
public String  generatePreSignedURL(String dataPartitionId, String filePath, String containerName,
                                    OffsetDateTime expiryTime, BlobSasPermission permissions)
public String  generatePreSignedURL(String dataPartitionId, String filePath, String containerName,
                                    OffsetDateTime expiryTime, BlobSasPermission permissions,
                                    String fileName, String contentType)
public String  generatePreSignedURL(String dataPartitionId, String containerName,
                                    OffsetDateTime expiryTime, BlobContainerSasPermission permissions)
public String  generatePreSignedUrlWithUserDelegationSas(String dataPartitionId, String containerName,
                                    OffsetDateTime startTime, OffsetDateTime expiryTime,
                                    BlobContainerSasPermission permissions)
public String  generatePreSignedUrlWithUserDelegationSas(String dataPartitionId, String containerName,
                                    String filePath, OffsetDateTime expiryTime, BlobSasPermission permissions)
public BlobCopyInfo  copyFile(String dataPartitionId, String filePath, String containerName, String sourceUrl)
public BlobProperties readBlobProperties(String dataPartitionId, String filePath, String containerName)
public BlobInputStream getBlobInputStream(String dataPartitionId, String filePath, String containerName)
```
Azure provider classes and their roles:

| Class | Role |
|---|---|
| `config/BlobStoreConfig` | `@Value("${azure.storage.persistent-area}") String persistentContainer`, `@Value("${azure.storage.staging-area}") String stagingContainer` |
| `config/BlobServiceClientWrapper` | `@RequestScope`; resolves `storageAccountURL` via `IBlobServiceClientFactory.getBlobServiceClient(dataPartitionId).getAccountUrl()` |
| `config/DataLakeConfig`, `DataLakeClientWrapper`, `AzureBootstrapConfig`, `CosmosContainerConfig`, `EventGridConfig`, `ServiceBusConfig`, `PublisherConfig`, `PropertiesConfiguration` | per-partition clients / eventing |
| `repository/StorageRepository` (`implements IStorageRepository`) | creates the empty blob then `blobStore.generatePreSignedURL(...)` / `generatePreSignedUrlWithUserDelegationSas(...)`; `revokeUserDelegationKeys` via `StorageAccountsClient.revokeUserDelegationKeysWithResponse` |
| `repository/DataLakeRepository` | ADLS Gen2 variant (`@Qualifier("DataLake")`) |
| `repository/FileLocationRepository` + `FileLocationEntityRepository` | Cosmos DB |
| `service/StorageServiceImpl` (`implements IStorageService`, `@Primary`) | `PROVIDER_KEY = "AZURE"`; creates `osdu-user/<millis>-<ts>/<fileID>`; enforces 1024-char path; `createSignedUrlFileLocation` sets `Content-Disposition`/`Content-Type` overrides; `revokeUrl` validates `resourceGroup`/`storageAccount` |
| `service/StorageUtilServiceImpl` (`implements IStorageUtilService`, `@Primary`) | `getPersistentLocation`/`getStagingLocation` via `String.format("%s/%s/%s", storageAccountURL, container, path)`; `getChecksum` = blob MD5 or stream-computed; `getChecksumAlgorithm() = ChecksumAlgorithm.MD5`; `CHECKSUM_CALCULATION_LIMIT` guard |
| `service/CloudStorageOperationImpl` (`implements ICloudStorageOperation`) | `copyFile` → `blobStore.copyFile(partition, filePath, container, sourceUrl)`; `deleteFile` → `blobStore.deleteFromStorageContainer`; `copyDirectories` → `DataLakeStore.moveDirectory` (ADLS has no directory copy) |
| `service/FileCollectionStorageServiceImpl` / `FileCollectionUtilServiceImpl` | ADLS Gen2 file-collection DMS |
| `service/AzureBlobSasTokenServiceImpl`, `AzureTokenServiceImpl`, `ExpirationDateHelper`, `InstantHelper`, `ServiceHelper`, `FilePathUtil` | SAS/token helpers; `ServiceHelper` regex-splits absolute paths into container / file path / file system |
| `mapper/AzureLocationMapper` (`implements ILocationMapper`) | builds `LocationResponse` with `SignedURL` + `FileSource` |
| `mapper/FileLocationMapper` (MapStruct) | `FileLocationEntity` ↔ `FileLocation`; `DriverType.valueOf(entity.getDriver())` |
| `security/AADSecurityConfig`, `AzureIstioSecurityConfig` | AAD / Istio auth |
| `status/StatusEventPublisher` | Event Grid / Service Bus status publishing |
| `storage/Storage`, `storage/StorageImpl` | the small local blob-create/sign façade shown above |

Azure persistence: **Cosmos DB**, via `org.opengroup.osdu.azure.cosmosdb.CosmosStore`
(`findItem`, `upsertItem`, `queryItems`), container `file-locations`, partition key = `id`, Spring Data Cosmos
`@Container(containerName = "file-locations")`. Config: `azure.cosmosdb.database=${cosmosdb_database}`,
`filelocation.container.name=FileLocationEntity`.

### 4.3 GCP (`file-core-plus` + `provider/file-gc`) — OBM/OSM abstraction

The real GCP implementation lives in **`file-core-plus`** (`org.opengroup.osdu.file.provider.gcp.*`). The
`provider/file-gc` module in master is essentially a stub application (`FileGcpDatastoreApplication.java` + resources).
It layers on the shared OSDU **OBM** (`os-obm-s3`, `org.opengroup.osdu.core.obm.core`) and **OSM**
(`os-osm-postgres`) libraries, which are fetched as Maven plugins/artifacts in CI (`.gitlab-ci.yml`
`download_plugins` job, `OSM_VERSION 0.27.3`, `OBM_VERSION 0.31.0`).

| Class | Role |
|---|---|
| `provider/gcp/provider/repository/ObmStorageRepository` (`@Component("ObmStorageRepository")`, `implements IStorageRepository`) | wraps `org.opengroup.osdu.core.obm.core.Driver`; `obmDriver.getSignedUrlWithParams(ObmSignedUrlParams)`; builds unsigned URI with `ObmStorageUrlBuilder.buildUnsignedUrl(protocol, bucket, path)` |
| `provider/gcp/provider/service/ObmStorageService` (`implements IStorageService`) | `createSignedUrl` resolves staging bucket from `PartitionPropertyResolver` (`file.staging.location`) with fallback `defaultStagingBucketName(partitionId)`; `providerKey = environmentResolver.getProviderKey()` |
| `provider/gcp/provider/service/ObmStorageUtilServiceImpl` (`IStorageUtilService`) | persistent/staging locations from partition properties |
| `provider/gcp/provider/service/ObmCloudStorageOperationImpl` (`ICloudStorageOperation`) | `obmDriver.getBlob / copyBlob / copyBlobs / listBlobsByPrefix / deleteBlob`; `ObmDestination.builder().partitionId(...)` |
| `provider/gcp/provider/service/ObmCollectionStorageService`, `ObmCollectionStorageUtilService`, `ObmDeliveryStorageServiceImpl` | collections + delivery |
| `provider/gcp/provider/repository/ObmCollectionStorageRepository` | collection blobs |
| `provider/gcp/provider/repository/OsmFileLocationRepository` (`implements IFileLocationRepository`) | persists `FileLocationOsm` via `org.opengroup.osdu.core.osm.core.service.Context` (`createAndGet`, `getResultsAsList`), `Destination{partitionId, namespace, kind}`, `Kind = gcp.file-location-kind` (default `file-locations-osm`) |
| `provider/gcp/provider/mapper/LocationMapper` (`implements ILocationMapper`) | `SignedURL` + `FileSource` |
| `provider/gcp/provider/util/ObmStorageUrlBuilder` | `static String buildUnsignedUrl(String transferProtocol, String bucketName, String filePath)` |
| `provider/gcp/validation/FileLocationRequestValidator` | `GCS_MAX_FILEPATH` limit |
| `provider/gcp/security/SecurityConfig`, `config/CacheConfig`, `config/PropertiesConfiguration`, `config/CorePlusConfigurationProperties` | security + `@ConfigurationProperties(prefix="gcp")` (`fileLocationKind`, `partitionInfoVmCache*`, `signedUrl.expirationDays=1`) |

### 4.4 IBM COS (`provider/file-ibm`)

| Class | Role |
|---|---|
| `service/IBMStorageServiceImpl` (`implements IStorageService`) | IBM Cloud Object Storage via `org.opengroup.osdu.core.ibm.objectstorage.CloudObjectStorageFactory` → `AmazonS3`-compatible client (`com.ibm.cloud.objectstorage.services.s3.AmazonS3`); `GeneratePresignedUrlRequest` + `ResponseHeaderOverrides`; config `ibm.cos.endpoint_url`, `ibm.cos.access_key`, `ibm.cos.secret_key`, `ibm.cos.region`, `ibm.cos.signed-url.expiration-days` (default 7, master config 1) |
| `service/IBMCloudStorageOperationImpl` (`implements ICloudStorageOperation`) | S3 copy/delete |
| `service/IBMStorageUtilServiceImpl`, `FileCollectionStorageServiceImpl`, `FileCollectionStorageUtilServiceImpl`, `IBMDeliveryStorageServiceImpl` | zones, collections, delivery |
| `repository/IBMFileRepositoryImpl` (`implements IFileLocationRepository`) | **IBM Cloudant / CouchDB** via `IBMCloudantClientFactory`, `com.cloudant.client.api.Database`; `db.find(FileLocationDoc.class, fileID)`, `db.save(doc)`, Cloudant JSON query index `find-json-index` on `createdDate, createdBy`; `ibm.schemaName=file-locations` |
| `model/file/FileLocationDoc extends FileLocation` | `_id`/`_rev`/`createdDate` Cloudant document |
| `model/file/S3Location` | parses `s3://bucket/key` (`UNSIGNED_URL_PREFIX = "s3://"`) |
| `model/file/TemporaryCredentials implements AWSSessionCredentials` | `{accessKeyId, secretAccessKey, sessionToken, expiration}` + `toConnectionString()` |
| `service/STSHelper`, `service/InstantHelper`, `service/ExpirationDateHelper` | temporary credentials for direct client access |
| `mapper/IBMLocationMapper` (`implements ILocationMapper`) | `SignedURL` + `FileSource` |
| `security/IBMSecurityConfig`, `security/WhoamiController` | auth |
| `validation/IBMFileLocationRequestValidator` | request validation |

### 4.5 AWS S3 (`provider/file-aws`) — **not in master**

Present only in AWS-specific release tags, e.g. `v0.28.0-aws.1`. Verified tree:
`provider/file-aws/src/main/java/org/opengroup/osdu/file/provider/aws/…`

| Class | Role |
|---|---|
| `impl/StorageServiceImpl` (`implements IStorageService`, `@Primary @RequestScope`) | delegates to `FileLocationProvider`; builds `FileDmsStorageLocation` incl. `connectionString` + STS `credentials`; `createSignedUrlFileLocation` uses `ResponseHeaderOverrides` for filename/Content-Type |
| `impl/StorageUtilServiceImpl`, `impl/CloudStorageOperationImpl`, `impl/FileCollectionStorageServiceImpl`, `impl/FileCollectionStorageUtilServiceImpl` | zones, copy, collections |
| `impl/FileLocationRepositoryImpl` (`implements IFileLocationRepository`) | **DynamoDB** via `org.opengroup.osdu.core.aws.dynamodb.DynamoDBQueryHelperFactory` / `DynamoDBQueryHelperV2`; filter expression `dataPartitionId = :partitionId AND createdAt BETWEEN :startDate and :endDate AND createdBy = :user` |
| `impl/LocationMapperImpl`, `impl/delivery/DeliveryStorageServiceImpl`, `impl/status/StatusEventPublisherImpl` | mapping, delivery, SNS/EventBridge status |
| `model/S3Location` | `s3://bucket/key`, `of(bucket,key)`, `of(uri)`, `isFolder()/isFile()`, `S3LocationBuilder.withBucket/withFolder/withFile` |
| `helper/S3Helper`, `S3ConnectionInfoHelper`, `StsCredentialsHelper`, `StsRoleHelper`, `ExpirationDateHelper` | signing + STS assume-role |
| `auth/TemporaryCredentials`, `auth/TemporaryCredentialsProvider`, `cache/S3ConnectionInfoCache`, `cache/StsIamRoleCache`, `config/ProviderConfigurationBag` | credentials caching |
| `service/FileLocationProvider` / `service/impl/FileLocationProviderImpl` | provider-location resolution |

### 4.6 Local filesystem / `FileSystem` driver

**Not present.** I found no `FileSystemStorageService`, no local-disk driver, and no `file-local` provider in the
repo at master or any tag. The only non-cloud storage in CI is a **SeaweedFS/S3** deployment for the bare-metal
"core-plus" chart (`devops/core-plus/deploy/README.md`: *"conf.s3SecretName … secret for SeaweedFS/S3 file storage"*),
which is consumed through the same OBM/S3 abstraction. **UNVERIFIED** whether an out-of-tree local driver exists.

### 4.7 `DriverType` / driver naming summary

| Layer | Name | Values found |
|---|---|---|
| API field | `FileLocation.Driver` | enum serialised |
| core-common enum | `org.opengroup.osdu.core.common.model.file.DriverType` | **`GCS` only** |
| GCP OBM | `org.opengroup.osdu.core.obm.core.Driver` (bean) | library-internal, **UNVERIFIED** |
| Azure | `PROVIDER_KEY = "AZURE"` (in `StorageServiceImpl` / `FileCollectionStorageServiceImpl`) | used in DMS `providerKey`, **not** in `Driver` |
| AWS | `FileLocationProvider.getProviderKey()` | **UNVERIFIED** exact literal |

---

## 5. Metadata persistence, indexing, and inter-service relationships

### 5.1 What the File Service itself stores

The File Service is **not** a metadata store for file records. It has two persistence roles:

1. **File-location rows** (only used by the deprecated `getLocation`/`getFileLocation`/`getFileList` flow):
   `FileLocation { FileID, Driver, Location, CreatedAt, CreatedBy }` in the provider's own KV/document store —
   **Cosmos DB** (Azure), **IBM Cloudant** (IBM), **OSM/Datastore** (GCP), **DynamoDB** (AWS).
2. **File metadata records** are written to the **Storage service**, never locally:
   `DataLakeStorageService.upsertRecord` → `PUT {storage.api}/records`;
   `getRecord` → `GET {storage.api}/records/{id}`; `deleteRecord` → `POST {storage.api}/records/{id}:delete`;
   `getRecords` → `POST {storage.api}/query/records`.
   Config: `storage.api` / `RECORDS_ROOT_URL` (= `http://storage/api/storage/v2/` in GCP defaults; Azure
   `RECORDS_ROOT_URL=${osdu_storage_url}`), plus `AppKey` header from `authorize.api.key`.

   `DataLakeStorageService` methods (verified in `file-core/.../service/storage/DataLakeStorageService.java`):
   ```java
   public UpsertRecords upsertRecord(Record recordToUpdate) throws StorageException   // PUT /records (array body)
   public UpsertRecords upsertRecord(Record[] records) throws StorageException
   public Record getRecord(String id) throws StorageException                          // GET /records/{id}; 404 -> null
   public HttpResponse deleteRecord(String id)                                         // POST /records/{id}:delete
   public MultiRecordInfo getRecords(Collection<String> ids) throws StorageException   // POST /query/records
   ```

**Elasticsearch / Postgres: absent from the File Service.** `grep -rli "elasticsearch\|postgres"` over
`provider/`, `file-core/`, `file-core-plus/` returns no File Service source hits. Indexing for search is done by the
**Storage service** (which writes to Elasticsearch) — the File Service only publishes to `os-core-common`/Google
Pub/Sub event publishers. The `docs/docs/File-Service.md` `/info` example showing `connectedOuterServices:
[elasticSearch, postgresSql, redis]` is a **copy-paste of the Storage service's example**, not the File Service's
own dependencies (verified: no `connectedOuterServices` configuration exists anywhere in the File Service repo).

### 5.2 Service dependency map (from config + imports)

| Service | How used | Evidence |
|---|---|---|
| **Storage** | `PUT/GET/POST /records` to persist, read, delete the `dataset--File.Generic` record; source of the metadata returned by `GET /v2/files/{id}/metadata` and `downloadURL` | `DataLakeStorageService`, `DatalakeStorageClientFactory` (`storage.api`), `RECORDS_ROOT_URL` |
| **Entitlements** | `authorizeAny(headers, requiredRoles)` on every guarded endpoint; also group resolution for the bearer token | `middleware/AuthorizationFilter`, `di/EntitlementsClientFactory` (`osdu.entitlements.url`, `osdu.entitlements.app-key`) |
| **Legal** | *Not called directly.* Legal-tag validation is delegated to the Storage service when the record is upserted. `Legal` object is only structurally validated (`@Valid`, non-empty legaltags/ORDC). | `FileMetadataService.saveMetadata` → Storage `PUT /records` |
| **Schema** | *Not called directly.* Kind validation is a hardcoded string check (`wks` + `dataset--File.Generic`), not a Schema-service lookup. | `FileMetadataService.validateKind` |
| **Partition** | `DpsHeaders.getPartitionIdWithFallbackToAccountId()`; GCP resolves staging/persistent bucket names per partition via `PartitionPropertyResolver` (`PARTITION_API`) | `LocationServiceImpl`, GCP `ObmStorageService` |
| **Search** | Only for the Delivery API (`DeliverySearchServiceImpl` uses `SEARCH_QUERY_RECORD_HOST`, default `…/api/search/v2/query`) | delivery config properties |
| **Pub/Sub / Event Grid / Service Bus** | status + dataset-details events: `FileStatusPublisher` (kind `status`, stage `DATASET_SYNC`, states `IN_PROGRESS`/`SUCCESS`/`FAILED`), `FileDatasetDetailsPublisher` (kind `datasetDetails`, `DatasetType.FILE`, `recordCount=1`) | `file-core/.../service/status/*`; Azure `azure.eventGrid.*`/`azure.serviceBus.*`; GCP `gcp.status.changed.topic-name` |
| **Dataset service** | No direct HTTP calls. The DMS endpoints (`/v2/files/storageInstructions`, `/retrievalInstructions`, `/copy`) are the *File service's* implementation of the Dataset/DMS contract; role names come from `DatasetConstants`. | `FileDmsApi`, `FileCollectionDmsApi` |

`Record` payload sent to Storage (`file-core/.../model/storage/Record.java`):
```java
class Record { String id; Long version; String kind; Acl acl; Legal legal; Ancestry ancestry;
               Map<String,Object> data; List<Map<String,Object>> meta; Map<String,String> tags; }
```
`FileMetadataRecordMapper.fileMetadataToRecord` maps `FileMetadata.data` (typed `FileData`) to a
`HashMap<String,Object>` via Jackson, preserving PascalCase property names, and `UpsertRecords` returns
`recordIds[]` + `recordIdVersions[]`.

---

## 6. Auth model

### 6.1 Headers

`org.opengroup.osdu.core.common.model.http.DpsHeaders` constants (verified):

```java
ACCOUNT_ID            = "account-id"
ON_BEHALF_OF          = "on-behalf-of"
CORRELATION_ID        = "correlation-id"
DATA_PARTITION_ID     = "data-partition-id"
USER_EMAIL            = "user"
USER_AUTHORIZED_GROUP_NAME = "user-authorized-group-name"
AUTHORIZATION         = "authorization"
CONTENT_TYPE          = "content-type"
LEGAL_TAGS            = "legal-tags"
ACL_HEADER            = "acl"
KIND_VERSION          = "kind_version"
PRIMARY_PARTITION_ID  = "primary-account-id"
FRAME_OF_REFERENCE    = "frame-of-reference"
USER_ID               = "x-user-id"
APP_ID                = "x-app-id"
X_ON_BEHALF_OF        = "x-on-behalf-of"
COLLABORATION         = "x-collaboration"
```

* `data-partition-id` is **required** on every documented endpoint (declared in the generated OpenAPI via
  `SwaggerConfiguration.operationCustomizer()`, which injects a required `data-partition-id` header parameter into
  every operation **except `revokeURL`**).
* `Authorization: Bearer <token>`; the OpenAPI security scheme is
  `type: http, scheme: bearer, bearerFormat: Authorization` (`SwaggerConfiguration.customOpenAPI`).
* Fallback: `headers.getPartitionIdWithFallbackToAccountId()` is used to pick the storage partition, so an
  `account-id` header can substitute for a missing `data-partition-id` in that specific path.

### 6.2 Enforcement

`file-core/src/main/java/org/opengroup/osdu/file/middleware/AuthorizationFilter.java` is registered as the bean
`"authorizationFilter"` and is what every `@PreAuthorize` calls:

```java
public boolean hasPermission(String... requiredRoles) {
    authenticationService.checkAuthentication(headers.getAuthorization(), headers.getPartitionId());
    AuthorizationResponse authResponse = authorizationService.authorizeAny(headers, requiredRoles);
    headers.put(DpsHeaders.USER_EMAIL, authResponse.getUser());
    headers.put(DpsHeaders.USER_AUTHORIZED_GROUP_NAME, authResponse.getUserAuthorizedGroupName());
    return true;
}
```

* `authorizationService` is `org.opengroup.osdu.core.common.provider.interfaces.IAuthorizationService`, i.e. the
  **Entitlements** service client.
* `AuthenticationServiceImpl.checkAuthentication` is deliberately minimal (the file itself carries
  `// TODO: add check of user permissions`):
  ```java
  if (StringUtils.isBlank(authorizationToken)) throw new OsduUnauthorizedException("Missing authorization token");
  if (StringUtils.isBlank(partitionID))        throw new OsduUnauthorizedException("Missing partitionID");
  ```
  → these are the two `401` paths asserted by the acceptance tests. Real role checks happen in Entitlements
  (`authorizeAny`) → `403` when the token is valid but no required role is held.
* `OsduUnauthorizedException` → `401` via `handleAccessDeniedException`. The acceptance tests accept **401 or 403**
  for invalid token/partition (see `IntegrationTest_File_Get_Location_FileLocation_FileList.feature`).

### 6.3 Roles

`file-core/src/main/java/org/opengroup/osdu/file/constant/FileServiceRole.java`:
```java
public static final String VIEWERS = "service.file.viewers";
public static final String EDITORS = "service.file.editors";
public static final String ADMIN   = "service.file.admin";
```
`constant/DeliveryRole.java`: `VIEWER = "service.delivery.viewer"`.

Roles referenced from `os-core-common`:
* `DatasetConstants.DATASET_VIEWER_ROLE = "service.dataset.viewers"`,
  `DATASET_EDITOR_ROLE = "service.dataset.editors"`, `DATASET_ADMIN_ROLE = "service.dataset.admin"`.
* `StorageRole.VIEWER = "service.storage.viewer"`, `CREATOR = "service.storage.creator"`,
  `ADMIN = "service.storage.admin"`, `PUBSUB = "storage.pubsub"`, plus `ROLE_`-prefixed variants.

Role → endpoint matrix is in table §1.1.

Default group wiring: the docs state that users in `users.datalake.viewers` / `users.datalake.editors` /
`users.datalake.admins` / `users.datalake.ops` are added to `service.file.viewers` / `service.file.editors`
**by default**. That mapping is provisioned by the Entitlements/partition bootstrap, **not** by code in this repo
(verified: no bootstrap code present here). **UNVERIFIED** as to where exactly it is defined.

### 6.4 ACL / legal-tag validation on file records

* **At the File Service boundary (verified):** `@ValidAcl` (non-empty `viewers` and `owners`, each matching
  `^data\..*@.*$`) and `@Valid`/`@NotNull` on `Legal` (non-empty `legaltags` and `otherRelevantDataCountries`).
  Bean validation runs in two groups, in order: `FileMetadataValidationSequence = @GroupSequence({Default.class,
  BusinessRuleValidation.class})`; `@ValidAcl`/`@ValidKind` are bound to the `BusinessRuleValidation` group.
* **Real enforcement (verified by call path):** the record is handed to the Storage service, which validates legal
  tags against the Legal service and ACL groups against Entitlements, and returns the error, which the File Service
  re-surfaces unchanged via `handleStorageException` (`"Storage record error"`, with the inner HTTP body in
  `errors[].errorProperties`).
* The `Legal` model itself is `os-core-common`'s `org.opengroup.osdu.core.common.model.legal.Legal`
  (`legaltags`, `otherRelevantDataCountries`, `status`).

### 6.5 Other auth surfaces

* Azure: `AADSecurityConfig` / `AzureIstioSecurityConfig`, `azure.istio.auth.enabled`, AAD app resource id.
* IBM: `spring.security.oauth2.resourceserver.jwt.jwk-set-uri`, `IBMSecurityConfig`.
* GCP: `provider/gcp/security/SecurityConfig`.
* Deployment-level authz bypass lists (Istio `AuthorizationPolicy`) show which paths are public — e.g. Azure allows
  `/api/file/swagger-resources/*`, `/api/file/webjars/*`, `/api/file/v2/info` unauthenticated.

---

## 7. OpenAPI / Swagger: generation and exact spec locations

### 7.1 There is no code generation

* The root `pom.xml` and `file-core/pom.xml` contain **no** `openapi-generator-maven-plugin`,
  **no** `swagger-codegen-maven-plugin`, and **no** generated-sources step. Grep over all `pom.xml` files for
  `openapi|swagger|springdoc|codegen` yields only:
  * `org.springdoc:springdoc-openapi-starter-webmvc-ui` (≥ `2.8.16`, property `<openapi.version>2.8.16</openapi.version>`),
  * `io.swagger.core.v3:swagger-annotations-jakarta` / `swagger-core-jakarta` / `swagger-models-jakarta` `2.2.45`.
* The spec is **generated at runtime from annotations** by springdoc. `file-core/src/main/resources/swagger.properties`:
  ```properties
  springdoc.swagger-ui.path=/v2/swagger
  springdoc.api-docs.path=/v2/api-docs
  springdoc.api-docs.version=openapi_3_1
  springdoc.default-produces-media-type=application/json
  springdoc.default-consumes-media-type=application/json
  swagger.apiTitle=File Service
  swagger.apiVersion=2.0.0
  swagger.apiServerUrl=${server.servlet.contextPath:/api/file/}
  swagger.apiServerFullUrlEnabled=${api.server.fullUrl.enabled:false}
  ```
  The `OpenAPI` bean and the `data-partition-id` operation customizer are in
  `file-core/src/main/java/org/opengroup/osdu/file/swagger/SwaggerConfiguration.java` (`@Profile("!noswagger")`).
  Server URL is `/api/file/` unless `api.server.fullUrl.enabled=true`.

**Runtime spec routes (verified in `.gitlab-ci.yml`):**
```
CIMPL_OPENAPI_CONFIG_ROUTE:    /api/file/v2/api-docs/swagger-config
SCHEMATHESIS_OPENAPI_YAML_ROUTE: /api/file/v2/api-docs.yaml
AZURE_SWAGGER_PATH:            api/file/v2/swagger-ui/index.html
```

### 7.2 The committed spec file

There is exactly **one** OpenAPI file in the repo:

```
docs/api/community/v2/openapi.yaml
raw: https://community.opengroup.org/osdu/platform/system/file/-/raw/master/docs/api/community/v2/openapi.yaml
```
(verified HTTP 200; 938 lines; `openapi: 3.1.0`; `info.title: File Service`; `info.version: 2.0.0`;
`servers[0].url: /api/file/`; `security: [{Authorization: []}]`).

Its `paths` block contains exactly: `/v2/files/revokeURL`, `/v2/files/metadata`, `/v2/readiness_check`,
`/v2/liveness_check`, `/v2/info`, `/v2/files/{id}/metadata` (get + delete), `/v2/files/{id}/downloadURL`,
`/v2/files/uploadURL`. Component schemas: `AppError`, `Acl`, `Ancestry`, `DatasetProperties`, `FileData`,
`FileMetadata`, `FileSourceInfo`, `Legal`, `FileMetadataResponse`, `ConnectedOuterService`, `FeatureFlagState`,
`VersionInfo`, `RecordVersion`, `DownloadUrlResponse`, `LocationResponse`.

> **Important caveat for a reimplementation:** this committed spec is **incomplete relative to the code**. It omits
> every `@Hidden` endpoint (`/v2/getLocation`, `/v2/getFileLocation`, `/v2/getFileList`, `/v2/files/storageInstructions`,
> `/v2/files/retrievalInstructions`, `/v2/files/copy`, the `/v2/file-collections/*` trio, `/v2/delivery/GetFileSignedUrl`)
> and also omits the generated `data-partition-id` header on some operations. Do **not** treat it as the wire contract;
> treat the controllers + the runtime `/api/file/v2/api-docs.yaml` as authoritative.

History (from the GitLab commits API for that path — 2 commits):
* `c069d6f87f` 2026-03-24 *"OpenAPI spec check CI job"* — **added** `docs/api/community/v2/openapi.yaml`, **deleted**
  `docs/docs/file-service_openapi.yaml`, and added CI variables `CIMPL_OPENAPI_SPEC_PATH` /
  `CIMPL_OPENAPI_YAML_ROUTE`.
* `273442b095` 2026-06-19 *"Migrate to os-core-common 7.1.0"* — updated it.
The repo's current `.gitlab-ci.yml` retains the later `SCHEMATHESIS_OPENAPI_YAML_ROUTE` (added by the commit
`d7c25c2` = current HEAD) and no longer sets `CIMPL_OPENAPI_SPEC_PATH`; the spec-check wiring lives in the external
project `osdu/platform/ci-cd-pipelines` (`reporting/schemathesis.yml`, `cloud-providers/cimpl-global.yml`), which I
did **not** fetch.

`docs/mkdocs.yml` does **not** reference `docs/api/community/v2/openapi.yaml`; the docs site
(`https://osdu.pages.opengroup.org/platform/system/file/`) publishes only `index.md`, `File-Service.md` and Allure
reports. So the yaml's only consumer is CI.

### 7.3 Raw URLs verified (all HTTP 200)

```
https://community.opengroup.org/api/v4/projects/osdu%2Fplatform%2Fsystem%2Ffile                     (project id 90)
https://community.opengroup.org/api/v4/projects/90/repository/tags?per_page=100
https://community.opengroup.org/api/v4/projects/90/repository/tree?recursive=true&per_page=100&page=N   (N=1..11)
https://community.opengroup.org/osdu/platform/system/file/-/raw/master/docs/api/community/v2/openapi.yaml
https://community.opengroup.org/osdu/platform/system/file/-/raw/master/docs/docs/File-Service.md
https://community.opengroup.org/osdu/platform/system/file/-/raw/master/docs/docs/metadataPayload.json
https://community.opengroup.org/osdu/platform/system/file/-/raw/master/docs/mkdocs.yml
https://community.opengroup.org/osdu/platform/system/file/-/raw/master/file-core/src/main/resources/swagger.properties
https://community.opengroup.org/osdu/platform/system/file/-/raw/master/file-core/src/main/resources/application-shared.properties
https://community.opengroup.org/osdu/platform/system/file/-/raw/master/file-core/src/main/java/org/opengroup/osdu/file/api/FileMetadataApi.java
https://community.opengroup.org/osdu/platform/system/file/-/raw/master/file-core/src/main/java/org/opengroup/osdu/file/api/FileLocationApi.java
https://community.opengroup.org/osdu/platform/system/file/-/raw/master/file-core/src/main/java/org/opengroup/osdu/file/api/FileDeliveryApi.java
https://community.opengroup.org/osdu/platform/system/file/-/raw/master/file-core/src/main/java/org/opengroup/osdu/file/api/FileDmsApi.java
https://community.opengroup.org/osdu/platform/system/file/-/raw/master/file-core/src/main/java/org/opengroup/osdu/file/api/FileCollectionDmsApi.java
https://community.opengroup.org/osdu/platform/system/file/-/raw/master/file-core/src/main/java/org/opengroup/osdu/file/api/FileListApi.java
https://community.opengroup.org/osdu/platform/system/file/-/raw/master/file-core/src/main/java/org/opengroup/osdu/file/api/FileAdminApi.java
https://community.opengroup.org/osdu/platform/system/file/-/raw/master/file-core/src/main/java/org/opengroup/osdu/file/api/DeliveryApi.java
https://community.opengroup.org/osdu/platform/system/file/-/raw/master/file-core/src/main/java/org/opengroup/osdu/file/api/InfoApi.java
https://community.opengroup.org/osdu/platform/system/file/-/raw/master/file-core/src/main/java/org/opengroup/osdu/file/api/HealthCheckApi.java
.../service/FileMetadataService.java, .../service/LocationServiceImpl.java, .../service/FileDeliveryService.java
.../provider/interfaces/*.java, .../middleware/AuthorizationFilter.java, .../swagger/SwaggerConfiguration.java
.../util/ExpiryTimeUtil.java, .../mapper/FileMetadataRecordMapper.java, .../exception/handler/RestExceptionHandler.java
https://community.opengroup.org/osdu/platform/system/file/-/raw/v0.5.0/file-core/src/main/java/.../api/*.java
https://community.opengroup.org/osdu/platform/system/file/-/raw/v0.28.0-aws.1/provider/file-aws/src/main/java/.../impl/StorageServiceImpl.java
https://community.opengroup.org/osdu/platform/system/file/-/raw/fdf450efa0403b93f1bafe62b3e7ac7797103a25/docs/docs/file-service_openapi.yaml
https://community.opengroup.org/osdu/platform/system/lib/core/os-core-common/-/raw/master/src/main/java/org/opengroup/osdu/core/common/model/file/{DriverType,FileLocation,LocationRequest,LocationResponse,FileLocationRequest,FileLocationResponse,FileListRequest,FileListResponse}.java
https://community.opengroup.org/osdu/platform/system/lib/core/os-core-common/-/raw/master/src/main/java/org/opengroup/osdu/core/common/dms/model/{StorageInstructionsResponse,RetrievalInstructionsResponse,DatasetRetrievalProperties,CopyDmsRequest,CopyDmsResponse}.java
https://community.opengroup.org/osdu/platform/system/lib/core/os-core-common/-/raw/master/src/main/java/org/opengroup/osdu/core/common/model/http/DpsHeaders.java
https://community.opengroup.org/osdu/platform/system/lib/core/os-core-common/-/raw/master/src/main/java/org/opengroup/osdu/core/common/model/entitlements/validation/{ValidAcl,AclValidator}.java
https://community.opengroup.org/osdu/platform/system/lib/core/os-core-common/-/raw/master/src/main/java/org/opengroup/osdu/core/common/model/storage/validation/{ValidKind,KindValidator,ValidationDoc}.java
https://community.opengroup.org/osdu/platform/system/lib/cloud/azure/os-core-lib-azure/-/raw/master/src/main/java/org/opengroup/osdu/azure/blobstorage/BlobStore.java
https://community.opengroup.org/osdu/data/data-definitions/-/raw/master/Generated/dataset/File.Generic.1.0.0.json
https://community.opengroup.org/osdu/data/data-definitions/-/raw/master/Generated/dataset/File.Generic.1.1.0.json
https://community.opengroup.org/osdu/data/data-definitions/-/raw/master/Examples/dataset/File.Generic.1.0.0.json
https://community.opengroup.org/osdu/data/data-definitions/-/raw/master/Generated/abstract/{AbstractFile.1.0.0,AbstractDataset.1.0.0,AbstractCommonResources.1.0.0,AbstractFileSourceInfo.1.0.0}.json
https://community.opengroup.org/osdu/data/data-definitions/-/raw/master/E-R/dataset/File.Generic.1.0.0.md
https://community.opengroup.org/osdu/data/data-definitions/-/raw/63303dc077/Generated/file/File.1.0.0.json
https://osdu.pages.opengroup.org/platform/system/file/
https://osdu.pages.opengroup.org/platform/system/file/File-Service/
https://www.ibm.com/docs/en/ednf/5.x?topic=apis-file-api
```

---

## 8. Honest gaps / could-not-verify

| Item | Status |
|---|---|
| `/v2/files/{id}/versions`, `PUT /v2/files/{id}/metadata` | **Not found anywhere** — not in master, not in any tag (v0.4.0…v0.30.2), not in the deleted legacy spec. Mapping grep returns **zero** `@PutMapping` in the whole repo. These likely belong to the **Storage** service (`/records/{id}/{version}`) or a vendor fork. |
| `artifact` metadata field | **Does not exist** in `dataset--File.Generic` (any version, any committed snapshot) nor in the legacy File Service spec. See §3.3. |
| `/health` endpoint | Does not exist as a File Service path. Only `/v2/liveness_check`, `/v2/readiness_check`; GCP adds actuator `health` on management port 8081. |
| Local filesystem driver | Not in the repo at master or in any tag. Bare-metal uses SeaweedFS/S3 via OBM. |
| `DriverType` values beyond `GCS` | `os-core-common` master enum has only `GCS`; provider forks may differ — **UNVERIFIED**. |
| Entitlements role→group bootstrap (`users.datalake.*` → `service.file.*`) | Not in this repo; presumably in Entitlements/partition. **UNVERIFIED exact location.** |
| Landing-zone 24 h auto-delete | Docs claim it; no lifecycle config found in `devops/`. **UNVERIFIED.** |
| CI spec-check mechanics (`reporting/schemathesis.yml`, `cloud-providers/cimpl-global.yml`) | Live in `osdu/platform/ci-cd-pipelines`, not fetched. |
| `os-core-lib-azure-spring-6` (project 1483) vs `os-core-lib-azure` (project 77) `BlobStore` | I read **project 77** (`master`). The File Service Azure provider currently depends on `core-lib-azure`; **UNVERIFIED** which of the two the current build resolves to. |
| `file-dms` project (`osdu/platform/system/file-dms`, id 216) | Exists on GitLab but has an **empty repository** — no DMS service source there. |
| AWS provider | Not on `master`; only in `v*-aws.1` release tags. Master's `provider/` contains only `file-azure`, `file-gc`, `file-ibm`. |
| Azure GitHub mirror `github.com/azure/osdu-file-azure` | Not fetched; the GitLab source at `provider/file-azure` was used instead and is authoritative for this report. |
| `provider/file-gc` module in master | Effectively a stub (`FileGcpDatastoreApplication` + properties); the real GCP code is `file-core-plus`. |
| Initial `osdu.pages.opengroup.org` DNS lookup | Failed once (`EAI_AGAIN`) on first attempt, succeeded on retry. Both doc pages were then fetched successfully. |

### Concrete spec-vs-code divergences to be aware of

1. Committed `docs/api/community/v2/openapi.yaml` omits all `@Hidden` endpoints → **incomplete**.
2. `docs/docs/File-Service.md` claims a 7-day default download-URL TTL; code default is **1 hour**
   (capped at 7 days).
3. `docs/docs/File-Service.md` lists `GET /v2/files/{Id}/metadata` as requiring `service.file.editors`; the code
   requires **`service.file.viewers`**.
4. IBM's public docs list `POST /files/storageInstructions` / `retrievalInstructions` as
   *"NO PERMISSION NEEDED"*; the code applies `service.dataset.editors` / `service.dataset.viewers`.
5. `getFileLocation` always reports `Driver: "GCS"` in the community reference (hardcoded `DriverType.GCS`),
   regardless of the deployed backend.
