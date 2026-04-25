#include "common/crypto.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <array>
#include <iomanip>
#include <sstream>
#include <vector>

namespace wops {

namespace {

std::string to_hex(const unsigned char* data, size_t len) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        out << std::setw(2) << static_cast<int>(data[i]);
    }
    return out.str();
}

}  // namespace

std::string random_hex(size_t bytes, std::string* error) {
    std::vector<unsigned char> buffer(bytes);
    if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1) {
        if (error) {
            *error = "RAND_bytes failed";
        }
        return {};
    }
    return to_hex(buffer.data(), buffer.size());
}

std::string hmac_sha256_hex(const std::string& secret, const std::string& message) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_len = 0;
    HMAC(
        EVP_sha256(),
        secret.data(),
        static_cast<int>(secret.size()),
        reinterpret_cast<const unsigned char*>(message.data()),
        message.size(),
        digest.data(),
        &digest_len);
    return to_hex(digest.data(), digest_len);
}

bool constant_time_equals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

}  // namespace wops

