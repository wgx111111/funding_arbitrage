#pragma once
#include "order_manager.h"
#include "execution_types.h"
#include "../utils/logger.h"
#include <memory>
#include <map>
#include <mutex>

namespace funding_arbitrage {
namespace trading {
namespace position {

class PositionManager {
public:
    struct PositionConfig {
        double max_position_size;           // 单个仓位最大大小
        int max_retries;                   // 开仓最大重试次数
        double price_deviation_threshold;   // 价格偏离阈值
        int position_timeout_ms;           // 开仓超时时间
        double min_order_size;             // 最小订单大小
        double max_slippage;               // 最大滑点
        int num_slices;                    // 拆单数量
        double slice_variance;             // 拆单大小随机波动
    };

    PositionManager(const std::shared_ptr<config::Config>& config,
                   std::shared_ptr<OrderManager> order_manager);

    // 仓位操作
    bool openPosition(const std::string& symbol, 
                     double size, 
                     OrderSide side,
                     const std::map<std::string, std::string>& options = {});
                     
    bool closePosition(const std::string& symbol);
    bool closeAllPositions();

    // 仓位管理
    std::vector<PositionInfo> getOpenPositions();
    bool adjustPosition(const std::string& symbol, double target_size);
    bool setLeverage(const std::string& symbol, int leverage);
    
    // 仓位查询
    double getPositionSize(const std::string& symbol);
    PositionInfo getPosition(const std::string& symbol);
    
    // 回调设置
    void setPositionUpdateCallback(std::function<void(const PositionInfo&)> callback);

private:
    std::shared_ptr<OrderManager> order_manager_;
    std::shared_ptr<utils::Logger> logger_;
    PositionConfig config_;

    // 仓位缓存
    std::mutex positions_mutex_;
    std::map<std::string, PositionInfo> positions_;
    std::function<void(const PositionInfo&)> position_callback_;

    // 内部方法
    std::vector<OrderRequest> createOrderRequests(
        const std::string& symbol,
        double total_size,
        OrderSide side,
        const std::map<std::string, std::string>& options
    );
    
    bool executeOrders(const std::vector<OrderRequest>& requests);
    void updatePositionCache(const PositionInfo& position);
    bool validatePositionSize(double size);
    double calculateOptimalSliceSize(double total_size);
    void handleOrderUpdate(const OrderInfo& order_info);
    int determineNumSlices(double total_size);
};

} // namespace position
} // namespace trading
} // namespace funding_arbitrage