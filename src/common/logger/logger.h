#pragma once
#include <string>
#include <fstream>
#include <memory>
#include <mutex>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace funding_arbitrage {
namespace common {
namespace logger {

class Logger {
public:
    enum class Level {
        DEBUG,
        INFO,
        WARN,
        ERROR,
        CRITICAL
    };

    explicit Logger(const std::string& name);
    ~Logger() = default;

    // 禁用拷贝构造和赋值操作
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // 日志记录方法
    void debug(const std::string& message);
    void info(const std::string& message);
    void warn(const std::string& message);
    void error(const std::string& message);
    void critical(const std::string& message);

    // 设置日志级别
    void setLevel(Level level);

    // 获取logger名称
    const std::string& getName() const { return name_; }

private:
    std::string name_;
    std::shared_ptr<spdlog::logger> logger_;
    static std::mutex initMutex_;

    // 初始化logger
    void initializeLogger();
    
    // 将Level枚举转换为spdlog level
    static spdlog::level::level_enum convertLevel(Level level);
};

} // namespace logger
} // namespace common
} // namespace funding_arbitrage