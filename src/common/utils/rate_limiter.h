#pragma once
#include <chrono>
#include <queue>
#include <mutex>
#include "../logger/logger.h"

namespace funding_arbitrage {
namespace common {
namespace utils {

class RateLimiter {
public:
    RateLimiter(int requests_per_second, int max_burst = 1)
        : requests_per_second_(requests_per_second)
        , max_burst_(max_burst)
        , logger_(std::make_shared<utils::Logger>("RateLimiter")) {
    }

    void acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        
        // 清理过期的请求记录
        while (!request_times_.empty() && 
               now - request_times_.front() > std::chrono::seconds(1)) {
            request_times_.pop();
        }

        // 检查是否超过限制
        if (request_times_.size() >= requests_per_second_) {
            auto sleep_duration = std::chrono::seconds(1) - 
                                (now - request_times_.front());
            if (sleep_duration > std::chrono::milliseconds(0)) {
                logger_->debug("Rate limit reached, sleeping for " + 
                             std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>
                             (sleep_duration).count()) + "ms");
                std::this_thread::sleep_for(sleep_duration);
                now = std::chrono::steady_clock::now();
            }
        }

        request_times_.push(now);
    }

private:
    int requests_per_second_;
    int max_burst_;
    std::queue<std::chrono::steady_clock::time_point> request_times_;
    std::mutex mutex_;
    std::shared_ptr<utils::Logger> logger_;
};

// 重试策略配置
struct RetryConfig {
    int max_retries = 3;
    std::chrono::milliseconds initial_delay{100};
    std::chrono::milliseconds max_delay{5000};
    double backoff_multiplier = 2.0;
    
    // 定义哪些HTTP状态码需要重试
    std::set<int> retriable_status_codes = {
        408, // Request Timeout
        429, // Too Many Requests
        500, // Internal Server Error
        502, // Bad Gateway
        503, // Service Unavailable
        504  // Gateway Timeout
    };
};

} // namespace utils
} // namespace common
} // namespace funding_arbitrage