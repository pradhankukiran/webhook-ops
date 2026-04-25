#include "common/log.hpp"

#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace wops {

namespace {

std::mutex g_log_mu;
LogLevel g_level = LogLevel::Info;

int level_rank(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return 0;
        case LogLevel::Info:
            return 1;
        case LogLevel::Warn:
            return 2;
        case LogLevel::Error:
            return 3;
    }
    return 1;
}

const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
    }
    return "INFO";
}

std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string quote_value(const std::string& value) {
    if (value.empty()) {
        return "\"\"";
    }

    bool needs_quote = false;
    for (const char ch : value) {
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == '"' || ch == '\\' || ch == '=') {
            needs_quote = true;
            break;
        }
    }
    if (!needs_quote) {
        return value;
    }

    std::string quoted = "\"";
    for (const char ch : value) {
        if (ch == '"' || ch == '\\') {
            quoted.push_back('\\');
        }
        quoted.push_back(ch);
    }
    quoted.push_back('"');
    return quoted;
}

}  // namespace

void set_log_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_log_mu);
    g_level = level;
}

void log(LogLevel level, const std::string& component, const std::string& event, std::initializer_list<LogField> fields) {
    std::lock_guard<std::mutex> lock(g_log_mu);
    if (level_rank(level) < level_rank(g_level)) {
        return;
    }

    std::ostream& out = level_rank(level) >= level_rank(LogLevel::Warn) ? std::cerr : std::cout;
    out << utc_timestamp();
    out << " level=" << level_name(level);
    out << " component=" << quote_value(component);
    out << " event=" << quote_value(event);
    for (const auto& [key, value] : fields) {
        out << " " << key << "=" << quote_value(value);
    }
    out << "\n";
}

void log_debug(const std::string& component, const std::string& event, std::initializer_list<LogField> fields) {
    log(LogLevel::Debug, component, event, fields);
}

void log_info(const std::string& component, const std::string& event, std::initializer_list<LogField> fields) {
    log(LogLevel::Info, component, event, fields);
}

void log_warn(const std::string& component, const std::string& event, std::initializer_list<LogField> fields) {
    log(LogLevel::Warn, component, event, fields);
}

void log_error(const std::string& component, const std::string& event, std::initializer_list<LogField> fields) {
    log(LogLevel::Error, component, event, fields);
}

}  // namespace wops
