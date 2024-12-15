#include "monitor_service.h"
#include "prometheus_exporter.h"
#include <sys/resource.h>
#include <algorithm>

namespace funding_arbitrage {
namespace monitor {
namespace metrics {

MonitorService::MonitorService(
    const std::shared_ptr<config::Config>& config,
    std::shared_ptr<api::BinanceApi> api,
    std::shared_ptr<api::BinanceWebsocketClient> ws_client)
    : api_(std::move(api))
    , ws_client_(std::move(ws_client))
    , logger_(std::make_shared<utils::Logger>("MonitorService"))
    , running_(false) {
    
    auto monitor_config = config->getSubConfig("monitor");
    if (!monitor_config) {
        throw std::runtime_error("Missing monitor configuration");
    }

    // 加载基础配置
    config_.check_interval_ms = monitor_config->getInt("general.check_interval_ms", 1000);
    config_.memory_threshold = monitor_config->getDouble("general.memory_threshold_mb", 1000);
    config_.cpu_threshold = monitor_config->getDouble("general.cpu_threshold_percent", 80);
    config_.max_errors_before_unhealthy = 
        monitor_config->getInt("general.max_errors_before_unhealthy", 3);
    
    // 加载监控的交易对
    auto symbols = monitor_config->getStringArray("monitored_symbols");
    config_.monitored_symbols = std::vector<std::string>(symbols.begin(), symbols.end());

    // 初始化 Prometheus exporter
    if (monitor_config->getBool("prometheus.enabled", true)) {
        prometheus_exporter_ = std::make_unique<PrometheusExporter>(config, shared_from_this());
    }

    // 初始化系统状态
    system_status_.is_healthy = true;
    system_status_.last_check = std::chrono::system_clock::now();

    logger_->info("MonitorService initialized with " + 
                 std::to_string(config_.monitored_symbols.size()) + " symbols");
}

MonitorService::~MonitorService() {
    stop();
}

void MonitorService::start() {
    if (running_) {
        return;
    }

    running_ = true;
    
    // 启动 Prometheus exporter
    if (prometheus_exporter_) {
        prometheus_exporter_->start();
    }
    
    monitor_thread_ = std::thread(&MonitorService::monitorLoop, this);
    logger_->info("MonitorService started");
}

void MonitorService::stop() {
    if (!running_) {
        return;
    }

    running_ = false;
    
    if (prometheus_exporter_) {
        prometheus_exporter_->stop();
    }
    
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
    
    logger_->info("MonitorService stopped");
}

void MonitorService::monitorLoop() {
    while (running_) {
        try {
            updateMetrics();
            updateSystemStatus();
            checkThresholds();

            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.check_interval_ms)
            );
        } catch (const std::exception& e) {
            logger_->error("Error in monitor loop: " + std::string(e.what()));
        }
    }
}

void MonitorService::updateMetrics() {
    try {
        for (const auto& symbol : config_.monitored_symbols) {
            collectTradeMetrics(symbol);
            collectPositionMetrics(symbol);
        }
        
        collectSystemMetrics();
        
        // 更新 Prometheus 指标
        if (prometheus_exporter_) {
            prometheus_exporter_->updateMetrics();
        }
    } catch (const std::exception& e) {
        handleApiError("updateMetrics", e.what());
    }
}

void MonitorService::collectTradeMetrics(const std::string& symbol) {
    auto& metrics = metrics_[symbol];
    auto& ext_metrics = extended_metrics_[symbol];
    
    try {
        // 获取最近的交易历史
        auto trades = api_->getRecentTrades(symbol);
        
        // 计算交易量和成功率
        ext_metrics.trading_volume = 0;
        int successful_trades = 0;
        
        for (const auto& trade : trades) {
            ext_metrics.trading_volume += trade.quantity * trade.price;
            if (trade.is_maker) {  // 假设maker订单为成功交易
                successful_trades++;
            }
        }
        
        ext_metrics.order_success_rate = trades.empty() ? 0 : 
            static_cast<double>(successful_trades) / trades.size();
            
        // 更新基本指标
        metrics.total_trades = trades.size();
        metrics.successful_trades = successful_trades;
        metrics.win_rate = ext_metrics.order_success_rate;
        metrics.last_update = std::chrono::system_clock::now();
        
    } catch (const std::exception& e) {
        logger_->error("Error collecting trade metrics for " + symbol + 
                      ": " + std::string(e.what()));
    }
}

void MonitorService::collectSystemMetrics() {
    try {
        // 检查API延迟
        auto start = std::chrono::steady_clock::now();
        api_->getLastPrice("BTCUSDT");
        auto end = std::chrono::steady_clock::now();
        
        system_status_.api_latency = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - start).count();
            
        // 更新连接状态
        system_status_.api_connected = true;
        system_status_.ws_connected = ws_client_->isConnected();
        
        // 更新消息率和错误率
        system_status_.ws_message_rate = calculateMessageRate();
        system_status_.error_rate = calculateErrorRate();
        
        // 检查系统资源
        checkResourceUsage();
        
    } catch (const std::exception& e) {
        system_status_.api_connected = false;
        logger_->error("Error collecting system metrics: " + std::string(e.what()));
    }
}

