#include "logger.h"
#include <spdlog/async.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <filesystem>

namespace funding_arbitrage {
namespace common {
namespace logger {

std::mutex Logger::initMutex_;

Logger::Logger(const std::string& name) : name_(name) {
    initializeLogger();
}

void Logger::initializeLogger() {
    std::lock_guard<std::mutex> lock(initMutex_);

    // 检查logger是否已经存在
    logger_ = spdlog::get(name_);
    if (logger_) {
        return;
    }

    try {
        // 创建日志目录
        std::filesystem::path logDir = "log";
        if (!std::filesystem::exists(logDir)) {
            std::filesystem::create_directory(logDir);
        }

        // 创建控制台sink
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(spdlog::level::debug);
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");

        // 创建文件sink
        auto file_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(
            "log/" + name_ + ".log", 
            0, 
            0,
            false
        );
        file_sink->set_level(spdlog::level::debug);
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] [%t] %v");

        // 创建多sink logger
        std::vector<spdlog::sink_ptr> sinks {console_sink, file_sink};
        logger_ = std::make_shared<spdlog::logger>(name_, sinks.begin(), sinks.end());
        logger_->set_level(spdlog::level::debug);
        logger_->flush_on(spdlog::level::debug);

        // 注册logger
        spdlog::register_logger(logger_);

    } catch (const spdlog::spdlog_ex& ex) {
        throw std::runtime_error("Logger initialization failed: " + std::string(ex.what()));
    }
}

void Logger::debug(const std::string& message) {
    logger_->debug(message);
}

void Logger::info(const std::string& message) {
    logger_->info(message);
}

void Logger::warn(const std::string& message) {
    logger_->warn(message);
}

void Logger::error(const std::string& message) {
    logger_->error(message);
}

void Logger::critical(const std::string& message) {
    logger_->critical(message);
}

void Logger::setLevel(Level level) {
    logger_->set_level(convertLevel(level));
}

spdlog::level::level_enum Logger::convertLevel(Level level) {
    switch (level) {
        case Level::DEBUG:
            return spdlog::level::debug;
        case Level::INFO:
            return spdlog::level::info;
        case Level::WARN:
            return spdlog::level::warn;
        case Level::ERROR:
            return spdlog::level::err;
        case Level::CRITICAL:
            return spdlog::level::critical;
        default:
            return spdlog::level::info;
    }
}

} // namespace logger
} // namespace common
} // namespace funding_arbitrage
