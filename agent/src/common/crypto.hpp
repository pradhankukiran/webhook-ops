#pragma once

#include <cstddef>
#include <string>

namespace wops {

std::string random_hex(size_t bytes, std::string* error);
std::string hmac_sha256_hex(const std::string& secret, const std::string& message);
bool constant_time_equals(const std::string& a, const std::string& b);

}  // namespace wops

