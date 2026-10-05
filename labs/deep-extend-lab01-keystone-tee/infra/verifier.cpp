// Lab adapter for Keystone's attestation example at revision 73aeab79.
// Uses Keystone SDK measurements and report verification, while delegating
// the final accept/reject decision to exercises/report_policy.hpp.
#include "verifier.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "host.h"
#include "host/hash_util.hpp"
#include "host/keystone.h"
#include "report_policy.hpp"
#include "verifier/report.h"
#include "verifier/test_dev_key.h"

void Verifier::run() {
  std::random_device random;
  std::string nonce;
  const char digits[] = "0123456789abcdef";
  for (int i = 0; i < 16; ++i) {
    const unsigned int value = random() & 0xff;
    nonce.push_back(digits[value >> 4]);
    nonce.push_back(digits[value & 0xf]);
  }
  Host host(params_, eapp_file_, rt_file_, ld_file_);
  Report report = host.run(nonce);
  verify_report(report, nonce);
}

void Verifier::verify_report(Report& report, const std::string& nonce) {
  byte expected_enclave_hash[MDSIZE];
  byte expected_sm_hash[MDSIZE];
  compute_expected_enclave_hash(expected_enclave_hash);
  compute_expected_sm_hash(expected_sm_hash);

  // Controlled negative cases. Only the verifier's inputs/report copy change;
  // the Keystone SM, driver, runtime and enclave remain the upstream build.
  std::string expected_nonce = nonce;
  const char* fault = std::getenv("LAB_FAULT");
  if (fault != nullptr && std::strcmp(fault, "hash") == 0) {
    expected_enclave_hash[0] ^= 1;
  } else if (fault != nullptr && std::strcmp(fault, "nonce") == 0) {
    expected_nonce[0] ^= 1;
  } else if (fault != nullptr && std::strcmp(fault, "signature") == 0) {
    if (report.getDataSize() > 0 && report.getDataSize() <= ATTEST_DATA_MAXLEN) {
      static_cast<byte*>(report.getDataSection())[0] ^= 1;
    }
  } else if (fault != nullptr && *fault != '\0') {
    throw std::runtime_error("unsupported LAB_FAULT value");
  }

  if (!lab_report_accept(report, expected_enclave_hash, expected_sm_hash,
                         _sanctum_dev_public_key, expected_nonce,
                         ATTEST_DATA_MAXLEN)) {
    std::fprintf(stderr, "LAB01_ATTEST=REJECT\n");
    throw std::runtime_error("attestation rejected");
  }
  std::puts("LAB01_ATTEST=PASS");
}

void Verifier::compute_expected_enclave_hash(byte* output) {
  Keystone::Enclave::measure(reinterpret_cast<char*>(output),
                             eapp_file_.c_str(), rt_file_.c_str(),
                             ld_file_.c_str());
}

void Verifier::compute_expected_sm_hash(byte* output) {
  // Match the upstream example's boot ROM measurement window. Keep the
  // remainder zero-filled when fw_jump.bin is shorter than this window.
  const std::size_t measured_size = 0x1ff000;
  std::vector<byte> image(measured_size, 0);
  FILE* firmware = std::fopen(sm_bin_file_.c_str(), "rb");
  if (!firmware) throw std::runtime_error("cannot open SM image");
  const std::size_t bytes = std::fread(image.data(), 1, image.size(), firmware);
  const int read_error = std::ferror(firmware);
  std::fclose(firmware);
  if (read_error || bytes == 0) throw std::runtime_error("cannot read SM image");
  hash_ctx_t hash;
  hash_init(&hash);
  hash_extend(&hash, image.data(), image.size());
  hash_finalize(output, &hash);
}
