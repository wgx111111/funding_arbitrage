#pragma once
#include <string>
#include <chrono>
#include <vector>

namespace funding_arbitrage {
namespace monitor {
namespace alerts {

// 告警级别
enum class AlertLevel {
    INFO,
    WARNING,
    ERROR,
    CRITICAL
};

// 告警类型
enum class AlertType {
    SYSTEM_ERROR,           // 系统错误
    CONNECTION_LOST,        // 连接丢失
    HIGH_LATENCY,          // 高延迟
    POSITION_RISK,         // 仓位风险
    MARGIN_RISK,           // 保证金风险
    LIQUIDATION_RISK,      // 强平风险
    DRAWDOWN_WARNING,      // 回撤警告
    PNL_WARNING,           // 盈亏警告
    FUNDING_RATE_WARNING,  // 资金费率警告
    TRADE_FREQUENCY_HIGH   // 交易频率过高
};

// 告警内容
struct Alert {
    AlertType type;
    AlertLevel level;
    std::string source;     // 告警来源
    std::string message;    // 告警消息
    std::string details;    // 详细信息
    double threshold;       // 阈值
    double current_value;   // 当前值
    bool is_resolved;       // 是否已解决
    std::chrono::system_clock::time_point time;  // 告警时间
    std::chrono::system_clock::time_point resolve_time;  // 解决时间
};

// 告警配置
struct AlertConfig {
    bool email_enabled;
    bool slack_enabled;
    bool telegram_enabled;
    std::string email_recipients;
    std::string slack_webhook;
    std::string telegram_bot_token;
    std::string telegram_chat_id;
    int alert_interval_seconds;     // 相同告警的最小间隔
    int max_alerts_per_hour;        // 每小时最大告警数
};

} // namespace alerts
} // namespace monitor
} // namespace funding_arbitrage