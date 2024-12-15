// src/risk/risk_manager.h
#pragma once
#include "risk_types.h"
#include "../config/config.h"
#include "../api/binance_api.h"
#include "../utils/logger.h"
#include "../execution/execution_types.h"
#include <memory>
#include <string>
#include <map>
#include <mutex>
#include <queue>

namespace funding_arbitrage {
namespace trading {
namespace risk {

class RiskManager {
public:
    RiskManager(const std::shared_ptr<config::Config>& config,
                std::shared_ptr<api::BinanceApi> api);

    // 风险检查接口
    bool checkNewPosition(const std::string& symbol, double size, double funding_rate);
    bool checkIncreasePosition(const std::string& symbol, double additional_size);
    bool checkClosePosition(const std::string& symbol);
    bool checkLeverageChange(const std::string& symbol, int new_leverage);
    bool checkPriceDeviation(const std::string& symbol, double intended_price);

    // 风险计算接口
    double calculateMaxAllowedPosition(const std::string& symbol);
    PositionRisk getPositionRisk(const std::string& symbol);
    RiskReport generateRiskReport(const std::string& symbol);

    // 风险监控接口
    RiskStatus getStatus() const;
    RiskMetrics getMetrics() const;
    std::vector<RiskEvent> getRecentEvents() const;
    
    // 风控设置接口
    void setRiskLimit(const std::string& limit_name, double value);
    void setRiskControlSettings(const RiskControlSettings& settings);
    void enableEmergencyMode(bool enable = true);
    void registerRiskEventCallback(std::function<void(const RiskEvent&)> callback);

private:
    std::shared_ptr<api::BinanceApi> api_;
    std::shared_ptr<utils::Logger> logger_;
    
    // 配置和状态
    RiskLimits limits_;
    RiskControlSettings control_settings_;
    std::atomic<bool> emergency_mode_;

    // 风险数据
    mutable std::mutex metrics_mutex_;
    RiskMetrics current_metrics_;
    std::map<std::string, PositionRisk> position_risks_;
    std::queue<RiskEvent> recent_events_;
    std::map<std::string, std::vector<execution::TradeInfo>> trade_history_;

    // 回调函数
    std::function<void(const RiskEvent&)> risk_event_callback_;

    // 内部检查方法
    bool checkMarginRequirements(const std::string& symbol, double required_margin);
    bool checkDrawdownLimit(const std::string& symbol, double potential_loss);
    bool checkLeverageLimits(const std::string& symbol);
    bool checkPositionLimits(const std::string& symbol, double size);
    bool checkFundingRateExposure(const std::string& symbol, double funding_rate, double size);
    bool checkVolatility(const std::string& symbol);
    bool checkTradingFrequency(const std::string& symbol);

    // 内部计算方法
    double calculateRequiredMargin(const std::string& symbol, double size);
    double calculateDrawdown() const;
    double calculatePnL(const std::chrono::system_clock::time_point& start_time);
    
    // 事件和更新处理
    void recordRiskEvent(RiskEventType type, const std::string& symbol, 
                        double current_value, double threshold_value);
    void updateMetrics(const std::string& symbol);
    void updatePositionRisk(const std::string& symbol);
    void processTradeUpdate(const std::string& symbol, const execution::TradeInfo& trade);
    
    // 紧急处理
    void handleEmergencyCase(const std::string& symbol, const RiskEvent& event);
    bool executeEmergencyActions(const std::string& symbol);
    
    // 辅助方法
    bool isWithinVolatilityThreshold(const std::string& symbol, double price);
    std::vector<std::string> getViolatedLimits() const;
    void cleanupOldEvents();
};

} // namespace risk
} // namespace trading
} // namespace funding_arbitrage