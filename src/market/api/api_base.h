#pragma once
#include "../types/execution_types.h"
#include <string>
#include <memory>

namespace funding_arbitrage {
namespace market {
namespace api {

class ApiBase {
public:
    virtual ~ApiBase() = default;

    // Market Data Methods
    virtual double getFundingRate(const std::string& symbol) = 0;
    virtual double getMarkPrice(const std::string& symbol) = 0;
    virtual double getLastPrice(const std::string& symbol) = 0;
    
    // Account & Trading Methods
    virtual double getBalance(const std::string& asset) = 0;
    virtual std::string placeOrder(const execution::OrderRequest& request) = 0;
    virtual bool cancelOrder(const std::string& symbol, const std::string& orderId) = 0;
    virtual execution::OrderInfo getOrderStatus(const std::string& symbol, const std::string& orderId) = 0;
    virtual std::vector<execution::OrderInfo> getOpenOrders(const std::string& symbol = "") = 0;
    virtual std::vector<execution::PositionInfo> getOpenPositions() = 0;
    virtual bool setLeverage(const std::string& symbol, int leverage) = 0;
    virtual bool setMarginType(const std::string& symbol, execution::MarginType marginType) = 0;

protected:
    virtual std::string signRequest(const std::string& queryString) = 0;
};

} // namespace api
} // namespace market
} // namespace funding_arbitrage
