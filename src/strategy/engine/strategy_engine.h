#pragma once
#include "strategy/types/strategy_types.h"
#include "market/api/binance_api.h"
#include "trading/execution/order_manager.h"
#include "trading/position/position_manager.h"
#include "trading/risk/risk_manager.h"
#include "monitor/alerts/alert_manager.h"
#include "common/logger/logger.h"
#include <memory>
#include <map>
#include <set>

namespace funding_arbitrage {
namespace strategy {

class StrategyEngine {
public:
    StrategyEngine(
        const std::shared_ptr<config::Config>& config,
        std::shared_ptr<market::api::BinanceApi> api,
        std::shared_ptr<trading::execution::OrderManager> order_manager,
        std::shared_ptr<trading::position::PositionManager> position_manager,
        std::shared_ptr<trading::risk::RiskManager> risk_manager,
        std::shared_ptr<monitor::alerts::AlertManager> alert_manager
    );

    virtual ~StrategyEngine() = default;

    // 策略控制
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;

    // 策略状态
    virtual std::vector<PositionInfo> getPositions() const = 0;
    virtual std::vector<Signal> getSignals() const = 0;

protected:
    std::shared_ptr<market::api::BinanceApi> api_;
    std::shared_ptr<trading::execution::OrderManager> order_manager_;
    std::shared_ptr<trading::position::PositionManager> position_manager_;
    std::shared_ptr<trading::risk::RiskManager> risk_manager_;
    std::shared_ptr<monitor::alerts::AlertManager> alert_manager_;
    std::shared_ptr<Logger> logger_;
    
    // 运行状态
    std::atomic<bool> running_{false};
    std::thread strategy_thread_;

    // 内部方法
    virtual void runStrategy() = 0;
    virtual void processSignals() = 0;
    virtual void managePositions() = 0;
};

} // namespace strategy
} // namespace funding_arbitrage