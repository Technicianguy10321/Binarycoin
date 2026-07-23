#include "logger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace bincoin {
namespace {

std::string timestamp_utc() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S")
           << '.' << std::setw(3) << std::setfill('0') << milliseconds << 'Z';
    return output.str();
}

} // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::configure(
    const std::filesystem::path& log_path,
    const bool print_to_console,
    std::set<std::string> debug_categories
) {
    std::lock_guard lock(mutex_);
    if (!log_path.parent_path().empty()) std::filesystem::create_directories(log_path.parent_path());
    file_.close();
    file_.open(log_path, std::ios::app);
    if (!file_) throw std::runtime_error("Unable to open debug log: " + log_path.string());
    print_to_console_ = print_to_console;
    debug_categories_ = std::move(debug_categories);
}

void Logger::set_debug_categories(std::set<std::string> categories) {
    std::lock_guard lock(mutex_);
    debug_categories_ = std::move(categories);
}

std::set<std::string> Logger::debug_categories() const {
    std::lock_guard lock(mutex_);
    return debug_categories_;
}

bool Logger::debug_enabled(const std::string_view category) const {
    std::lock_guard lock(mutex_);
    return debug_categories_.contains("1") || debug_categories_.contains("all") ||
           debug_categories_.contains(std::string(category));
}

void Logger::info(const std::string_view category, const std::string_view message) {
    write("INFO", category, message);
}

void Logger::debug(const std::string_view category, const std::string_view message) {
    if (debug_enabled(category)) write("DEBUG", category, message);
}

void Logger::error(const std::string_view category, const std::string_view message) {
    write("ERROR", category, message);
}

void Logger::write(
    const std::string_view level,
    const std::string_view category,
    const std::string_view message
) {
    std::lock_guard lock(mutex_);
    std::ostringstream line;
    line << timestamp_utc() << " [" << std::this_thread::get_id() << "] "
         << '[' << category << "] " << level << ": " << message << '\n';
    if (file_) {
        file_ << line.str();
        file_.flush();
    }
    if (print_to_console_) {
        std::ostream& stream = level == "ERROR" ? std::cerr : std::cout;
        stream << line.str();
        stream.flush();
    }
}

void Logger::flush() {
    std::lock_guard lock(mutex_);
    if (file_) file_.flush();
    std::cout.flush();
    std::cerr.flush();
}

} // namespace bincoin
