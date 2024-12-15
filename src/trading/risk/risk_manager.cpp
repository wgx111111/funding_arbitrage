// src/risk/risk_manager.cpp
#include "risk_manager.h"
#include <algorithm>
#include <numeric>
#include <cmath>

namespace funding_arbitrage {
namespace trading {
namespace risk {

RiskManager::RiskManager(
    const std::shared_ptr<config::Config>& config,
    std::shared_ptr<api::BinanceApi> api)
    : api_(std::move(api))
    , logger_(std::make_shared<utils::Logger>("RiskManager"))
    , emergency_mode_(false) {
    
    auto risk_config = config->getSubConfig("risk");
    if (!risk_config) {
        throw std::runtime_error("Missing risk configuration");
    }

    // 初始化风险限制
    limits_.max_position_size = risk_config->getDouble("limits.max_position_size", 1.0);
    limits_.max_total_positions = risk_config->getDouble("limits.max_total_positions", 3.0);
    limits_.max_leverage = risk_config->getDouble("limits.max_leverage", 20.0);
    limits_.max_drawdown = risk_config->getDouble("limits.max_drawdown", 0.1);
    limits_.max_daily_loss = risk_config->getDouble("limits.max_daily_loss", 0.05);
    limits_.max_hourly_loss = risk_config->getDouble("limits.max_hourly_loss", 0.02);
    limits_.min_margin_ratio = risk_config->getDouble("limits.min_margin_ratio", 0.05);
    limits_.max_funding_exposure = risk_config->getDouble("limits.max_funding_exposure", 0.01);
    limits_.max_trades_per_hour = risk_config->getInt("limits.max_trades_per_hour", 30);
    limits_.price_deviation_threshold = risk_config->getDouble("limits.price_deviation_threshold", 0.003);

    // 初始化风控设置
    control_settings_.auto_reduce_position = risk_config->getBool("control.auto_reduce_position", true);
    control_settings_.auto_adjust_leverage = risk_config->getBool("control.auto_adjust_leverage", true);
    control_settings_.auto_reduce_threshold = risk_config->getDouble("control.auto_reduce_threshold", 0.8);
    control_settings_.position_reduction_ratio = risk_config->getDouble("control.position_reduction_ratio", 0.5);
    control_settings_.max_retries = risk_config->getInt("control.max_retries", 3);
    control_settings_.retry_delay = std::chrono::milliseconds(
        risk_config->getInt("control.retry_delay_ms", 1000)
    );

    logger_->info("RiskManager initialized with maxPositionSize=" + 
                 std::to_string(limits_.max_position_size));
}

bool RiskManager::checkNewPosition(const std::string& symbol, 
                                 double size, 
                                 double funding_rate) {
    if (emergency_mode_) {
        recordRiskEvent(RiskEventType::POSITION_LIMIT_BREACH, symbol, size, 0.0);
        return false;
    }

    try {
        // 基础检查
        if (!checkPositionLimits(symbol, size)) {
            return false;
        }

        // 检查保证金要求
        double required_margin = calculateRequiredMargin(symbol, size);
        if (!checkMarginRequirements(symbol, required_margin)) {
            return false;
        }

        // 检查资金费率敞口
        if (!checkFundingRateExposure(symbol, funding_rate, size)) {
            return false;
        }

        // 检查价格波动性
        if (!checkVolatility(symbol)) {
            return false;
        }

        // 检查交易频率
        if (!checkTradingFrequency(symbol)) {
            return false;
        }

        // 更新风险指标
        updateMetrics(symbol);
        return true;

    } catch (const std::exception& e) {
        logger_->error("Error in position check: " + std::string(e.what()));
        return false;
    }
}

bool RiskManager::checkPositionLimits(const std::string& symbol, double size) {
    try {
        // 检查单个仓位限制
        if (size > limits_.max_position_size) {
            recordRiskEvent(RiskEventType::POSITION_LIMIT_BREACH, 
                          symbol, size, limits_.max_position_size);
            return false;
        }

        // 检查总仓位限制
        double total_positions = size;
        for (const auto& [sym, risk] : position_risks_) {
            if (sym != symbol) {
                total_positions += std::abs(risk.size);
            }
        }

        if (total_positions > limits_.max_total_positions) {
            recordRiskEvent(RiskEventType::POSITION_LIMIT_BREACH,
                          symbol, total_positions, limits_.max_total_positions);
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        logger_->error("Error checking position limits: " + std::string(e.what()));
        return false;
    }
}

void RiskManager::recordRiskEvent(RiskEventType type, 
                                const std::string& symbol,
                                double current_value, 
                                double threshold_value) {
    RiskEvent event{
        .type = type,
        .symbol = symbol,
        .current_value = current_value,
        .threshold_value = threshold_value,
        .time = std::chrono::system_clock::now()
    };

    // 设置事件消息
    switch (type) {
        case RiskEventType::MARGIN_CALL:
            event.message = "Margin ratio below minimum requirement";
            break;
        case RiskEventType::LIQUIDATION_WARNING:
            event.message = "Position approaching liquidation price";
            break;
        case RiskEventType::DRAWDOWN_LIMIT_BREACH:
            event.message = "Drawdown limit exceeded";
            break;
        // ... 其他事件类型的消息
    }

    // 记录事件
    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        recent_events_.push(event);
        cleanupOldEvents();
    }

    // 触发回调
    if (risk_event_callback_) {
        try {
            risk_event_callback_(event);
        } catch (const std::exception& e) {
            logger_->error("Error in risk event callback: " + std::string(e.what()));
        }
    }

    // 检查是否需要紧急处理
    if (shouldTriggerEmergency(type)) {
        handleEmergencyCase(symbol, event);
    }
}


void RiskManager::updateMetrics(const std::string& symbol) {
    try {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        
        // 更新基础指标
        auto positions = api_->getOpenPositions();
        current_metrics_.total_position_value = 0;
        current_metrics_.largest_position_size = 0;

        for (const auto& pos : positions) {
            updatePositionRisk(pos.symbol);
            double pos_value = std::abs(pos.amount * pos.mark_price);
            current_metrics_.total_position_value += pos_value;
            current_metrics_.largest_position_size = 
                std::max(current_metrics_.largest_position_size, pos_value);
        }

        // 更新PnL指标
        current_metrics_.daily_pnl = calculatePnL(
            std::chrono::system_clock::now() - std::chrono::hours(24)
        );
        current_metrics_.hourly_pnl = calculatePnL(
            std::chrono::system_clock::now() - std::chrono::hours(1)
        );

        // 更新回撤
        current_metrics_.current_drawdown = calculateDrawdown();
        
        // 检查是否违反任何限制
        checkMetricsThresholds();
        
        current_metrics_.last_update = std::chrono::system_clock::now();

    } catch (const std::exception& e) {
        logger_->error("Error updating metrics: " + std::string(e.what()));
    }
}

void RiskManager::updatePositionRisk(const std::string& symbol) {
    try {
        auto& risk = position_risks_[symbol];
        risk.symbol = symbol;

        // 获取当前价格和仓位信息
        double mark_price = api_->getMarkPrice(symbol);
        auto positions = api_->getOpenPositions();
        
        for (const auto& pos : positions) {
            if (pos.symbol == symbol) {
                risk.size = pos.amount;
                risk.entry_price = pos.entry_price;
                risk.current_price = mark_price;
                risk.unrealized_pnl = pos.unrealized_pnl;
                risk.liquidation_price = pos.liquidation_price;
                risk.margin_ratio = pos.margin / (pos.amount * mark_price);
                risk.leverage = pos.leverage;
                risk.funding_rate = api_->getFundingRate(symbol);
                risk.funding_fee = calculateFundingFee(risk);
                
                // 检查是否接近强平价
                checkLiquidationRisk(risk);
                break;
            }
        }
    } catch (const std::exception& e) {
        logger_->error("Error updating position risk for " + symbol + 
                      ": " + std::string(e.what()));
    }
}

void RiskManager::checkMetricsThresholds() {
    // 检查回撤限制
    if (current_metrics_.current_drawdown > limits_.max_drawdown) {
        recordRiskEvent(RiskEventType::DRAWDOWN_LIMIT_BREACH,
                       "GLOBAL",
                       current_metrics_.current_drawdown,
                       limits_.max_drawdown);
    }

    // 检查日损失限制
    if (current_metrics_.daily_pnl < -limits_.max_daily_loss) {
        recordRiskEvent(RiskEventType::DAILY_LOSS_LIMIT_BREACH,
                       "GLOBAL",
                       std::abs(current_metrics_.daily_pnl),
                       limits_.max_daily_loss);
    }

    // 检查小时损失限制
    if (current_metrics_.hourly_pnl < -limits_.max_hourly_loss) {
        recordRiskEvent(RiskEventType::DAILY_LOSS_LIMIT_BREACH,
                       "GLOBAL",
                       std::abs(current_metrics_.hourly_pnl),
                       limits_.max_hourly_loss);
    }
}

void RiskManager::checkLiquidationRisk(const PositionRisk& risk) {
    if (risk.liquidation_price <= 0) {
        return;
    }

    double price_to_liquidation = std::abs(risk.current_price - risk.liquidation_price);
    double liquidation_distance = price_to_liquidation / risk.current_price;

    if (liquidation_distance < 0.05) {  // 5% buffer to liquidation
        recordRiskEvent(RiskEventType::LIQUIDATION_WARNING,
                       risk.symbol,
                       liquidation_distance,
                       0.05);

        if (control_settings_.auto_reduce_position) {
            executeEmergencyActions(risk.symbol);
        }
    }
}

bool RiskManager::executeEmergencyActions(const std::string& symbol) {
    try {
        auto& risk = position_risks_[symbol];
        
        // 自动减仓
        if (control_settings_.auto_reduce_position) {
            double reduction_size = std::abs(risk.size * control_settings_.position_reduction_ratio);
            if (reduction_size > 0) {
                // 创建市价减仓订单
                execution::OrderRequest request{
                    .symbol = symbol,
                    .side = risk.size > 0 ? execution::OrderSide::SELL : execution::OrderSide::BUY,
                    .type = execution::OrderType::MARKET,
                    .quantity = reduction_size,
                    .reduce_only = true
                };

                api_->placeOrder(request);
                logger_->info("Emergency position reduction executed for " + symbol);
            }
        }

        // 调整杠杆
        if (control_settings_.auto_adjust_leverage && risk.leverage > 1) {
            int new_leverage = std::max(1, risk.leverage / 2);
            api_->setLeverage(symbol, new_leverage);
            logger_->info("Emergency leverage reduction executed for " + symbol);
        }

        return true;
    } catch (const std::exception& e) {
        logger_->error("Failed to execute emergency actions: " + std::string(e.what()));
        return false;
    }
}

RiskReport RiskManager::generateRiskReport(const std::string& symbol) {
    RiskReport report;
    report.symbol = symbol;
    report.report_time = std::chrono::system_clock::now();

    try {
        // 获取当前指标
        {
            std::lock_guard<std::mutex> lock(metrics_mutex_);
            report.metrics = current_metrics_;
        }

        // 获取近期事件
        auto recent_events = getRecentEvents();
        for (const auto& event : recent_events) {
            if (event.symbol == symbol || event.symbol == "GLOBAL") {
                report.recent_events.push_back(event);
            }
        }

        // 添加警告信息
        auto violations = getViolatedLimits();
        report.warnings.insert(report.warnings.end(), 
                             violations.begin(), violations.end());

        // 生成建议
        generateRiskRecommendations(report);

        return report;
    } catch (const std::exception& e) {
        logger_->error("Error generating risk report: " + std::string(e.what()));
        throw;
    }
}

void RiskManager::generateRiskRecommendations(RiskReport& report) {
    const auto& risk = position_risks_[report.symbol];
    
    // 基于杠杆的建议
    if (risk.leverage > limits_.max_leverage * 0.8) {
        report.recommendations.push_back(
            "Consider reducing leverage to decrease liquidation risk"
        );
    }

    // 基于回撤的建议
    if (current_metrics_.current_drawdown > limits_.max_drawdown * 0.7) {
        report.recommendations.push_back(
            "Consider reducing position size to manage drawdown risk"
        );
    }

    // 基于资金费率的建议
    if (std::abs(risk.funding_rate) > limits_.max_funding_exposure * 0.8) {
        report.recommendations.push_back(
            "High funding rate exposure - consider adjusting position before next funding"
        );
    }

    // 基于波动性的建议
    if (!isWithinVolatilityThreshold(report.symbol, risk.current_price)) {
        report.recommendations.push_back(
            "High market volatility - consider reducing position size"
        );
    }
}

void RiskManager::cleanupOldEvents() {
    auto now = std::chrono::system_clock::now();
    while (!recent_events_.empty()) {
        const auto& event = recent_events_.front();
        if (now - event.time > std::chrono::hours(24)) {
            recent_events_.pop();
        } else {
            break;
        }
    }
}

void RiskManager::setRiskControlSettings(const RiskControlSettings& settings) {
    control_settings_ = settings;
    logger_->info("Risk control settings updated");
}

void RiskManager::registerRiskEventCallback(
    std::function<void(const RiskEvent&)> callback) {
    risk_event_callback_ = std::move(callback);
}

} // namespace risk
} // namespace trading
} // namespace funding_arbitrage