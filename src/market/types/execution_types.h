#pragma once
#include <string>
#include <vector>
#include <chrono>
#include <map>

namespace funding_arbitrage {
namespace market {
namespace execution {

// 订单方向
enum class OrderSide {
    BUY,
    SELL
};

// 订单类型
enum class OrderType {
    MARKET,          // 市价单
    LIMIT,           // 限价单
    POST_ONLY,       // 只做Maker单
    STOP_MARKET,     // 止损市价单
    STOP_LIMIT,      // 止损限价单
    TAKE_PROFIT,     // 止盈单
    LIQUIDATION      // 强平单
};

// 订单状态
enum class OrderStatus {
    NEW,                // 新建订单
    PARTIALLY_FILLED,   // 部分成交
    FILLED,            // 完全成交
    CANCELED,          // 已取消
    REJECTED,          // 被拒绝
    EXPIRED,           // 已过期
    PENDING_CANCEL     // 待取消
};

// 订单时效性
enum class TimeInForce {
    GTC,    // Good Till Cancel
    IOC,    // Immediate Or Cancel
    FOK,    // Fill Or Kill
    GTX     // Good Till Crossing
};

// 仓位方向
enum class PositionSide {
    LONG,
    SHORT,
    BOTH    // 双向持仓
};

// 保证金类型
enum class MarginType {
    ISOLATED,   // 逐仓
    CROSS       // 全仓
};

// 订单请求
struct OrderRequest {
    std::string symbol;                     // 交易对
    OrderSide side;                         // 订单方向
    OrderType type;                         // 订单类型
    double quantity;                        // 数量
    double price{0.0};                      // 价格（限价单必需）
    double stop_price{0.0};                 // 触发价格（止损/止盈单必需）
    std::string client_order_id;           // 客户端订单ID
    TimeInForce time_in_force{TimeInForce::GTC}; // 订单时效性
    bool reduce_only{false};               // 是否只减仓
    bool close_position{false};            // 是否平仓
    PositionSide position_side{PositionSide::BOTH}; // 仓位方向
    MarginType margin_type{MarginType::ISOLATED};   // 保证金类型
    double activation_price{0.0};          // 追踪止损激活价格
    double callback_rate{0.0};             // 追踪止损回调比例
    std::map<std::string, std::string> extra_params; // 额外参数
};

// 订单信息
struct OrderInfo {
    std::string order_id;                  // 订单ID
    std::string client_order_id;           // 客户端订单ID
    std::string symbol;                    // 交易对
    OrderSide side;                        // 订单方向
    OrderType type;                        // 订单类型
    OrderStatus status;                    // 订单状态
    PositionSide position_side;            // 仓位方向
    MarginType margin_type;                // 保证金类型
    
    double original_quantity;              // 原始数量
    double executed_quantity;              // 已成交数量
    double remaining_quantity;             // 剩余数量
    double price;                          // 订单价格
    double average_price;                  // 平均成交价格
    double stop_price;                     // 触发价格
    double commission;                     // 手续费
    std::string commission_asset;          // 手续费资产
    
    std::chrono::system_clock::time_point create_time;  // 创建时间
    std::chrono::system_clock::time_point update_time;  // 更新时间
    
