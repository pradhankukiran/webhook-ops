#pragma once

#include <initializer_list>
#include <string>
#include <utility>

namespace wops {

enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error,
};

using LogField = std::pair<std::string, std::string>;

void set_log_level(LogLevel level);
void log(LogLevel level, const std::string& component, const std::string& event, std::initializer_list<LogField> fields = {});

void log_debug(const std::string& component, const std::string& event, std::initializer_list<LogField> fields = {});
void log_info(const std::string& component, const std::string& event, std::initializer_list<LogField> fields = {});
void log_warn(const std::string& component, const std::string& event, std::initializer_list<LogField> fields = {});
void log_error(const std::string& component, const std::string& event, std::initializer_list<LogField> fields = {});

}  // namespace wops