void MonitorService::collectPositionMetrics(const std::string& symbol) {
    try {
        auto positions = api_->getOpenPositions();
        for (const auto& pos : positions) {
            if (pos.symbol == symbol) {
                auto& metrics = metrics_[symbol];
                
                // 更新仓位信息
                metrics.current_position_size = std::abs(pos.amount);
                metrics.unrealized_pnl = pos.unrealized_pnl;
                metrics.entry_price = pos.entry_price;
                metrics.liquidation_price = pos.liquidation_price;
                
                // 更新最大仓位
                metrics.max_position_size = std::max(
                    metrics.max_position_size,
                    metrics.current_position_size
                );
                
                // 计算风险指标
                calculateRiskMetrics(symbol, pos);
            }
        }
    } catch (const std::exception& e) {
        logger_->error("Error collecting position metrics for " + symbol + 
                      ": " + std::string(e.what()));
    }
}

// src/monitor/monitor_service.cpp (continued)

void MonitorService::calculateRiskMetrics(const std::string& symbol,
                                        const execution::PositionInfo& position) {
    auto& metrics = metrics_[symbol];
    
    try {
        // 计算距离强平价的百分比
        if (position.liquidation_price > 0) {
            double mark_price = api_->getMarkPrice(symbol);
            metrics.distance_to_liquidation = 
                std::abs(mark_price - position.liquidation_price) / mark_price;
        }

        // 计算当前回撤
        if (metrics.max_position_size > 0) {
            metrics.current_drawdown = 
                (metrics.max_position_size - metrics.current_position_size) / 
                metrics.max_position_size;
                
            metrics.max_drawdown = std::max(
                metrics.max_drawdown,
                metrics.current_drawdown
            );
        }

        // 更新盈亏指标
        auto& ext_metrics = extended_metrics_[symbol];
        auto now = std::chrono::system_clock::now();
        auto today_start = std::chrono::floor<std::chrono::days>(now);
        
        if (position.realized_pnl != 0) {
            if (now - today_start < std::chrono::hours(24)) {
                ext_metrics.daily_pnl += position.realized_pnl;
            }
            metrics.realized_pnl += position.realized_pnl;
        }
        
        metrics.total_pnl = metrics.realized_pnl + position.unrealized_pnl;
        
    } catch (const std::exception& e) {
        logger_->error("Error calculating risk metrics for " + symbol + 
                      ": " + std::string(e.what()));
    }
}

void MonitorService::checkResourceUsage() {
    rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        // 内存使用（MB）
        system_status_.memory_usage = 
            static_cast<double>(usage.ru_maxrss) / 1024;

        // CPU使用率计算
        static auto last_check_time = std::chrono::steady_clock::now();
        static auto last_cpu_usage = usage.ru_utime;

        auto current_time = std::chrono::steady_clock::now();
        auto time_diff = std::chrono::duration_cast<std::chrono::microseconds>(
            current_time - last_check_time
        ).count();

        if (time_diff > 0) {
            auto cpu_diff = usage.ru_utime.tv_sec * 1000000 + usage.ru_utime.tv_usec
                           - (last_cpu_usage.tv_sec * 1000000 + last_cpu_usage.tv_usec);
            
            system_status_.cpu_usage = (cpu_diff * 100.0) / time_diff;
        }

        last_check_time = current_time;
        last_cpu_usage = usage.ru_utime;
    }
}

