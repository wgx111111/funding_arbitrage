#pragma once
#include "alert_types.h"
#include "common/config/config.h"
#include "common/logger/logger.h"
#include <memory>
#include <queue>
#include <mutex>
#include <map>
#include <functional>

namespace funding_arbitrage {
namespace monitor {
namespace alerts {

class AlertManager {
public:
    explicit AlertManager(const std::shared_ptr<config::Config>& config);
    
    // 告警接口
    void sendAlert(const Alert& alert);
    void resolveAlert(AlertType type, const std::string& source);
    
    // 配置接口
    void setAlertCallback(std::function<void(const Alert&)> callback);
    void setAlertConfig(const AlertConfig& config);
    void enableAlertType(AlertType type, bool enable);
    
    // 查询接口
    std::vector<Alert> getActiveAlerts() const;
    std::vector<Alert> getAlertHistory(
        const std::chrono::system_clock::time_point& start_time,
        const std::chrono::system_clock::time_point& end_time) const;
    
    // 告警统计
    int getAlertCount(AlertType type) const;
    bool isAlertThrottled(AlertType type) const;

private:
    std::shared_ptr<Logger> logger_;
    AlertConfig config_;
    std::function<void(const Alert&)> alert_callback_;
    
    // 告警存储
    mutable std::mutex alerts_mutex_;
    std::vector<Alert> alert_history_;
    std::map<std::pair<AlertType, std::string>, Alert> active_alerts_;
    
    // 告警限流
    std::map<AlertType, std::chrono::system_clock::time_point> last_alert_times_;
    std::map<AlertType, int> alert_counts_;
    
    // 告警通道
    void sendEmailAlert(const Alert& alert);
    void sendSlackAlert(const Alert& alert);
    void sendTelegramAlert(const Alert& alert);
    
    // 内部方法
    bool shouldThrottleAlert(const Alert& alert);
    void cleanupAlertHistory();
    void updateAlertCounts();
};

} // namespace alerts
} // namespace monitor
} // namespace funding_arbitrage