#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

namespace bincoin {

class Logger {
public:
    static Logger& instance();

    void configure(
        const std::filesystem::path& log_path,
        bool print_to_console,
        std::set<std::string> debug_categories = {}
    );
    void set_debug_categories(std::set<std::string> categories);
    [[nodiscard]] std::set<std::string> debug_categories() const;
    [[nodiscard]] bool debug_enabled(std::string_view category) const;

    void info(std::string_view category, std::string_view message);
    void debug(std::string_view category, std::string_view message);
    void error(std::string_view category, std::string_view message);
    void flush();

private:
    Logger() = default;
    void write(std::string_view level, std::string_view category, std::string_view message);

    mutable std::mutex mutex_;
    std::ofstream file_;
    bool print_to_console_{false};
    std::set<std::string> debug_categories_;
};

inline void log_info(const std::string_view category, const std::string_view message) {
    Logger::instance().info(category, message);
}

inline void log_debug(const std::string_view category, const std::string_view message) {
    Logger::instance().debug(category, message);
}

inline void log_error(const std::string_view category, const std::string_view message) {
    Logger::instance().error(category, message);
}

} // namespace bincoin
