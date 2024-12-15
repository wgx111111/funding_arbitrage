#pragma once
#include "api_base.h"
#include "../../common/logger/logger.h"
#include "../../common/utils/rate_limiter.h"
#include "../../common/config/config.h"
#include <string>
#include <memory>
#include <map>
#include <curl/curl.h>
#include <json/json.h>

namespace funding_arbitrage {
namespace market {
namespace api {

class BinanceApi : public ApiBase {
public:
    explicit BinanceApi(const std::shared_ptr<config::Config>& config);
    ~BinanceApi() override;

    // 禁用拷贝
    BinanceApi(const BinanceApi&) = delete;
    BinanceApi& operator=(const BinanceApi&) = delete;

    // Market Data Methods
    double getFundingRate(const std::string& symbol) override;
    double getMarkPrice(const std::string& symbol) override;
    double getLastPrice(const std::string& symbol) override;
    
    // Account & Trading Methods
    double getBalance(const std::string& asset) override;
    std::string placeOrder(const execution::OrderRequest& request) override;
    bool cancelOrder(const std::string& symbol, const std::string& orderId) override;
    execution::OrderInfo getOrderStatus(const std::string& symbol, const std::string& orderId) override;
    std::vector<execution::OrderInfo> getOpenOrders(const std::string& symbol = "") override;
    std::vector<execution::PositionInfo> getOpenPositions() override;
    bool setLeverage(const std::string& symbol, int leverage) override;
    bool setMarginType(const std::string& symbol, execution::MarginType marginType) override;

private:
    // Configuration
    std::string apiKey_;
    std::string secretKey_;
    std::string baseUrl_;
    CURL* curl_;
    std::shared_ptr<utils::Logger> logger_;

    // Rate Limiters
    utils::RateLimiter request_limiter_;
    utils::RateLimiter order_limiter_;

    // Request retry configuration
    struct RetryConfig {
        int max_retries;
        int retry_delay_ms;
        double backoff_multiplier;
        std::set<int> retriable_status_codes;
    } retry_config_;

    // API Base implementation
    std::string signRequest(const std::string& queryString) override;

    // HTTP Request Methods
    Json::Value makeRequest(const std::string& method,
                          const std::string& endpoint,
                          const std::map<std::string, std::string>& params = {},
                          bool needSign = false);

    template<typename T>
    T executeWithRetry(const std::function<T()>& func, const std::string& operation);

    // Helper Methods
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp);
    std::string buildQueryString(const std::map<std::string, std::string>& params);
    void validateSymbol(const std::string& symbol) const;
    void validateResponse(const Json::Value& response) const;
    long long getTimestamp() const;
    int extractStatusCode(const std::string& error_msg) const;

    // Error Handling
    void initializeCurl();
    void handleCurlError(CURLcode res, const std::string& operation);
    void handleJsonError(const std::string& response, const std::string& operation);
    bool shouldRetry(int status_code, int retry_count) const;
};

} // namespace api
} // namespace market
} // namespace funding_arbitrage