#include "binance_api.h"
#include <openssl/hmac.h>
#include <sstream>
#include <iomanip>
#include <chrono>

namespace funding_arbitrage {
namespace market {
namespace api {

BinanceApi::BinanceApi(const std::shared_ptr<config::Config>& config)
    : logger_(std::make_shared<utils::Logger>("BinanceApi"))
    , curl_(nullptr) {
    
    auto binance_config = config->getSubConfig("api.binance");
    if (!binance_config) {
        throw std::runtime_error("Missing Binance configuration");
    }

    // 初始化基础配置
    apiKey_ = binance_config->getString("api_key");
    secretKey_ = binance_config->getString("api_secret");
    baseUrl_ = binance_config->getString("base_url", "https://fapi.binance.com");

    // 初始化限流器
    auto rate_limit = binance_config->getSubConfig("rate_limit");
    if (rate_limit) {
        request_limiter_ = utils::RateLimiter(
            rate_limit->getInt("requests_per_second", 10)
        );
        order_limiter_ = utils::RateLimiter(
            rate_limit->getInt("orders_per_second", 5)
        );
    }

    // 初始化重试配置
    auto retry = binance_config->getSubConfig("retry");
    if (retry) {
        retry_config_.max_retries = retry->getInt("max_retries", 3);
        retry_config_.retry_delay_ms = retry->getInt("retry_delay_ms", 1000);
        retry_config_.backoff_multiplier = retry->getDouble("backoff_multiplier", 2.0);
        retry_config_.retriable_status_codes = {408, 429, 500, 502, 503, 504};
    }

    initializeCurl();
    logger_->info("BinanceApi initialized successfully");
}

BinanceApi::~BinanceApi() {
    if (curl_) {
        curl_easy_cleanup(curl_);
        logger_->info("CURL cleanup completed");
    }
}

double BinanceApi::getFundingRate(const std::string& symbol) {
    return executeWithRetry<double>(
        [this, &symbol]() {
            validateSymbol(symbol);

            std::map<std::string, std::string> params;
            params["symbol"] = symbol;

            Json::Value response = makeRequest("GET", "/fapi/v1/premiumIndex", params);
            return std::stod(response["lastFundingRate"].asString());
        },
        "GetFundingRate"
    );
}

double BinanceApi::getMarkPrice(const std::string& symbol) {
    return executeWithRetry<double>(
        [this, &symbol]() {
            validateSymbol(symbol);

            std::map<std::string, std::string> params;
            params["symbol"] = symbol;

            Json::Value response = makeRequest("GET", "/fapi/v1/premiumIndex", params);
            return std::stod(response["markPrice"].asString());
        },
        "GetMarkPrice"
    );
}

double BinanceApi::getLastPrice(const std::string& symbol) {
    return executeWithRetry<double>(
        [this, &symbol]() {
            validateSymbol(symbol);

            std::map<std::string, std::string> params;
            params["symbol"] = symbol;

            Json::Value response = makeRequest("GET", "/fapi/v1/ticker/price", params);
            return std::stod(response["price"].asString());
        },
        "GetLastPrice"
    );
}

std::string BinanceApi::placeOrder(const execution::OrderRequest& request) {
    return executeWithRetry<std::string>(
        [this, &request]() {
            if (!order_limiter_.tryAcquire()) {
                throw std::runtime_error("Order rate limit exceeded");
            }

            validateSymbol(request.symbol);

            std::map<std::string, std::string> params;
            params["symbol"] = request.symbol;
            params["side"] = execution::orderSideToString(request.side);
            params["type"] = execution::orderTypeToString(request.type);
            params["quantity"] = std::to_string(request.quantity);

            if (request.type != execution::OrderType::MARKET) {
                params["price"] = std::to_string(request.price);
                params["timeInForce"] = execution::timeInForceToString(request.time_in_force);
            }

            if (request.stop_price > 0) {
                params["stopPrice"] = std::to_string(request.stop_price);
            }

            if (request.reduce_only) {
                params["reduceOnly"] = "true";
            }

            if (request.close_position) {
                params["closePosition"] = "true";
            }

            // 添加额外参数
            for (const auto& param : request.extra_params) {
                params[param.first] = param.second;
            }

            Json::Value response = makeRequest("POST", "/fapi/v1/order", params, true);
            return response["orderId"].asString();
        },
        "PlaceOrder"
    );
}

bool BinanceApi::cancelOrder(const std::string& symbol, const std::string& orderId) {
    return executeWithRetry<bool>(
        [this, &symbol, &orderId]() {
            validateSymbol(symbol);

            std::map<std::string, std::string> params;
            params["symbol"] = symbol;
            params["orderId"] = orderId;

            makeRequest("DELETE", "/fapi/v1/order", params, true);
            return true;
        },
        "CancelOrder"
    );
}

execution::OrderInfo BinanceApi::getOrderStatus(const std::string& symbol, 
                                              const std::string& orderId) {
    return executeWithRetry<execution::OrderInfo>(
        [this, &symbol, &orderId]() {
            validateSymbol(symbol);

            std::map<std::string, std::string> params;
            params["symbol"] = symbol;
            params["orderId"] = orderId;

            Json::Value response = makeRequest("GET", "/fapi/v1/order", params, true);
            
            execution::OrderInfo info;
            info.order_id = response["orderId"].asString();
            info.client_order_id = response["clientOrderId"].asString();
            info.symbol = response["symbol"].asString();
            info.side = execution::stringToOrderSide(response["side"].asString());
            info.type = execution::stringToOrderType(response["type"].asString());
            info.status = execution::stringToOrderStatus(response["status"].asString());
            info.original_quantity = std::stod(response["origQty"].asString());
            info.executed_quantity = std::stod(response["executedQty"].asString());
            info.remaining_quantity = info.original_quantity - info.executed_quantity;
            info.price = std::stod(response["price"].asString());
            info.average_price = std::stod(response["avgPrice"].asString());

            // 从时间戳转换
            info.create_time = std::chrono::system_clock::from_time_t(
                response["time"].asInt64() / 1000
            );
            info.update_time = std::chrono::system_clock::from_time_t(
                response["updateTime"].asInt64() / 1000
            );

            return info;
        },
        "GetOrderStatus"
    );
}

std::vector<execution::OrderInfo> BinanceApi::getOpenOrders(const std::string& symbol) {
    return executeWithRetry<std::vector<execution::OrderInfo>>(
        [this, &symbol]() {
            std::map<std::string, std::string> params;
            if (!symbol.empty()) {
                validateSymbol(symbol);
                params["symbol"] = symbol;
            }

            Json::Value response = makeRequest("GET", "/fapi/v1/openOrders", params, true);
            std::vector<execution::OrderInfo> orders;

            for (const auto& order : response) {
                execution::OrderInfo info;
                info.order_id = order["orderId"].asString();
                info.symbol = order["symbol"].asString();
                info.side = execution::stringToOrderSide(order["side"].asString());
                info.type = execution::stringToOrderType(order["type"].asString());
                info.status = execution::stringToOrderStatus(order["status"].asString());
                info.original_quantity = std::stod(order["origQty"].asString());
                info.executed_quantity = std::stod(order["executedQty"].asString());
                info.price = std::stod(order["price"].asString());
                orders.push_back(info);
            }

            return orders;
        },
        "GetOpenOrders"
    );
}

template<typename T>
T BinanceApi::executeWithRetry(const std::function<T()>& func, 
                              const std::string& operation) {
    int retry_count = 0;
    int delay_ms = retry_config_.retry_delay_ms;

    while (true) {
        try {
            return func();
        } catch (const std::exception& e) {
            std::string error_msg = e.what();
            int status_code = extractStatusCode(error_msg);

            if (!shouldRetry(status_code, retry_count)) {
                logger_->error("Operation " + operation + " failed: " + error_msg);
                throw;
            }

            retry_count++;
            logger_->warn("Retrying " + operation + " (attempt " + 
                         std::to_string(retry_count) + "): " + error_msg);
            
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms *= retry_config_.backoff_multiplier;
        }
    }
}

void BinanceApi::initializeCurl() {
    curl_ = curl_easy_init();
    if (!curl_) {
        throw std::runtime_error("Failed to initialize CURL");
    }
}

std::string BinanceApi::signRequest(const std::string& queryString) {
    unsigned char* digest = HMAC(EVP_sha256(), 
                                secretKey_.c_str(), 
                                secretKey_.length(),
                                (unsigned char*)queryString.c_str(), 
                                queryString.length(),
                                nullptr, 
                                nullptr);
    
    std::stringstream ss;
    for(int i = 0; i < 32; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
    }
    return ss.str();
}

Json::Value BinanceApi::makeRequest(const std::string& method,
                                  const std::string& endpoint,
                                  const std::map<std::string, std::string>& params,
                                  bool needSign) {
    // 应用请求限流
    request_limiter_.acquire();

    if (!curl_) {
        throw std::runtime_error("CURL not initialized");
    }

    std::string queryString = buildQueryString(params);
    std::string url = baseUrl_ + endpoint;
    std::string finalUrl = url;

    if (needSign) {
        if (!queryString.empty()) {
            queryString += "&timestamp=" + std::to_string(getTimestamp());
        } else {
            queryString = "timestamp=" + std::to_string(getTimestamp());
        }
        queryString += "&signature=" + signRequest(queryString);
    }

    if (!queryString.empty()) {
        finalUrl += "?" + queryString;
    }

    logger_->debug("Making " + method + " request to: " + finalUrl);

    curl_easy_reset(curl_);
    
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!apiKey_.empty()) {
        headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + apiKey_).c_str());
    }

