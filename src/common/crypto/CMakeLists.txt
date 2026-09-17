# L1：密码学基础（OpenSSL 后端）。只允许被 L2/L5 使用；L3 领域层不得直接依赖
# （领域层通过 ISelfSignedUrlCodec 端口使用）。
add_library(fss_crypto STATIC crypto.cpp)
target_include_directories(fss_crypto PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fss_crypto PUBLIC fss_common OpenSSL::Crypto)
