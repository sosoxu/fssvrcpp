// =============================================================================
//  Catch2 主入口（**唯一**）
// =============================================================================
//  为什么单独放一个文件：Catch2 v2 要求每个测试可执行文件里**恰好一个** TU 定义
//  CATCH_CONFIG_MAIN。放在各测试文件里容易漏（本项目就漏过一次：
//  test_result.cpp / test_crypto.cpp 忘了写，症状是链接期
//  "undefined reference to main" 加一堆 Catch 符号未定义）。
//  现在由 fss_add_test 统一注入本文件，测试文件里**不要**再写 CATCH_CONFIG_MAIN。
#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