    bool is_working;                       // 是否在orderbook上
    bool is_isolated;                      // 是否是逐仓
    std::vector<std::string> fills;        // 成交明细ID列表
};

// 持仓信息
struct PositionInfo {
    std::string symbol;                    // 交易对
    PositionSide side;                     // 持仓方向
    MarginType margin_type;                // 保证金类型
    double amount;                         // 持仓数量
    double entry_price;                    // 开仓均价
    double mark_price;                     // 标记价格
    double liquidation_price;              // 强平价格
    double margin;                         // 保证金
    double leverage;                       // 杠杆倍数
    double unrealized_pnl;                 // 未实现盈亏
    double realized_pnl;                   // 已实现盈亏
    bool isolated;                         // 是否逐仓
    std::chrono::system_clock::time_point update_time; // 更新时间
};

// 成交明细
struct TradeInfo {
    std::string trade_id;                  // 成交ID
    std::string order_id;                  // 订单ID
    std::string symbol;                    // 交易对
    OrderSide side;                        // 成交方向
    double price;                          // 成交价格
    double quantity;                       // 成交数量
    double commission;                     // 手续费
    std::string commission_asset;          // 手续费资产
    bool is_maker;                         // 是否是maker
    std::chrono::system_clock::time_point time; // 成交时间
};

// 订单执行结果
struct ExecutionResult {
    bool success;                          // 是否成功
    std::string order_id;                  // 订单ID
    std::string error_message;             // 错误信息
    OrderStatus status;                    // 订单状态
    double filled_quantity;                // 成交数量
    double average_price;                  // 平均成交价
    std::vector<TradeInfo> trades;         // 成交明细
    std::chrono::system_clock::time_point time; // 执行时间
};

// 工具函数
inline std::string orderSideToString(OrderSide side) {
    switch (side) {
        case OrderSide::BUY: return "BUY";
        case OrderSide::SELL: return "SELL";
        default: return "UNKNOWN";
    }
}

inline std::string orderTypeToString(OrderType type) {
    switch (type) {
        case OrderType::MARKET: return "MARKET";
        case OrderType::LIMIT: return "LIMIT";
        case OrderType::POST_ONLY: return "POST_ONLY";
        case OrderType::STOP_MARKET: return "STOP_MARKET";
        case OrderType::STOP_LIMIT: return "STOP_LIMIT";
        case OrderType::TAKE_PROFIT: return "TAKE_PROFIT";
        case OrderType::LIQUIDATION: return "LIQUIDATION";
        default: return "UNKNOWN";
    }
}

inline std::string orderStatusToString(OrderStatus status) {
    switch (status) {
        case OrderStatus::NEW: return "NEW";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderStatus::FILLED: return "FILLED";
        case OrderStatus::CANCELED: return "CANCELED";
        case OrderStatus::REJECTED: return "REJECTED";
        case OrderStatus::EXPIRED: return "EXPIRED";
        case OrderStatus::PENDING_CANCEL: return "PENDING_CANCEL";
        default: return "UNKNOWN";
    }
}

inline std::string timeInForceToString(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
        case TimeInForce::GTX: return "GTX";
        default: return "UNKNOWN";
    }
}

inline std::string positionSideToString(PositionSide side) {
    switch (side) {
        case PositionSide::LONG: return "LONG";
        case PositionSide::SHORT: return "SHORT";
        case PositionSide::BOTH: return "BOTH";
        default: return "UNKNOWN";
    }
}

inline std::string marginTypeToString(MarginType type) {
    switch (type) {
        case MarginType::ISOLATED: return "ISOLATED";
        case MarginType::CROSS: return "CROSS";
        default: return "UNKNOWN";
    }
}

// 字符串转换函数
inline OrderSide stringToOrderSide(const std::string& str) {
    if (str == "BUY") return OrderSide::BUY;
    if (str == "SELL") return OrderSide::SELL;
    throw std::invalid_argument("Invalid order side: " + str);
}

inline OrderType stringToOrderType(const std::string& str) {
    if (str == "MARKET") return OrderType::MARKET;
    if (str == "LIMIT") return OrderType::LIMIT;
    if (str == "POST_ONLY") return OrderType::POST_ONLY;
    if (str == "STOP_MARKET") return OrderType::STOP_MARKET;
    if (str == "STOP_LIMIT") return OrderType::STOP_LIMIT;
    if (str == "TAKE_PROFIT") return OrderType::TAKE_PROFIT;
    if (str == "LIQUIDATION") return OrderType::LIQUIDATION;
    throw std::invalid_argument("Invalid order type: " + str);
}

inline OrderStatus stringToOrderStatus(const std::string& str) {
    if (str == "NEW") return OrderStatus::NEW;
    if (str == "PARTIALLY_FILLED") return OrderStatus::PARTIALLY_FILLED;
    if (str == "FILLED") return OrderStatus::FILLED;
    if (str == "CANCELED") return OrderStatus::CANCELED;
    if (str == "REJECTED") return OrderStatus::REJECTED;
    if (str == "EXPIRED") return OrderStatus::EXPIRED;
    if (str == "PENDING_CANCEL") return OrderStatus::PENDING_CANCEL;
    throw std::invalid_argument("Invalid order status: " + str);
}

} // namespace execution
} // namespace market
} // namespace funding_arbitrage