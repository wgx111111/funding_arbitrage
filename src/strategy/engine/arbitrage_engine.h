#pragma once
#include "strategy_engine.h"

namespace funding_arbitrage {
namespace strategy {

class ArbitrageEngine : public StrategyEngine {
public:
    ArbitrageEngine(
        const std::shared_ptr<config::Config>& config,
        std::shared_ptr<market::api::BinanceApi> api,
        std::shared_ptr<trading::execution::OrderManager> order_manager,
        std::shared_ptr<trading::position::PositionManager> position_manager,
        std::shared_ptr<trading::risk::RiskManager> risk_manager,
        std::shared_ptr<monitor::alerts::AlertManager> alert_manager
    );

    // StrategyEngine interface implementation
    void start() override;
    void stop() override;
    bool isRunning() const override;
    std::vector<PositionInfo> getPositions() const override;
    std::vector<Signal> getSignals() const override;

private:
    StrategyParams params_;
    std::map<std::string, PositionInfo> positions_;
    std::vector<Signal> recent_signals_;
    mutable std::mutex data_mutex_;

    // 策略主循环
    void runStrategy() override;
    void processSignals() override;
    void managePositions() override;

    // 信号生成
    std::vector<SymbolInfo> selectTopSymbols();
    Signal analyzeSymbol(const SymbolInfo& symbol_info);
    bool checkLiquidity(const std::string& symbol);
    bool isNearFunding(const SymbolInfo& symbol_info) const;

    // 交易执行
    bool openPosition(const Signal& signal);
    bool closePosition(const PositionInfo& position, const std::string& reason);
    double calculatePositionSize(const SymbolInfo& symbol_info) const;

    // 风险检查
    bool checkTradingRisks(const Signal& signal) const;
    bool validateSymbolInfo(const SymbolInfo& info) const;
    void monitorBasisRisk();
    void checkStopLoss();

    // 辅助方法
    SymbolInfo getSymbolInfo(const std::string& symbol);
    bool isTradingHour() const;
    void updatePositionInfo();
    void logTradeExecution(const std::string& action, 
                          const std::string& symbol,
                          double size,
                          double price,
                          const std::string& reason);
};

} // namespace strategy
} // namespace funding_arbitrage