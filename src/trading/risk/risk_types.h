
// src/risk/risk_types.h
#pragma once
#include <string>
#include <vector>
#include <chrono>

namespace funding_arbitrage {
namespace trading {
namespace risk {

// 风险限制配置
struct RiskLimits {
    double max_position_size;           // 单个仓位最大规模
    double max_total_positions;         // 总持仓限制
    double max_leverage;               // 最大杠杆倍数
    double max_drawdown;              // 最大回撤限制
    double max_daily_loss;            // 单日最大亏损
    double max_hourly_loss;           // 单小时最大亏损
    double min_margin_ratio;          // 最小保证金率
    double max_funding_exposure;       // 最大资金费率敞口
    int max_trades_per_hour;          // 每小时最大交易次数
    double price_deviation_threshold;  // 价格偏离阈值
};

// 风险指标
struct RiskMetrics {
    double current_drawdown{0.0};
    double daily_pnl{0.0};
    double hourly_pnl{0.0};
    double total_position_value{0.0};
    double margin_ratio{0.0};
    double largest_position_size{0.0};
    int hourly_trade_count{0};
    std::chrono::system_clock::time_point last_update;
};

// 仓位风险信息
struct PositionRisk {
    std::string symbol;
    double size;
    double entry_price;
    double current_price;
    double unrealized_pnl;
    double liquidation_price;
    double margin_ratio;
    double funding_rate;
    double funding_fee;
    int leverage;
};

// 风险监控状态
struct RiskStatus {
    bool is_within_limits;
    std::vector<std::string> violated_limits;
    std::string warning_message;
    RiskMetrics current_metrics;
    std::chrono::system_clock::time_point last_check;
};

// 风险事件类型
enum class RiskEventType {
    MARGIN_CALL,                // 保证金不足
    LIQUIDATION_WARNING,        // 接近强平价
    DRAWDOWN_LIMIT_BREACH,      // 超过回撤限制
    DAILY_LOSS_LIMIT_BREACH,    // 超过日损失限制
    POSITION_LIMIT_BREACH,      // 超过仓位限制
    HIGH_VOLATILITY,           // 高波动性警告
    FUNDING_RATE_WARNING,      // 资金费率警告
    TRADE_FREQUENCY_WARNING    // 交易频率警告
};

// 风险事件
struct RiskEvent {
    RiskEventType type;
    std::string symbol;
    std::string message;
    double threshold_value;
    double current_value;
    std::chrono::system_clock::time_point time;
};

// 风险控制设置
struct RiskControlSettings {
    bool auto_reduce_position;          // 自动减仓
    bool auto_adjust_leverage;          // 自动调整杠杆
    bool emergency_mode_enabled;        // 紧急模式
    double auto_reduce_threshold;       // 自动减仓阈值
    double position_reduction_ratio;    // 减仓比例
    int max_retries;                   // 最大重试次数
    std::chrono::milliseconds retry_delay;  // 重试延迟
};

// 风险报告
struct RiskReport {
    std::string symbol;
    RiskMetrics metrics;
    std::vector<RiskEvent> recent_events;
    std::vector<std::string> warnings;
    std::vector<std::string> recommendations;
    std::chrono::system_clock::time_point report_time;

    // 添加详细信息
    std::string getFormattedReport() const {
        // 实现报告格式化逻辑
        return "";
    }
};

} // namespace risk
} // namespace trading
} // namespace funding_arbitrage