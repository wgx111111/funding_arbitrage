#pragma once
#include <memory>
#include <string>
#include <map>
#include <chrono>
#include <mutex>
#include <vector>
#include <thread>
#include <atomic>
#include "../../common/logger/logger.h"
#include "../../common/config/config.h"
#include "../../market/types/execution_types.h"
#include "../../market/api/binance_api.h"
#include "../../market/api/websocket_client.h"

namespace funding_arbitrage {
namespace monitor {
namespace metrics {

// 基本监控指标
struct MonitorMetrics {
    std::string symbol;
    double total_pnl{0.0};
    double unrealized_pnl{0.0};
    double realized_pnl{0.0};
    double funding_earned{0.0};
    double fees_paid{0.0};
    int total_trades{0};
    int successful_trades{0};
    double win_rate{0.0};
    double max_drawdown{0.0};
    double current_drawdown{0.0};
    double max_position_size{0.0};
    double current_position_size{0.0};
    double entry_price{0.0};
    double liquidation_price{0.0};
    double distance_to_liquidation{0.0};
    std::chrono::system_clock::time_point last_update;
};

// 扩展指标
struct ExtendedMetrics {
    double trading_volume{0.0};
    double average_execution_time{0.0};
    double order_success_rate{0.0};
    int failed_order_count{0};
    std::chrono::system_clock::time_point last_trade_time;
    double daily_pnl{0.0};
    double weekly_pnl{0.0};
    double monthly_pnl{0.0};
};

// 系统状态
struct SystemStatus {
    bool is_healthy{true};
    bool api_connected{false};
    bool ws_connected{false};
    double memory_usage{0.0};
    double cpu_usage{0.0};
    double api_latency{0.0};
    double ws_message_rate{0.0};
    double error_rate{0.0};
    std::string last_error;
    std::chrono::system_clock::time_point last_check;
};

class MonitorService : public std::enable_shared_from_this<MonitorService> {
public:
    MonitorService(const std::shared_ptr<config::Config>& config,
                  std::shared_ptr<api::BinanceApi> api,
                  std::shared_ptr<api::BinanceWebsocketClient> ws_client);
    ~MonitorService();

    // 服务控制
    void start();
    void stop();
    bool isRunning() const { return running_; }

    // 监控数据获取
    MonitorMetrics getMetrics(const std::string& symbol) const;
    std::vector<MonitorMetrics> getAllMetrics() const;
    SystemStatus getSystemStatus() const;
    
    // 告警设置
    void setAlertCallback(std::function<void(const std::string&, const std::string&)> callback);
    void setMetricsThreshold(const std::string& metric_name, double threshold);

    // 状态查询
    bool isHealthy() const;
    std::string getLastError() const;

private:
    // 组件引用
    std::shared_ptr<api::BinanceApi> api_;
    std::shared_ptr<api::BinanceWebsocketClient> ws_client_;
    std::shared_ptr<utils::Logger> logger_;
    std::unique_ptr<class PrometheusExporter> prometheus_exporter_;

    // 运行状态
    std::atomic<bool> running_;
    std::thread monitor_thread_;
    
    // 监控数据
    mutable std::mutex metrics_mutex_;
    std::map<std::string, MonitorMetrics> metrics_;
    std::map<std::string, ExtendedMetrics> extended_metrics_;
    SystemStatus system_status_;
    
    // 配置
    struct Config {
        int check_interval_ms;
        double memory_threshold;
        double cpu_threshold;
        int max_errors_before_unhealthy;
        std::vector<std::string> monitored_symbols;
    } config_;

    // 告警相关
    std::map<std::string, double> alert_thresholds_;
    std::function<void(const std::string&, const std::string&)> alert_callback_;
    
    // 内部方法
    void monitorLoop();
    void updateMetrics();
    void collectTradeMetrics(const std::string& symbol);
    void collectSystemMetrics();
    void collectPositionMetrics(const std::string& symbol);
    void calculateRiskMetrics(const std::string& symbol, 
                            const execution::PositionInfo& position);
    void updateSystemStatus();
    void checkThresholds();
    void checkResourceUsage();
    void handleApiError(const std::string& operation, const std::string& error);
    bool calculateSystemHealth();
    double calculateMessageRate() const;
    double calculateErrorRate() const;
    void triggerAlert(const std::string& source, const std::string& message);
};

} // namespace metrics
} // namespace monitor
} // namespace funding_arbitrage