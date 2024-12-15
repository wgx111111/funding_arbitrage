#pragma once
#include "../api/binance_api.h"
#include "../api/binance_websocket_client.h"
#include "execution_types.h"
#include "../utils/logger.h"
#include <memory>
#include <map>
#include <mutex>

namespace funding_arbitrage {
namespace trading {
namespace execution {

class OrderManager {
public:
    OrderManager(const std::shared_ptr<config::Config>& config,
                std::shared_ptr<api::BinanceApi> api,
                std::shared_ptr<api::BinanceWebsocketClient> ws_client);

    // 下单接口
    std::string placeOrder(const OrderRequest& request);
    std::string placeBatchOrders(const std::vector<OrderRequest>& requests);
    
    // 取消订单
    bool cancelOrder(const std::string& symbol, const std::string& orderId);
    bool cancelAllOrders(const std::string& symbol);

    // 订单查询
    OrderInfo getOrderStatus(const std::string& symbol, const std::string& orderId);
    std::vector<OrderInfo> getOpenOrders(const std::string& symbol = "");
    
    // 订单等待
    bool waitForOrderFill(const std::string& orderId, int timeout_ms = 5000);

    // 订单事件回调设置
    void setOrderUpdateCallback(std::function<void(const OrderInfo&)> callback);

private:
    std::shared_ptr<api::BinanceApi> api_;
    std::shared_ptr<api::BinanceWebsocketClient> ws_client_;
    std::shared_ptr<utils::Logger> logger_;

    // 订单缓存
    std::mutex orders_mutex_;
    std::map<std::string, OrderInfo> active_orders_;
    std::map<std::string, std::function<void(const OrderInfo&)>> order_callbacks_;

    // 执行配置
    struct {
        int max_retry_times;
        int retry_delay_ms;
        double price_deviation_threshold;
        int order_timeout_ms;
        bool use_post_only;
    } config_;

    // 内部方法
    void handleOrderUpdate(const OrderInfo& update);
    void subscribeToOrderUpdates();
    void updateOrderCache(const OrderInfo& order);
    std::vector<OrderRequest> splitOrder(const OrderRequest& request);
    bool validateOrderRequest(const OrderRequest& request);
    double calculateSlippagePrice(const std::string& symbol, 
                                OrderSide side, 
                                double price);
};

} // namespace execution
} // namespace trading
} // namespace funding_arbitrage