#include "order_manager.h"
#include <chrono>

namespace funding_arbitrage {
namespace trading {
namespace position {

OrderManager::OrderManager(
    const std::shared_ptr<config::Config>& config,
    std::shared_ptr<api::BinanceApi> api,
    std::shared_ptr<api::BinanceWebsocketClient> ws_client)
    : api_(std::move(api))
    , ws_client_(std::move(ws_client))
    , logger_(std::make_shared<utils::Logger>("OrderManager")) {

    auto execution_config = config->getSubConfig("execution.order");
    if (!execution_config) {
        throw std::runtime_error("Missing execution configuration");
    }

    // 加载配置
    config_.max_retry_times = execution_config->getInt("max_retry_times", 3);
    config_.retry_delay_ms = execution_config->getInt("retry_delay_ms", 200);
    config_.price_deviation_threshold = execution_config->getDouble("price_deviation_threshold", 0.001);
    config_.order_timeout_ms = execution_config->getInt("order_timeout_ms", 5000);
    config_.use_post_only = execution_config->getBool("use_post_only", true);

    // 订阅订单更新
    subscribeToOrderUpdates();
    
    logger_->info("OrderManager initialized");
}

void OrderManager::subscribeToOrderUpdates() {
    if (!ws_client_) {
        logger_->error("WebSocket client not initialized");
        return;
    }

    ws_client_->registerEventHandler(
        std::make_shared<api::WebSocketEventHandler>([this](const api::WebSocketEvent& event) {
            if (event.type == api::WebSocketEventType::ORDER_UPDATE) {
                OrderInfo order_info;
                // 从事件中解析订单信息
                order_info.order_id = event.data["o"]["i"].asString();
                order_info.symbol = event.data["o"]["s"].asString();
                order_info.side = stringToOrderSide(event.data["o"]["S"].asString());
                order_info.type = stringToOrderType(event.data["o"]["o"].asString());
                order_info.status = stringToOrderStatus(event.data["o"]["X"].asString());
                order_info.price = std::stod(event.data["o"]["p"].asString());
                order_info.executed_quantity = std::stod(event.data["o"]["z"].asString());

                handleOrderUpdate(order_info);
            }
            return true;
        })
    );
}

std::string OrderManager::placeOrder(const OrderRequest& request) {
    if (!validateOrderRequest(request)) {
        throw std::invalid_argument("Invalid order request");
    }

    // 检查是否需要拆分订单
    auto split_orders = splitOrder(request);
    if (split_orders.size() > 1) {
        return placeBatchOrders(split_orders);
    }

    try {
        // 获取当前市场价格
        double mark_price = api_->getMarkPrice(request.symbol);
        double adjusted_price = calculateSlippagePrice(request.symbol, request.side, mark_price);

        // 修改订单请求
        OrderRequest adjusted_request = request;
        if (request.type != OrderType::MARKET) {
            adjusted_request.price = adjusted_price;
            if (config_.use_post_only) {
                adjusted_request.type = OrderType::POST_ONLY;
            }
        }

        // 发送订单
        std::string order_id = api_->placeOrder(adjusted_request);
        
        // 缓存订单信息
        OrderInfo info;
        info.order_id = order_id;
        info.symbol = request.symbol;
        info.side = request.side;
        info.type = request.type;
        info.status = OrderStatus::NEW;
        info.original_quantity = request.quantity;
        info.price = adjusted_request.price;
        updateOrderCache(info);

        logger_->info("Order placed successfully: " + order_id);
        return order_id;

    } catch (const std::exception& e) {
        logger_->error("Failed to place order: " + std::string(e.what()));
        throw;
    }
}

std::string OrderManager::placeBatchOrders(const std::vector<OrderRequest>& requests) {
    logger_->info("Placing batch orders, count: " + std::to_string(requests.size()));

    std::string first_order_id;
    for (const auto& request : requests) {
        try {
            std::string order_id = placeOrder(request);
            if (first_order_id.empty()) {
                first_order_id = order_id;
            }
        } catch (const std::exception& e) {
            logger_->error("Failed to place batch order: " + std::string(e.what()));
            // 继续执行其他订单
        }
    }

    return first_order_id;
}

bool OrderManager::cancelOrder(const std::string& symbol, const std::string& orderId) {
    try {
        if (api_->cancelOrder(symbol, orderId)) {
            logger_->info("Order cancelled: " + orderId);
            
            std::lock_guard<std::mutex> lock(orders_mutex_);
            if (active_orders_.count(orderId)) {
                active_orders_[orderId].status = OrderStatus::CANCELED;
            }
            return true;
        }
        return false;
    } catch (const std::exception& e) {
        logger_->error("Failed to cancel order: " + std::string(e.what()));
        return false;
    }
}

bool OrderManager::cancelAllOrders(const std::string& symbol) {
    try {
        std::vector<OrderInfo> open_orders = getOpenOrders(symbol);
        bool all_cancelled = true;
        
        for (const auto& order : open_orders) {
            if (!cancelOrder(symbol, order.order_id)) {
                all_cancelled = false;
            }
        }
        
        return all_cancelled;
    } catch (const std::exception& e) {
        logger_->error("Failed to cancel all orders: " + std::string(e.what()));
        return false;
    }
}

OrderInfo OrderManager::getOrderStatus(const std::string& symbol, const std::string& orderId) {
    // 首先检查缓存
    {
        std::lock_guard<std::mutex> lock(orders_mutex_);
        auto it = active_orders_.find(orderId);
        if (it != active_orders_.end()) {
            return it->second;
        }
    }

    // 从API获取
    try {
        return api_->getOrderStatus(symbol, orderId);
    } catch (const std::exception& e) {
        logger_->error("Failed to get order status: " + std::string(e.what()));
        throw;
    }
}

std::vector<OrderInfo> OrderManager::getOpenOrders(const std::string& symbol) {
    try {
        return api_->getOpenOrders(symbol);
    } catch (const std::exception& e) {
        logger_->error("Failed to get open orders: " + std::string(e.what()));
        throw;
    }
}

bool OrderManager::waitForOrderFill(const std::string& orderId, int timeout_ms) {
    auto start_time = std::chrono::steady_clock::now();
    
    while (true) {
        try {
            auto info = getOrderStatus("", orderId);  // symbol can be empty as we have orderId
            
            if (info.status == OrderStatus::FILLED) {
                return true;
            }
            
            if (info.status == OrderStatus::CANCELED || 
                info.status == OrderStatus::REJECTED ||
                info.status == OrderStatus::EXPIRED) {
                return false;
            }
            
            auto current_time = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                current_time - start_time).count();
                
            if (elapsed >= timeout_ms) {
                logger_->warn("Order fill timeout: " + orderId);
                return false;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
        } catch (const std::exception& e) {
            logger_->error("Error waiting for order fill: " + std::string(e.what()));
            return false;
        }
    }
}

void OrderManager::handleOrderUpdate(const OrderInfo& update) {
    // 更新缓存
    updateOrderCache(update);

    // 调用回调
    std::lock_guard<std::mutex> lock(orders_mutex_);
    auto it = order_callbacks_.find(update.order_id);
    if (it != order_callbacks_.end()) {
        try {
            it->second(update);
        } catch (const std::exception& e) {
            logger_->error("Order callback error: " + std::string(e.what()));
        }
    }
}

void OrderManager::updateOrderCache(const OrderInfo& order) {
    std::lock_guard<std::mutex> lock(orders_mutex_);
    if (order.status == OrderStatus::FILLED || 
        order.status == OrderStatus::CANCELED ||
        order.status == OrderStatus::REJECTED ||
        order.status == OrderStatus::EXPIRED) {
        active_orders_.erase(order.order_id);
    } else {
        active_orders_[order.order_id] = order;
    }
}

std::vector<OrderRequest> OrderManager::splitOrder(const OrderRequest& request) {
    std::vector<OrderRequest> split_orders;
    
    // 实现订单拆分逻辑
    // 这里可以根据具体需求实现不同的拆分策略
    split_orders.push_back(request);  // 暂时不拆分
    
    return split_orders;
}

bool OrderManager::validateOrderRequest(const OrderRequest& request) {
    if (request.symbol.empty()) {
        logger_->error("Empty symbol in order request");
        return false;
    }
    
    if (request.quantity <= 0) {
        logger_->error("Invalid quantity: " + std::to_string(request.quantity));
        return false;
    }
    
    if (request.type != OrderType::MARKET && request.price <= 0) {
        logger_->error("Invalid price for limit order: " + 
                      std::to_string(request.price));
        return false;
    }
    
    return true;
}

double OrderManager::calculateSlippagePrice(const std::string& symbol, 
                                          OrderSide side, 
                                          double price) {
    double slippage = price * config_.price_deviation_threshold;
    if (side == OrderSide::BUY) {
        return price * (1 + config_.price_deviation_threshold);
    } else {
        return price * (1 - config_.price_deviation_threshold);
    }
}

void OrderManager::setOrderUpdateCallback(
    std::function<void(const OrderInfo&)> callback) {
    // 不需要锁，因为这个通常在初始化时调用
    order_callbacks_["global"] = std::move(callback);
}

} // namespace position
} // namespace trading
} // namespace funding_arbitrage