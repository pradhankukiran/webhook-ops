#pragma once

#include <map>
#include <string>
#include <vector>

namespace wops {

struct ConfigFile {
    std::map<std::string, std::vector<std::string>> values;
};

bool load_config_file(const std::string& path, ConfigFile* config, std::string* error);
std::string config_get(const ConfigFile& config, const std::string& key, const std::string& fallback = {});
std::vector<std::string> config_get_all(const ConfigFile& config, const std::string& key);

}  // namespace wops