    std::string response;
    curl_easy_setopt(curl_, CURLOPT_URL, finalUrl.c_str());
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
    
    if (method == "POST") {
        curl_easy_setopt(curl_, CURLOPT_POST, 1L);
    } else if (method == "DELETE") {
        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "DELETE");
    }

    // SSL 配置
    curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);
    
    // 超时设置
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl_);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        handleCurlError(res, method + " " + endpoint);
    }

    logger_->debug("Raw response: " + response);

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(response, root)) {
        handleJsonError(response, method + " " + endpoint);
    }

    validateResponse(root);
    return root;
}

std::vector<execution::PositionInfo> BinanceApi::getOpenPositions() {
    return executeWithRetry<std::vector<execution::PositionInfo>>(
        [this]() {
            Json::Value response = makeRequest("GET", "/fapi/v2/positionRisk", {}, true);
            std::vector<execution::PositionInfo> positions;

            for (const auto& pos : response) {
                double amount = std::stod(pos["positionAmt"].asString());
                if (amount != 0.0) {
                    execution::PositionInfo info;
                    info.symbol = pos["symbol"].asString();
                    info.amount = amount;
                    info.entry_price = std::stod(pos["entryPrice"].asString());
                    info.mark_price = std::stod(pos["markPrice"].asString());
                    info.unrealized_pnl = std::stod(pos["unRealizedProfit"].asString());
                    info.liquidation_price = std::stod(pos["liquidationPrice"].asString());
                    info.leverage = std::stod(pos["leverage"].asString());
                    info.margin_type = pos["marginType"].asString() == "isolated" ?
                        execution::MarginType::ISOLATED : execution::MarginType::CROSS;
                    
                    positions.push_back(info);
                }
            }

            return positions;
        },
        "GetOpenPositions"
    );
}

