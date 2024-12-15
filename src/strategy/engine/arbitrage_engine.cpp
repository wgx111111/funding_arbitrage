#include "funding_arbitrage_engine.h"
#include <algorithm>
#include <numeric>

namespace funding_arbitrage {
namespace strategy {

FundingArbitrageEngine::FundingArbitrageEngine(
    const std::shared_ptr<config::Config>& config,
    std::shared_ptr<market::api::BinanceApi> api,
    std::shared_ptr<trading::execution::OrderManager> order_manager,
    std::shared_ptr<trading::risk::RiskManager> risk_manager,
    std::shared_ptr<monitor::alerts::AlertManager> alert_manager)
    : api_(std::move(api))
    , order_manager_(std::move(order_manager))
    , risk_manager_(std::move(risk_manager))
    , alert_manager_(std::move(alert_manager))
    , logger_(std::make_shared<utils::Logger>("FundingArbitrageEngine"))
    , running_(false) {

    // 加载策略参数
    auto strategy_config = config->getSubConfig("strategy.funding_arbitrage");
    params_.top_n_instruments = strategy_config->getInt("top_n_instruments", 5);
    params_.min_basis_ratio = strategy_config->getDouble("min_basis_ratio", 0.0008);
    params_.min_funding_rate = strategy_config->getDouble("min_funding_rate", 0.0001);
    params_.pre_funding_minutes = strategy_config->getInt("pre_funding_minutes", 60);
    params_.position_size_usd = strategy_config->getDouble("position_size_usd", 1000.0);
    
    // 加载风控参数
    params_.max_position_per_symbol = strategy_config->getDouble("max_position_per_symbol", 0.1);
    params_.max_total_position = strategy_config->getDouble("max_total_position", 0.5);
    params_.min_liquidity_score = strategy_config->getDouble("min_liquidity_score", 0.7);
    params_.max_spread_ratio = strategy_config->getDouble("max_spread_ratio", 0.001);
    
    logger_->info("FundingArbitrageEngine initialized");
}

void FundingArbitrageEngine::start() {
    if (running_) {
        return;
    }

    running_ = true;
    strategy_thread_ = std::thread(&FundingArbitrageEngine::runStrategy, this);
    logger_->info("Strategy started");
}

void FundingArbitrageEngine::runStrategy() {
    while (running_) {
        try {
            // 更新状态
            updateState();

            // 检查是否在交易窗口期
            if (checkTradingWindow()) {
                // 选择交易标的
                auto instruments = selectInstruments();
                
                for (const auto& instrument : instruments) {
                    if (validateInstrument(instrument)) {
                        // 计算开仓大小
                        double size = calculateOptimalSize(instrument);
                        if (size > 0 && checkPositionLimits(instrument.symbol, size)) {
                            executePairTrade(instrument, size);
                        }
                    }
                }
            } else {
                // 不在交易窗口期，监控并平仓
                monitorPositions();
            }

            // 更新指标
            updateMetrics();
            
            // 等待下一个检查周期
            std::this_thread::sleep_for(std::chrono::seconds(5));

        } catch (const std::exception& e) {
            logger_->error("Strategy error: " + std::string(e.what()));
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
}

std::vector<InstrumentInfo> FundingArbitrageEngine::selectInstruments() {
    std::vector<InstrumentInfo> instruments;
    try {
        auto symbols = api_->getAllSymbols();
        
        for (const auto& symbol : symbols) {
            InstrumentInfo info;
            info.symbol = symbol;
            info.spot_price = api_->getSpotPrice(symbol);
            info.futures_price = api_->getFuturesPrice(symbol);
            info.funding_rate = api_->getFundingRate(symbol);
            info.next_funding_time = api_->getNextFundingTime(symbol);
            info.volume_24h = api_->get24hVolume(symbol);
            info.bid_ask_spread = api_->getBidAskSpread(symbol);
            
            // 计算基差率
            info.basis = (info.futures_price - info.spot_price) / info.spot_price;
            
            // 计算流动性评分
            info.liquidity_score = calculateLiquidityScore(info);
            
            instruments.push_back(info);
        }

        // 按资金费率绝对值排序
        std::sort(instruments.begin(), instruments.end(),
            [](const InstrumentInfo& a, const InstrumentInfo& b) {
                return std::abs(a.funding_rate) > std::abs(b.funding_rate);
            });

        // 只保留前N个
        if (instruments.size() > params_.top_n_instruments) {
            instruments.resize(params_.top_n_instruments);
        }

    } catch (const std::exception& e) {
        logger_->error("Error selecting instruments: " + std::string(e.what()));
    }
    return instruments;
}

bool FundingArbitrageEngine::checkTradingWindow() const {
    try {
        auto now = std::chrono::system_clock::now();
        
        // 检查每个活跃标的的下次资金费率时间
        for (const auto& instrument : state_.active_instruments) {
            auto time_to_funding = std::chrono::duration_cast<std::chrono::minutes>(
                instrument.next_funding_time - now).count();
                
            if (time_to_funding <= params_.pre_funding_minutes &&
                time_to_funding > 0) {
                return true;
            }
        }
    } catch (const std::exception& e) {
        logger_->error("Error checking trading window: " + std::string(e.what()));
    }
    return false;
}

void FundingArbitrageEngine::executePairTrade(
    const InstrumentInfo& instrument, double size) {
    try {
        logger_->info("Executing pair trade for " + instrument.symbol + 
                     " size: " + std::to_string(size));

        // 检查基差盈利机会
        double fee_cost = calculateTotalFees(instrument, size);
        double basis_profit = std::abs(instrument.basis) * size * instrument.spot_price;
        
        if (basis_profit <= fee_cost) {
            logger_->debug("Insufficient basis profit for " + instrument.symbol);
            return;
        }

        // 确定交易方向
        bool long_spot = instrument.futures_price > instrument.spot_price;
        
        // 使用TWAP执行
        if (params_.use_twap) {
            // 同时执行现货和合约
            auto spot_future = std::async(std::launch::async, [&]() {
                executeTwapOrder(instrument.symbol, size, true, long_spot);
            });
            
            auto futures_future = std::async(std::launch::async, [&]() {
                executeTwapOrder(instrument.symbol, size, false, !long_spot);
            });
            
            spot_future.wait();
            futures_future.wait();
        } else {
            // 普通市价单执行
            executeSingleOrder(instrument.symbol, size, true, long_spot);
            executeSingleOrder(instrument.symbol, size, false, !long_spot);
        }

        // 检查持仓平衡
        balancePositions(instrument.symbol);
        
        // 更新状态
        updateState();
        
        logger_->info("Pair trade executed successfully for " + instrument.symbol);

    } catch (const std::exception& e) {
        logger_->error("Failed to execute pair trade: " + std::string(e.what()));
        handleExecutionError(instrument.symbol, e.what());
    }
}

void FundingArbitrageEngine::monitorPositions() {
    try {
        std::lock_guard<std::mutex> lock(state_mutex_);
        
        for (const auto& [symbol, spot_size] : state_.spot_positions) {
            auto instrument = std::find_if(
                state_.active_instruments.begin(),
                state_.active_instruments.end(),
                [&](const InstrumentInfo& info) { return info.symbol == symbol; }
            );

            if (instrument != state_.active_instruments.end()) {
                // 检查是否应该平仓
                bool should_close = false;
                
                // 资金费率结算后平仓
                auto now = std::chrono::system_clock::now();
                if (now > instrument->next_funding_time) {
                    should_close = true;
                }
                
                // 止盈止损检查
                double unrealized_pnl = calculateUnrealizedPnL(*instrument);
                if (unrealized_pnl / params_.position_size_usd >= params_.profit_take_ratio ||
                    unrealized_pnl / params_.position_size_usd <= -params_.stop_loss_ratio) {
                    should_close = true;
                }

                // 执行平仓
                if (should_close) {
                    closePositions(symbol);
                }
            }
        }
    } catch (const std::exception& e) {
        logger_->error("Error monitoring positions: " + std::string(e.what()));
    }
}

bool FundingArbitrageEngine::validateInstrument(const InstrumentInfo& instrument) const {
    try {
        // 检查最小资金费率
        if (std::abs(instrument.funding_rate) < params_.min_funding_rate) {
            logger_->debug(instrument.symbol + " funding rate too low: " + 
                         std::to_string(instrument.funding_rate));
            return false;
        }

        // 检查基差率
        if (std::abs(instrument.basis) < params_.min_basis_ratio) {
            logger_->debug(instrument.symbol + " basis too low: " + 
                         std::to_string(instrument.basis));
            return false;
        }

        // 检查流动性
        if (!checkLiquidity(instrument)) {
            logger_->debug(instrument.symbol + " failed liquidity check");
            return false;
        }

        // 检查买卖价差
        if (instrument.bid_ask_spread / instrument.spot_price > params_.max_spread_ratio) {
            logger_->debug(instrument.symbol + " spread too high: " + 
                         std::to_string(instrument.bid_ask_spread));
            return false;
        }

        // 检查24小时成交量
        if (instrument.volume_24h * instrument.spot_price < params_.min_volume_usd) {
            logger_->debug(instrument.symbol + " volume too low: " + 
                         std::to_string(instrument.volume_24h));
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        logger_->error("Error validating instrument: " + std::string(e.what()));
        return false;
    }
}

bool FundingArbitrageEngine::checkLiquidity(const InstrumentInfo& instrument) const {
    try {
        // 获取订单簿深度
        auto spot_depth = api_->getOrderBookDepth(instrument.symbol, true);
        auto futures_depth = api_->getOrderBookDepth(instrument.symbol, false);

        double target_value = params_.position_size_usd;
        double spot_liquidity = 0;
        double futures_liquidity = 0;

        // 计算流动性深度
        for (const auto& level : spot_depth) {
            spot_liquidity += level.price * level.quantity;
            if (spot_liquidity >= target_value * 3) break;  // 3倍覆盖
        }

        for (const auto& level : futures_depth) {
            futures_liquidity += level.price * level.quantity;
            if (futures_liquidity >= target_value * 3) break;
        }

        // 检查最小流动性要求
        return spot_liquidity >= target_value * 3 && 
               futures_liquidity >= target_value * 3;

    } catch (const std::exception& e) {
        logger_->error("Error checking liquidity: " + std::string(e.what()));
        return false;
    }
}

bool FundingArbitrageEngine::checkMarketImpact(
    const InstrumentInfo& instrument, double size) const {
    try {
        // 获取最近的成交历史
        auto trades = api_->getRecentTrades(
            instrument.symbol, 
            std::chrono::minutes(params_.min_market_impact_minutes)
        );

        // 计算平均成交量
        double avg_trade_size = 0;
        if (!trades.empty()) {
            double total_volume = std::accumulate(trades.begin(), trades.end(), 0.0,
                [](double sum, const auto& trade) {
                    return sum + trade.quantity;
                });
            avg_trade_size = total_volume / trades.size();
        }

        // 检查订单大小是否会造成显著市场冲击
        return size <= avg_trade_size * 3;  // 不超过平均成交量的3倍

    } catch (const std::exception& e) {
        logger_->error("Error checking market impact: " + std::string(e.what()));
        return false;
    }
}

double FundingArbitrageEngine::calculateOptimalSize(
    const InstrumentInfo& instrument) const {
    try {
        // 基础仓位大小
        double base_size = params_.position_size_usd / instrument.spot_price;
        
        // 考虑流动性限制
        double max_size_by_liquidity = calculateLiquidityConstrainedSize(instrument);
        base_size = std::min(base_size, max_size_by_liquidity);

        // 考虑仓位限制
        double available_position = params_.max_position_per_symbol * 
            state_.total_equity / instrument.spot_price;
        base_size = std::min(base_size, available_position);

        // 考虑市场冲击
        if (!checkMarketImpact(instrument, base_size)) {
            double reduced_size = base_size * 0.5;  // 减小50%
            while (reduced_size > base_size * 0.1 && 
                   !checkMarketImpact(instrument, reduced_size)) {
                reduced_size *= 0.5;
            }
            base_size = reduced_size;
        }

        // 确保最小交易规模
        if (base_size * instrument.spot_price < 100) {  // 最小100USD
            return 0;
        }

        return base_size;
    } catch (const std::exception& e) {
        logger_->error("Error calculating optimal size: " + std::string(e.what()));
        return 0;
    }
}

void FundingArbitrageEngine::executeTwapOrder(
    const std::string& symbol, double total_size, bool is_spot, bool is_buy) {
    try {
        double size_per_order = total_size / params_.twap_intervals;
        
        for (int i = 0; i < params_.twap_intervals; ++i) {
            // 创建子订单
            trading::execution::OrderRequest request{
                .symbol = symbol,
                .side = is_buy ? trading::OrderSide::BUY : trading::OrderSide::SELL,
                .type = trading::OrderType::LIMIT,
                .quantity = size_per_order,
                .is_spot = is_spot
            };

            // 获取当前最优价格
            double current_price = is_buy ? 
                api_->getBestAskPrice(symbol, is_spot) :
                api_->getBestBidPrice(symbol, is_spot);

            // 添加滑点容忍度
            request.price = is_buy ? 
                current_price * (1 + params_.max_slippage) :
                current_price * (1 - params_.max_slippage);

            // 执行订单
            std::string order_id = order_manager_->placeOrder(request);

            // 等待订单执行
            if (!waitForExecution(order_id, symbol, size_per_order)) {
                throw std::runtime_error("TWAP order execution failed");
            }

            // 间隔等待
            if (i < params_.twap_intervals - 1) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
    } catch (const std::exception& e) {
        logger_->error("TWAP execution error: " + std::string(e.what()));
        throw;
    }
}

bool FundingArbitrageEngine::waitForExecution(
    const std::string& order_id,
    const std::string& symbol,
    double expected_size) {
    
    auto start_time = std::chrono::steady_clock::now();
    while (true) {
        try {
            auto order_info = order_manager_->getOrderStatus(symbol, order_id);
            
            // 检查是否完全成交
            if (order_info.status == trading::OrderStatus::FILLED) {
                return true;
            }

            // 检查是否超时
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();
            
            if (elapsed > params_.execution_timeout_seconds) {
                // 取消订单
                order_manager_->cancelOrder(symbol, order_id);
                return false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } catch (const std::exception& e) {
            logger_->error("Error waiting for execution: " + std::string(e.what()));
            return false;
        }
    }
}

void FundingArbitrageEngine::balancePositions(const std::string& symbol) {
    try {
        double spot_position = state_.spot_positions[symbol];
        double futures_position = state_.futures_positions[symbol];
        
        double imbalance = std::abs(spot_position + futures_position);
        if (imbalance > params_.position_imbalance_tolerance) {
            logger_->warn("Position imbalance detected for " + symbol + 
                         ": " + std::to_string(imbalance));
            
            // 调整持仓以达到平衡
            double adjustment_size = imbalance / 2;
            if (spot_position > -futures_position) {
                // 减少现货或增加空头
                executeSingleOrder(symbol, adjustment_size, true, false);
            } else {
                // 减少空头或增加现货
                executeSingleOrder(symbol, adjustment_size, false, true);
            }
        }
    } catch (const std::exception& e) {
        logger_->error("Error balancing positions: " + std::string(e.what()));
    }
}

void FundingArbitrageEngine::closePositions(const std::string& symbol) {
    try {
        std::vector<std::string> symbols_to_close;
        if (symbol.empty()) {
            // 关闭所有仓位
            for (const auto& [sym, _] : state_.spot_positions) {
                symbols_to_close.push_back(sym);
            }
        } else {
            symbols_to_close.push_back(symbol);
        }

        for (const auto& sym : symbols_to_close) {
            logger_->info("Closing positions for " + sym);
            
            // 获取当前仓位
            double spot_size = state_.spot_positions[sym];
            double futures_size = state_.futures_positions[sym];

            if (std::abs(spot_size) > 0 || std::abs(futures_size) > 0) {
                // 使用TWAP平仓
                if (params_.use_twap) {
                    if (spot_size > 0) {
                        executeTwapOrder(sym, std::abs(spot_size), true, false);
                    }
                    if (futures_size > 0) {
                        executeTwapOrder(sym, std::abs(futures_size), false, false);
                    }
                } else {
                    // 市价平仓
                    if (spot_size > 0) {
                        executeSingleOrder(sym, std::abs(spot_size), true, false);
                    }
                    if (futures_size > 0) {
                        executeSingleOrder(sym, std::abs(futures_size), false, false);
                    }
                }

                // 更新状态
                state_.spot_positions.erase(sym);
                state_.futures_positions.erase(sym);
            }
        }
    } catch (const std::exception& e) {
        logger_->error("Error closing positions: " + std::string(e.what()));
        throw;
    }
}

void FundingArbitrageEngine::updateState() {
    try {
        std::lock_guard<std::mutex> lock(state_mutex_);

        // 更新交易窗口状态
        state_.is_pre_funding_window = checkTradingWindow();

        // 更新活跃交易标的
        if (state_.is_pre_funding_window) {
            state_.active_instruments = selectInstruments();
        }

        // 更新持仓信息
        auto positions = api_->getOpenPositions();
        state_.spot_positions.clear();
        state_.futures_positions.clear();

        for (const auto& pos : positions) {
            if (pos.is_spot) {
                state_.spot_positions[pos.symbol] = pos.amount;
            } else {
                state_.futures_positions[pos.symbol] = pos.amount;
            }
        }

        // 更新盈亏信息
        updatePnL();

        // 更新回撤
        updateDrawdown();

        // 检查是否需要触发风险控制
        checkRiskLimits();

    } catch (const std::exception& e) {
        logger_->error("Error updating state: " + std::string(e.what()));
    }
}

void FundingArbitrageEngine::updatePnL() {
    try {
        double total_unrealized_pnl = 0;
        double total_realized_pnl = 0;

        // 计算每个标的的盈亏
        for (const auto& [symbol, spot_size] : state_.spot_positions) {
            auto futures_size = state_.futures_positions[symbol];
            
            // 获取当前价格
            double spot_price = api_->getSpotPrice(symbol);
            double futures_price = api_->getFuturesPrice(symbol);

            // 计算未实现盈亏
            double spot_pnl = (spot_price - state_.entry_prices[symbol].spot) * spot_size;
            double futures_pnl = (futures_price - state_.entry_prices[symbol].futures) * futures_size;
            total_unrealized_pnl += spot_pnl + futures_pnl;
        }

        // 更新状态
        state_.total_pnl = total_realized_pnl + total_unrealized_pnl;

        // 记录每小时盈亏变化
        if (std::chrono::system_clock::now() - state_.last_pnl_update > std::chrono::hours(1)) {
            state_.hourly_pnl_history.push_back(state_.total_pnl);
            state_.last_pnl_update = std::chrono::system_clock::now();

            // 保持最近24小时的记录
            if (state_.hourly_pnl_history.size() > 24) {
                state_.hourly_pnl_history.pop_front();
            }
        }

    } catch (const std::exception& e) {
        logger_->error("Error updating PnL: " + std::string(e.what()));
    }
}

void FundingArbitrageEngine::updateDrawdown() {
    try {
        if (state_.hourly_pnl_history.empty()) {
            return;
        }

        double peak = *std::max_element(
            state_.hourly_pnl_history.begin(),
            state_.hourly_pnl_history.end()
        );

        if (peak > 0) {
            state_.current_drawdown = (peak - state_.total_pnl) / peak;
        }

        // 记录最大回撤
        if (state_.current_drawdown > state_.max_drawdown) {
            state_.max_drawdown = state_.current_drawdown;
            
            // 发送告警
            if (state_.max_drawdown > params_.max_drawdown) {
                alert_manager_->sendAlert({
                    .type = monitor::alerts::AlertType::DRAWDOWN_WARNING,
                    .level = monitor::alerts::AlertLevel::WARNING,
                    .message = "Maximum drawdown exceeded",
                    .current_value = state_.max_drawdown,
                    .threshold = params_.max_drawdown
                });
            }
        }

    } catch (const std::exception& e) {
        logger_->error("Error updating drawdown: " + std::string(e.what()));
    }
}

void FundingArbitrageEngine::checkRiskLimits() {
    try {
        bool should_reduce_exposure = false;

        // 检查回撤限制
        if (state_.current_drawdown > params_.max_drawdown) {
            should_reduce_exposure = true;
            logger_->warn("Drawdown limit exceeded: " + 
                         std::to_string(state_.current_drawdown));
        }

        // 检查总持仓限制
        double total_exposure = 0;
        for (const auto& [symbol, spot_size] : state_.spot_positions) {
            double spot_price = api_->getSpotPrice(symbol);
            total_exposure += std::abs(spot_size * spot_price);
        }

        if (total_exposure > params_.max_total_position * state_.total_equity) {
            should_reduce_exposure = true;
            logger_->warn("Total position limit exceeded: " + 
                         std::to_string(total_exposure));
        }

        // 如果需要降低风险敞口，逐步减仓
        if (should_reduce_exposure) {
            auto symbols = state_.spot_positions;
            for (const auto& [symbol, size] : symbols) {
                // 减仓50%
                double reduction_size = std::abs(size) * 0.5;
                closePartialPosition(symbol, reduction_size);
            }
        }

    } catch (const std::exception& e) {
        logger_->error("Error checking risk limits: " + std::string(e.what()));
    }
}

void FundingArbitrageEngine::closePartialPosition(
    const std::string& symbol, double size) {
    try {
        logger_->info("Closing partial position for " + symbol + 
                     ", size: " + std::to_string(size));

        // 使用TWAP减仓
        executeTwapOrder(symbol, size, true, false);
        executeTwapOrder(symbol, size, false, false);

        // 更新状态
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_.spot_positions[symbol] -= size;
        state_.futures_positions[symbol] -= size;

        // 如果仓位很小，完全平仓
        if (std::abs(state_.spot_positions[symbol]) < 0.0001) {
            state_.spot_positions.erase(symbol);
            state_.futures_positions.erase(symbol);
        }

    } catch (const std::exception& e) {
        logger_->error("Error in partial position close: " + std::string(e.what()));
        throw;
    }
}

} // namespace strategy
} // namespace funding_arbitrage