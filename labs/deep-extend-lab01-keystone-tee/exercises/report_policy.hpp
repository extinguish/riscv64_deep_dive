#pragma once

#include <cstddef>
#include <string>

// Report is a Keystone SDK Report (or the unit-test double).
// The report came through the untrusted host/shared-buffer path. In particular,
// getDataSize() must be checked before Report::verify(), because the latter
// uses that size when checking the enclave signature.
template <typename Report>
bool lab_report_accept(Report& report,
                       const unsigned char* expected_enclave_hash,
                       const unsigned char* expected_sm_hash,
                       const unsigned char* device_public_key,
                       const std::string& nonce,
                       std::size_t max_report_data) {
  (void)report;
  (void)expected_enclave_hash;
  (void)expected_sm_hash;
  (void)device_public_key;
  (void)nonce;
  (void)max_report_data;
  // TODO: reject empty/oversized nonce, invalid length and null report data
  //       before calling verify().
  // TODO: require signature + both measurements to verify.
  // TODO: compare the exact nonce bytes and expected trailing NUL.
  return false;
}
