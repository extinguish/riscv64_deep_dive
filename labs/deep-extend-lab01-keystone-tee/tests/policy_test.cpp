#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "report_policy.hpp"

namespace {
struct MockReport {
  std::array<unsigned char, 32> payload{};
  std::size_t size = 0;
  bool cryptographic_result = true;
  bool verify_called = false;
  bool null_data = false;
  int verify(const unsigned char*, const unsigned char*, const unsigned char*) {
    verify_called = true;
    return cryptographic_result ? 1 : 0;
  }
  std::size_t getDataSize() { return size; }
  void* getDataSection() { return null_data ? nullptr : payload.data(); }
};

int failures = 0;
void check(bool ok, const char* name) {
  std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
  if (!ok) ++failures;
}
bool accepts(MockReport& r, const std::string& nonce, std::size_t limit = 32) {
  const unsigned char expected_hash[1] = {1};
  const unsigned char expected_sm[1] = {2};
  const unsigned char key[1] = {3};
  return lab_report_accept(r, expected_hash, expected_sm, key, nonce, limit);
}
}  // namespace

int main() {
  MockReport valid;
  std::memcpy(valid.payload.data(), "nonce", 6);
  valid.size = 6;
  check(accepts(valid, "nonce") && valid.verify_called, "valid signed report");

  MockReport bad_signature = valid;
  bad_signature.cryptographic_result = false;
  bad_signature.verify_called = false;
  check(!accepts(bad_signature, "nonce") && bad_signature.verify_called,
        "signature or measurement mismatch");

  MockReport stale_nonce = valid;
  check(!accepts(stale_nonce, "other"), "wrong nonce");

  MockReport short_nonce = valid;
  short_nonce.size = 5;
  check(!accepts(short_nonce, "nonce"), "missing terminator");

  MockReport bad_terminator = valid;
  bad_terminator.payload[5] = '!';
  check(!accepts(bad_terminator, "nonce"), "wrong terminator");

  MockReport embedded_null;
  const unsigned char embedded_bytes[] = {'n', 'o', 0, 'n', 'c', 'e', 0};
  std::memcpy(embedded_null.payload.data(), embedded_bytes, sizeof(embedded_bytes));
  embedded_null.size = sizeof(embedded_bytes);
  check(!accepts(embedded_null, std::string("no\0xxx", 6)),
        "embedded NUL does not truncate comparison");

  MockReport oversized = valid;
  oversized.size = 33;
  oversized.verify_called = false;
  check(!accepts(oversized, "nonce") && !oversized.verify_called,
        "oversized report rejected before crypto");

  MockReport too_long_nonce = valid;
  too_long_nonce.verify_called = false;
  check(!accepts(too_long_nonce, std::string(32, 'x')) &&
            !too_long_nonce.verify_called,
        "oversized challenge rejected before crypto");

  MockReport missing_data = valid;
  missing_data.null_data = true;
  missing_data.verify_called = false;
  check(!accepts(missing_data, "nonce") && !missing_data.verify_called,
        "missing payload rejected before crypto");

  MockReport empty_challenge = valid;
  empty_challenge.verify_called = false;
  check(!accepts(empty_challenge, "") && !empty_challenge.verify_called,
        "empty challenge rejected before crypto");

  return failures ? 1 : 0;
}