bool BinanceApi::setLeverage(const std::string& symbol, int leverage) {
    return executeWithRetry<bool>(
        [this, &symbol, leverage]() {
            validateSymbol(symbol);

            std::map<std::string, std::string> params;
            params["symbol"] = symbol;
            params["leverage"] = std::to_string(leverage);

            makeRequest("POST", "/fapi/v1/leverage", params, true);
            return true;
        },
        "SetLeverage"
    );
}

bool BinanceApi::setMarginType(const std::string& symbol, execution::MarginType marginType) {
    return executeWithRetry<bool>(
        [this, &symbol, marginType]() {
            validateSymbol(symbol);

            std::map<std::string, std::string> params;
            params["symbol"] = symbol;
            params["marginType"] = execution::marginTypeToString(marginType);

            makeRequest("POST", "/fapi/v1/marginType", params, true);
            return true;
        },
        "SetMarginType"
    );
}

// 辅助方法实现
size_t BinanceApi::WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    size_t realsize = size * nmemb;
    userp->append((char*)contents, realsize);
    return realsize;
}

std::string BinanceApi::buildQueryString(const std::map<std::string, std::string>& params) {
    std::stringstream ss;
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (it != params.begin()) {
            ss << "&";
        }
        ss << it->first << "=" << it->second;
    }
    return ss.str();
}

void BinanceApi::validateSymbol(const std::string& symbol) const {
    if (symbol.empty()) {
        logger_->error("Empty symbol provided");
        throw std::invalid_argument("Symbol cannot be empty");
    }

    if (symbol.length() < 2 || symbol.length() > 20) {
        logger_->error("Invalid symbol length: " + symbol);
        throw std::invalid_argument("Invalid symbol length: " + symbol);
    }

    // Additional symbol validation logic can be added here
    logger_->debug("Symbol validated: " + symbol);
}

void BinanceApi::validateResponse(const Json::Value& response) const {
    if (response.isMember("code") && response.isMember("msg")) {
        std::string errorMsg = "API error - Code: " + response["code"].asString() + 
                             ", Message: " + response["msg"].asString();
        logger_->error(errorMsg);
        throw std::runtime_error(errorMsg);
    }
}

long long BinanceApi::getTimestamp() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

void BinanceApi::handleCurlError(CURLcode res, const std::string& operation) {
    std::string error = "CURL error during " + operation + ": " + 
                       std::string(curl_easy_strerror(res));
    logger_->error(error);
    throw std::runtime_error(error);
}

void BinanceApi::handleJsonError(const std::string& response, const std::string& operation) {
    std::string error = "Failed to parse JSON response during " + operation + 
                       ": " + response;
    logger_->error(error);
    throw std::runtime_error(error);
}

bool BinanceApi::shouldRetry(int status_code, int retry_count) const {
    if (retry_count >= retry_config_.max_retries) {
        return false;
    }

    return retry_config_.retriable_status_codes.count(status_code) > 0;
}

int BinanceApi::extractStatusCode(const std::string& error_msg) const {
    try {
        // 尝试从错误消息中提取HTTP状态码
        size_t pos = error_msg.find("HTTP ");
        if (pos != std::string::npos) {
            return std::stoi(error_msg.substr(pos + 5, 3));
        }

        // 尝试从API错误码中提取
        pos = error_msg.find("Code: ");
        if (pos != std::string::npos) {
            std::string code_str = error_msg.substr(pos + 6);
            pos = code_str.find(",");
            if (pos != std::string::npos) {
                return std::stoi(code_str.substr(0, pos));
            }
        }
    } catch (const std::exception& e) {
        logger_->warn("Failed to extract status code from error message: " + error_msg);
    }
    
    return 0;
}

} // namespace api
} // namespace market
} // namespace funding_arbitrage
