#include "common/config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace wops {

namespace {

std::string trim_copy(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

}  // namespace

bool load_config_file(const std::string& path, ConfigFile* config, std::string* error) {
    std::ifstream file(path);
    if (!file) {
        if (error) {
            *error = "could not open config file: " + path;
        }
        return false;
    }

    std::string line;
    int line_no = 0;
    while (std::getline(file, line)) {
        ++line_no;

        const auto comment = line.find_first_of("#;");
        if (comment != std::string::npos) {
            line = line.substr(0, comment);
        }

        line = trim_copy(line);
        if (line.empty()) {
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            if (error) {
                *error = path + ":" + std::to_string(line_no) + ": expected key=value";
            }
            return false;
        }

        auto key = lower_copy(trim_copy(line.substr(0, equals)));
        auto value = trim_copy(line.substr(equals + 1));
        if (key.empty()) {
            if (error) {
                *error = path + ":" + std::to_string(line_no) + ": empty key";
            }
            return false;
        }
        config->values[key].push_back(value);
    }

    return true;
}

std::string config_get(const ConfigFile& config, const std::string& key, const std::string& fallback) {
    const auto it = config.values.find(lower_copy(key));
    if (it == config.values.end() || it->second.empty()) {
        return fallback;
    }
    return it->second.back();
}

std::vector<std::string> config_get_all(const ConfigFile& config, const std::string& key) {
    const auto it = config.values.find(lower_copy(key));
    if (it == config.values.end()) {
        return {};
    }
    return it->second;
}

}  // namespace wops

