# 上游验收样例（vendored，Apache-2.0）

这些文件**逐字**取自 OSDU File Service 上游仓库的验收测试资源，用作契约 §3.4 的
一手依据（此前我们只能从调研笔记得知"期望消息要点"，无法逐字比对）。

| 项 | 值 |
| --- | --- |
| 来源仓库 | `osdu-file-service`（本机归档：`/home/ll/osdu-file-upstream`） |
| 归档 commit | `d7c25c2d7f5d2f42bed901c68a407098195389bb` |
| 子目录 | `testing/file-test-baremetal/src/test/resources/{input_payloads,output_payloads}` |
| 许可证 | Apache License 2.0（见上游仓库 `LICENSE`） |
| 复制日期 | 见本仓库 `docs/test-evidence/phase4.md` 的切片 5 记录 |

## 文件对应关系

- `File_<样例>.json` —— 请求体（`POST /api/file/v2/files/metadata` 的输入）
- `File_<样例>_msg.json` —— 上游断言期望的**错误报文**（`error.code` / `error.errors[].message`）
- `File_CorrectPayload.json` —— 权威黄金样例（同时被 `tests/unit/test_file_metadata.cpp` 使用）
- `File_Calculate_Checksum.json` —— 正向样例：服务端覆写校验和

## ⚠️ 两行在上游被注释掉（未真正执行）

`testing/file-test-*/.../features/IntegrationTest_File_POST.feature` 的 Examples 表里：

```
#      | "/input_payloads/File_invalid_ScalarIndicator.json" | "400" | ... |
      #| "/input_payloads/File_Datatype_Mismatch.json"       | "400" | ... |
```

也就是说上游自己**没有**运行这两条（`#` 开头）。本仓库的处理：

- `File_invalid_ScalarIndicator`：**照实现**。上游的枚举定义与消息在源码里是明确的
  （`filedetails/ScalarIndicator.java` = `STANDARD`/`NOSCALE`/`OVERRIDE`，
  `EnumValidationException` 产出 `Invalid value of <v> for ScalarIndicator`），
  我们据此实现并断言，比上游测得更严。
- `File_Datatype_Mismatch`：状态码对齐（400），**消息不对齐**（上游期望的是 Jackson
  反序列化失败的通用 `Bad Request. Invalid Input.`，我们给的是更具体的约束消息）。
  理由：该行上游未执行，且"更具体的消息"对客户端排障更有价值；差异已在
  `docs/03-api-contract.md` §3.4 与阶段证据中显式登记。

## 占位符

上游样例里的 `<tenant_name>` / `<acl_viewers>` / `<legal_tags>` / `<cloud_domain>` 等
占位符由测试脚手架替换；我们的用例在装载后按同样方式替换（见 `test_rest_contract.cpp`）。
