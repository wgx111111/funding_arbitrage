#pragma once
#include <prometheus/exposer.h>
#include <prometheus/registry.h>
#include <prometheus/gauge.h>
#include <prometheus/counter.h>
#include "monitor_service.h"

namespace funding_arbitrage {
namespace monitor {

class PrometheusExporter {
public:
    PrometheusExporter(const std::shared_ptr<config::Config>& config,
                      std::shared_ptr<MonitorService> monitor_service);

    void start();
    void stop();
    void updateMetrics();

private:
    std::shared_ptr<MonitorService> monitor_service_;
    std::shared_ptr<utils::Logger> logger_;
    std::unique_ptr<prometheus::Exposer> exposer_;
    std::shared_ptr<prometheus::Registry> registry_;

    // Prometheus metrics
    prometheus::Gauge* system_memory_usage_;
    prometheus::Gauge* system_cpu_usage_;
    prometheus::Gauge* position_size_;
    prometheus::Gauge* unrealized_pnl_;
    prometheus::Counter* total_trades_;
    prometheus::Counter* funding_earned_;
    
    void setupMetrics();
};

} // namespace monitor
} // namespace funding_arbitrage