void MonitorService::checkThresholds() {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    for (const auto& [symbol, metrics] : metrics_) {
        // 检查仓位大小
        if (auto it = alert_thresholds_.find("position_size"); 
            it != alert_thresholds_.end() && 
            metrics.current_position_size > it->second) {
            triggerAlert(symbol, "Position size exceeded threshold: " + 
                        std::to_string(metrics.current_position_size));
        }
        
        // 检查回撤
        if (auto it = alert_thresholds_.find("drawdown");
            it != alert_thresholds_.end() && 
            metrics.current_drawdown > it->second) {
            triggerAlert(symbol, "Drawdown exceeded threshold: " + 
                        std::to_string(metrics.current_drawdown));
        }
        
        // 检查强平距离
        if (auto it = alert_thresholds_.find("liquidation_distance");
            it != alert_thresholds_.end() && 
            metrics.distance_to_liquidation < it->second) {
            triggerAlert(symbol, "Position too close to liquidation price: " + 
                        std::to_string(metrics.distance_to_liquidation));
        }
    }

    // 检查系统指标
    if (system_status_.memory_usage > config_.memory_threshold) {
        triggerAlert("System", "Memory usage exceeded threshold: " + 
                    std::to_string(system_status_.memory_usage) + "MB");
    }
    
    if (system_status_.cpu_usage > config_.cpu_threshold) {
        triggerAlert("System", "CPU usage exceeded threshold: " + 
                    std::to_string(system_status_.cpu_usage) + "%");
    }
    
    if (system_status_.error_rate > 0.01) {  // 1% error rate threshold
        triggerAlert("System", "High error rate detected: " + 
                    std::to_string(system_status_.error_rate));
    }
}

void MonitorService::triggerAlert(const std::string& source, 
                                const std::string& message) {
    if (alert_callback_) {
        try {
            alert_callback_(source, message);
        } catch (const std::exception& e) {
            logger_->error("Error in alert callback: " + std::string(e.what()));
        }
    }
    logger_->warn("Alert: [" + source + "] " + message);
}

double MonitorService::calculateMessageRate() const {
    // 计算每秒消息处理率
    static std::atomic<int64_t> message_count{0};
    static auto last_check = std::chrono::steady_clock::now();
    static double last_rate = 0.0;

    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_check).count();

    if (duration >= 1) {
        last_rate = static_cast<double>(message_count) / duration;
        message_count = 0;
        last_check = now;
    }

    return last_rate;
}

double MonitorService::calculateErrorRate() const {
    // 计算错误率
    static std::atomic<int64_t> total_requests{0};
    static std::atomic<int64_t> error_count{0};
    static auto last_check = std::chrono::steady_clock::now();
    static double last_rate = 0.0;

    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_check).count();

    if (duration >= 1) {
        if (total_requests > 0) {
            last_rate = static_cast<double>(error_count) / total_requests;
        }
        total_requests = 0;
        error_count = 0;
        last_check = now;
    }

    return last_rate;
}

void MonitorService::handleApiError(const std::string& operation, 
                                  const std::string& error) {
    logger_->error(operation + " error: " + error);
    system_status_.last_error = error;
}

bool MonitorService::calculateSystemHealth() {
    return system_status_.api_connected && 
           system_status_.ws_connected &&
           system_status_.memory_usage < config_.memory_threshold &&
           system_status_.cpu_usage < config_.cpu_threshold &&
           system_status_.error_rate < 0.01;  // 1% error rate threshold
}

MonitorMetrics MonitorService::getMetrics(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    auto it = metrics_.find(symbol);
    if (it != metrics_.end()) {
        return it->second;
    }
    return MonitorMetrics{};
}

std::vector<MonitorMetrics> MonitorService::getAllMetrics() const {
    std::vector<MonitorMetrics> result;
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    result.reserve(metrics_.size());
    for (const auto& [symbol, metrics] : metrics_) {
        result.push_back(metrics);
    }
    return result;
}

void MonitorService::setAlertCallback(
    std::function<void(const std::string&, const std::string&)> callback) {
    alert_callback_ = std::move(callback);
}

void MonitorService::setMetricsThreshold(const std::string& metric_name, 
                                       double threshold) {
    alert_thresholds_[metric_name] = threshold;
}

bool MonitorService::isHealthy() const {
    return system_status_.is_healthy;
}

std::string MonitorService::getLastError() const {
    return system_status_.last_error;
}

} // namespace metrics
} // namespace monitor
} // namespace funding_arbitrage