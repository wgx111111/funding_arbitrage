#pragma once
#include <string>
#include <json/json.h>

namespace funding_arbitrage {
namespace market {
namespace api {

enum class WebSocketEventType {
    MARKET_PRICE,           // 市场价格更新
    MARK_PRICE,            // 标记价格更新
    FUNDING_RATE,          // 资金费率更新
    BOOK_TICKER,           // 最优挂单更新
    ORDER_UPDATE,          // 订单更新
    ACCOUNT_UPDATE,        // 账户更新
    POSITION_UPDATE,       // 仓位更新
    SUBSCRIPTION_SUCCESS,   // 订阅成功
    SUBSCRIPTION_FAILED,    // 订阅失败
    UNKNOWN               // 未知事件
};

struct WebSocketEvent {
    WebSocketEventType type;
    std::string raw_data;
    Json::Value data;
    std::string symbol;
    int64_t timestamp;
    bool is_valid;
    std::string error_message;

    static WebSocketEvent parse(const std::string& message);
    bool validate() const;
};

class WebSocketEventHandler {
public:
    virtual ~WebSocketEventHandler() = default;
    virtual bool handleEvent(const WebSocketEvent& event) = 0;
    virtual bool canHandle(const WebSocketEvent& event) const = 0;
};

// 具体的事件处理器
class MarketDataEventHandler : public WebSocketEventHandler {
public:
    using PriceCallback = std::function<void(const std::string& symbol, 
                                           double price, 
                                           int64_t timestamp)>;
    using FundingRateCallback = std::function<void(const std::string& symbol,
                                                  double rate,
                                                  int64_t timestamp)>;

    void setPriceCallback(PriceCallback callback) {
        price_callback_ = std::move(callback);
    }

    void setFundingRateCallback(FundingRateCallback callback) {
        funding_callback_ = std::move(callback);
    }

    bool handleEvent(const WebSocketEvent& event) override;
    bool canHandle(const WebSocketEvent& event) const override;

private:
    PriceCallback price_callback_;
    FundingRateCallback funding_callback_;
};

} // namespace api
} // namespace market
} // namespace funding_arbitrage