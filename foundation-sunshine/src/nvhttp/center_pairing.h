#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/x509.h>

namespace nvhttp::center_pairing {
  inline std::string host_key;
  inline std::string managed_token;
  inline constexpr char enrollment_path[] = "/sunandmon-pair";
  inline constexpr int managed_restart_exit_code = 85;
  inline constexpr std::int64_t center_ticket_lifetime_seconds = 60;
  inline constexpr std::int64_t clock_grace_seconds = 30 * 60;

  inline bool
  lower_hex(std::string_view value, std::size_t length) {
    return value.size() == length && std::all_of(value.begin(), value.end(), [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
  }

  inline std::string
  hex(const unsigned char *bytes, std::size_t length) {
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(length * 2);
    for (std::size_t i = 0; i < length; ++i) {
      output.push_back(digits[bytes[i] >> 4]);
      output.push_back(digits[bytes[i] & 15]);
    }
    return output;
  }

  inline std::string
  sign(std::string_view message) {
    std::array<unsigned char, 32> digest {};
    unsigned int length = 0;
    if (!HMAC(EVP_sha256(), host_key.data(), static_cast<int>(host_key.size()),
          reinterpret_cast<const unsigned char *>(message.data()), message.size(), digest.data(), &length) ||
        length != digest.size()) {
      return {};
    }
    return hex(digest.data(), digest.size());
  }

  inline std::string
  fingerprint(const X509 *cert) {
    std::array<unsigned char, 32> digest {};
    unsigned int length = 0;
    if (!cert || X509_digest(cert, EVP_sha256(), digest.data(), &length) != 1 || length != digest.size()) {
      return {};
    }
    return hex(digest.data(), digest.size());
  }

  inline bool
  consume(std::string_view ticket, const X509 *cert) {
    if (host_key.empty() || !cert || ticket.size() > 120) {
      return false;
    }
    const auto first = ticket.find('.');
    const auto last = ticket.rfind('.');
    if (first == std::string_view::npos || first == last) {
      return false;
    }
    const auto expiry_field = ticket.substr(0, first);
    const auto nonce = ticket.substr(first + 1, last - first - 1);
    const auto signature = ticket.substr(last + 1);
    if (nonce.size() != 26 || !lower_hex(signature, 64)) {
      return false;
    }

    std::int64_t expiry = 0;
    const auto [end, error] = std::from_chars(expiry_field.data(), expiry_field.data() + expiry_field.size(), expiry);
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
    if (error != std::errc {} || end != expiry_field.data() + expiry_field.size() ||
        expiry <= now - clock_grace_seconds ||
        expiry > now + center_ticket_lifetime_seconds + clock_grace_seconds) {
      return false;
    }

    const auto cert_fingerprint = fingerprint(cert);
    if (cert_fingerprint.empty()) {
      return false;
    }
    const auto payload = std::string(ticket.substr(0, last)) + "." + cert_fingerprint;
    const auto expected = sign("ticket:" + payload);
    if (expected.empty() || CRYPTO_memcmp(expected.data(), signature.data(), signature.size()) != 0) {
      return false;
    }

    static std::mutex mutex;
    static std::unordered_map<std::string, std::int64_t> redeemed;
    std::lock_guard lock { mutex };
    std::erase_if(redeemed, [now](const auto &entry) { return entry.second <= now; });
    // Keep the nonce until the ticket can no longer be accepted, including clock grace.
    return redeemed.size() < 4096 && redeemed.emplace(nonce, expiry + clock_grace_seconds).second;
  }
}  // namespace nvhttp::center_pairing
