#include "common/ids/id_generator.h"

#include <array>
#include <cstdio>

#include "common/crypto/crypto.h"

namespace fss {
namespace {

// 把 16 字节按 RFC 4122 格式化为 8-4-4-4-12，并设置版本位(4)与变体位(10xx)
std::string FormatUuid(const std::array<std::uint8_t, 16>& b) {
  char buf[40];
  ::snprintf(buf, sizeof(buf),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
             b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14],
             b[15]);
  return std::string(buf);
}

std::string FormatNoDash(const std::array<std::uint8_t, 16>& b) {
  char buf[40];
  ::snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12],
             b[13], b[14], b[15]);
  return std::string(buf);
}

std::array<std::uint8_t, 16> NewRandomUuidBytes() {
  const auto raw = crypto::RandomBytes(16);
  std::array<std::uint8_t, 16> b{};
  for (std::size_t i = 0; i < 16; ++i) b[i] = raw[i];
  b[6] = static_cast<std::uint8_t>((b[6] & 0x0F) | 0x40);  // version 4
  b[8] = static_cast<std::uint8_t>((b[8] & 0x3F) | 0x80);  // variant 10xx
  return b;
}

}  // namespace

std::string UuidGenerator::NewUuid() const { return FormatUuid(NewRandomUuidBytes()); }
std::string UuidGenerator::NewUuidNoDash() const { return FormatNoDash(NewRandomUuidBytes()); }

std::string SequentialIdGenerator::NewUuid() const {
  char buf[48];
  ::snprintf(buf, sizeof(buf), "00000000-0000-4000-8000-%012llx",
             static_cast<unsigned long long>(next_++));
  return std::string(buf);
}

std::string SequentialIdGenerator::NewUuidNoDash() const {
  char buf[48];
  ::snprintf(buf, sizeof(buf), "%032llx", static_cast<unsigned long long>(next_++));
  return std::string(buf);
}

}  // namespace fss
