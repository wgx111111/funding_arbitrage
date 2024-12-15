// src/monitor/prometheus_exporter.cpp
#include "prometheus_exporter.h"

namespace funding_arbitrage {
namespace monitor {
namespace metrics {

PrometheusExporter::PrometheusExporter(
    const std::shared_ptr<config::Config>& config,
    std::shared_ptr<MonitorService> monitor_service)
    : monitor_service_(std::move(monitor_service))
    , logger_(std::make_shared<utils::Logger>("PrometheusExporter")) {

    auto prometheus_config = config->getSubConfig("monitor.prometheus");
    std::string bind_address = prometheus_config->getString("bind_address", "0.0.0.0:9090");

    // 创建 Prometheus exposer
    exposer_ = std::make_unique<prometheus::Exposer>(bind_address);
    registry_ = std::make_shared<prometheus::Registry>();

    // 设置指标
    setupMetrics();
    
    // 注册指标收集器
    exposer_->RegisterCollectable(registry_);
}

void PrometheusExporter::setupMetrics() {
    // 系统指标
    auto& system_metrics = prometheus::BuildGauge()
        .Name("trading_system_metrics")
        .Help("Trading system metrics")
        .Register(*registry_);

    system_memory_usage_ = &system_metrics.Add({{"type", "memory_usage"}});
    system_cpu_usage_ = &system_metrics.Add({{"type", "cpu_usage"}});

    // 交易指标
    auto& trading_metrics = prometheus::BuildGauge()
        .Name("trading_position_metrics")
        .Help("Trading position metrics")
        .Register(*registry_);

    position_size_ = &trading_metrics.Add({{"type", "position_size"}});
    unrealized_pnl_ = &trading_metrics.Add({{"type", "unrealized_pnl"}});

    // 计数器类型指标
    auto& trading_counters = prometheus::BuildCounter()
        .Name("trading_cumulative_metrics")
        .Help("Trading cumulative metrics")
        .Register(*registry_);

    total_trades_ = &trading_counters.Add({{"type", "total_trades"}});
    funding_earned_ = &trading_counters.Add({{"type", "funding_earned"}});
}

void PrometheusExporter::updateMetrics() {
    try {
        // 更新系统指标
        auto status = monitor_service_->getSystemStatus();
        system_memory_usage_->Set(status.memory_usage);
        system_cpu_usage_->Set(status.cpu_usage);

        // 更新交易指标
        auto metrics = monitor_service_->getAllMetrics();
        for (const auto& metric : metrics) {
            position_size_->Set(metric.current_position_size);
            unrealized_pnl_->Set(metric.unrealized_pnl);
            total_trades_->Increment(metric.total_trades);
            funding_earned_->Increment(metric.funding_earned);
        }

    } catch (const std::exception& e) {
        logger_->error("Error updating Prometheus metrics: " + std::string(e.what()));
    }
}

} // namespace metrics
} // namespace monitor
} // namespace funding_arbitrage