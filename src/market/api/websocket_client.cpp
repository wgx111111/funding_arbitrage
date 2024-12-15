#include "binance_websocket_client.h"
#include <json/json.h>
#include <openssl/ssl.h>
#include <chrono>

namespace funding_arbitrage {
namespace market {
namespace api {

BinanceWebsocketClient::BinanceWebsocketClient(const std::shared_ptr<config::Config>& config)
    : logger_(std::make_shared<utils::Logger>("BinanceWebsocketClient"))
    , running_(false)
    , reconnecting_(false)
    , last_pong_time_(0) {
    
    auto ws_config = config->getSubConfig("api.binance.websocket");
    if (!ws_config) {
        throw std::runtime_error("Missing WebSocket configuration");
    }

    // 初始化配置
    ws_url_ = ws_config->getString("url", "wss://fstream.binance.com/ws");
    config_.ping_interval_sec = ws_config->getInt("ping_interval_sec", 30);
    config_.pong_timeout_sec = ws_config->getInt("pong_timeout_sec", 10);
    config_.max_reconnect_attempts = ws_config->getInt("max_reconnect_attempts", 5);
    config_.reconnect_interval_sec = ws_config->getInt("reconnect_interval_sec", 5);

    // 初始化限流
    auto rate_limit = ws_config->getSubConfig("rate_limit");
    if (rate_limit) {
        subscription_limiter_ = utils::RateLimiter(
            rate_limit->getInt("subscriptions_per_second", 10)
        );
    }

    // 初始化市场数据处理器
    market_data_handler_ = std::make_shared<MarketDataEventHandler>();
    registerEventHandler(market_data_handler_);

    // 配置websocket客户端
    client_.clear_access_channels(websocketpp::log::alevel::all);
    client_.set_access_channels(websocketpp::log::alevel::connect);
    client_.set_access_channels(websocketpp::log::alevel::disconnect);
    client_.set_access_channels(websocketpp::log::alevel::app);

    client_.init_asio();

    // 设置回调
    client_.set_tls_init_handler(
        std::bind(&BinanceWebsocketClient::onTlsInit, this, std::placeholders::_1)
    );
    client_.set_open_handler(
        std::bind(&BinanceWebsocketClient::onOpen, this, std::placeholders::_1)
    );
    client_.set_close_handler(
        std::bind(&BinanceWebsocketClient::onClose, this, std::placeholders::_1)
    );
    client_.set_fail_handler(
        std::bind(&BinanceWebsocketClient::onFail, this, std::placeholders::_1)
    );
    client_.set_message_handler(
        std::bind(&BinanceWebsocketClient::onMessage, this, 
                 std::placeholders::_1, std::placeholders::_2)
    );
    client_.set_ping_handler(
        std::bind(&BinanceWebsocketClient::onPing, this,
                 std::placeholders::_1, std::placeholders::_2)
    );
    client_.set_pong_handler(
        std::bind(&BinanceWebsocketClient::onPong, this,
                 std::placeholders::_1, std::placeholders::_2)
    );

    logger_->info("BinanceWebsocketClient initialized");
}

BinanceWebsocketClient::~BinanceWebsocketClient() {
    disconnect();
}

void BinanceWebsocketClient::handleMessage(const std::string& message) {
    try {
        // 解析事件
        WebSocketEvent event = WebSocketEvent::parse(message);
        if (!event.is_valid) {
            logger_->error("Failed to parse message: " + event.error_message);
            return;
        }

        // 处理订阅响应
        if (event.type == WebSocketEventType::SUBSCRIPTION_SUCCESS ||
            event.type == WebSocketEventType::SUBSCRIPTION_FAILED) {
            processSubscriptionResponse(event.data);
            return;
        }

        // 分发事件
        dispatchEvent(event);

        // 处理传统回调
        if (event.data.isMember("stream") && event.data.isMember("data")) {
            std::string stream = event.data["stream"].asString();
            std::string data = event.data["data"].toStyledString();

            std::lock_guard<std::mutex> lock(mutex_);
            auto it = callbacks_.find(stream);
            if (it != callbacks_.end()) {
                try {
                    it->second(data);
                } catch (const std::exception& e) {
                    logger_->error("Callback error for stream " + stream + 
                                 ": " + e.what());
                }
            }
        }
    } catch (const std::exception& e) {
        logger_->error("Error processing message: " + std::string(e.what()));
    }
}

void BinanceWebsocketClient::dispatchEvent(const WebSocketEvent& event) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    for (const auto& handler : event_handlers_) {
        if (handler->canHandle(event)) {
            try {
                handler->handleEvent(event);
            } catch (const std::exception& e) {
                logger_->error("Handler error: " + std::string(e.what()));
            }
        }
    }
}

void BinanceWebsocketClient::registerEventHandler(
    std::shared_ptr<WebSocketEventHandler> handler) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    event_handlers_.push_back(handler);
}

void BinanceWebsocketClient::removeEventHandler(
    const std::shared_ptr<WebSocketEventHandler>& handler) {
    std::lock_guard<std::mutex> lock(handlers_mutex_);
    auto it = std::find(event_handlers_.begin(), event_handlers_.end(), handler);
    if (it != event_handlers_.end()) {
        event_handlers_.erase(it);
    }
}

void BinanceWebsocketClient::setMarkPriceCallback(
    const MarketDataEventHandler::PriceCallback& callback) {
    market_data_handler_->setPriceCallback(callback);
}

void BinanceWebsocketClient::setFundingRateCallback(
    const MarketDataEventHandler::FundingRateCallback& callback) {
    market_data_handler_->setFundingRateCallback(callback);
}

bool BinanceWebsocketClient::connect() {
    if (!subscription_limiter_.tryAcquire()) {
        logger_->error("Connection rate limit exceeded");
        return false;
    }

    try {
        websocketpp::lib::error_code ec;
        auto con = client_.get_connection(ws_url_, ec);
        
        if (ec) {
            logger_->error("Failed to create connection: " + ec.message());
            return false;
        }

        client_.connect(con);
        startClientThread();
        
        std::unique_lock<std::mutex> lock(mutex_);
        bool connected = cv_.wait_for(lock, 
                                    std::chrono::seconds(5),
                                    [this]() { return isConnected(); });

        if (connected) {
            startHeartbeat();
            logger_->info("Successfully connected to " + ws_url_);
            return true;
        }

        logger_->error("Failed to connect within timeout");
        return false;

    } catch (const std::exception& e) {
        logger_->error("Connection error: " + std::string(e.what()));
        return false;
    }
}

void BinanceWebsocketClient::disconnect() {
    if (isConnected()) {
        logger_->info("Disconnecting from websocket");
        try {
            stopHeartbeat();
            client_.close(connection_, websocketpp::close::status::normal, 
                         "Client disconnecting");
        } catch (const std::exception& e) {
            logger_->error("Error during disconnect: " + std::string(e.what()));
        }
    }
    stopClientThread();
}

bool BinanceWebsocketClient::isConnected() const {
    return connection_.lock() != nullptr;
}

bool BinanceWebsocketClient::reconnect() {
    if (reconnecting_) {
        return false;
    }

    reconnecting_ = true;
    logger_->info("Attempting to reconnect...");

    disconnect();
    
    int attempts = 0;
    while (running_ && attempts < config_.max_reconnect_attempts) {
        attempts++;
        logger_->info("Reconnection attempt " + std::to_string(attempts) + 
                     " of " + std::to_string(config_.max_reconnect_attempts));

        if (connect() && resubscribeAll()) {
            logger_->info("Reconnection successful");
            reconnecting_ = false;
            return true;
        }

        std::this_thread::sleep_for(
            std::chrono::seconds(config_.reconnect_interval_sec)
        );
    }

    logger_->error("Failed to reconnect after " + 
                  std::to_string(config_.max_reconnect_attempts) + " attempts");
    reconnecting_ = false;
    return false;
}

bool BinanceWebsocketClient::subscribe(const std::string& channel, 
                                     const MessageCallback& callback) {
    if (!isConnected()) {
        logger_->error("Cannot subscribe: not connected");
        return false;
    }

    if (!subscription_limiter_.tryAcquire()) {
        logger_->error("Subscription rate limit exceeded for channel: " + channel);
        return false;
    }

    try {
        std::vector<std::string> channels = {channel};
        std::string message = buildSubscriptionMessage(channels);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks_[channel] = callback;
        }

        client_.send(connection_, message, websocketpp::frame::opcode::text);
        logger_->info("Subscription request sent for channel: " + channel);
        return true;

    } catch (const std::exception& e) {
        logger_->error("Subscribe error: " + std::string(e.what()));
        return false;
    }
}

bool BinanceWebsocketClient::subscribeBatch(const std::vector<std::string>& channels,
                                          const MessageCallback& callback) {
    if (channels.empty()) {
        return true;
    }

    if (!isConnected()) {
        logger_->error("Cannot subscribe: not connected");
        return false;
    }

    if (!subscription_limiter_.tryAcquire()) {
        logger_->error("Subscription rate limit exceeded");
        return false;
    }

    try {
        std::string message = buildSubscriptionMessage(channels);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& channel : channels) {
                callbacks_[channel] = callback;
            }
        }

        client_.send(connection_, message, websocketpp::frame::opcode::text);
        logger_->info("Batch subscription request sent for " + 
                     std::to_string(channels.size()) + " channels");
        return true;

    } catch (const std::exception& e) {
        logger_->error("Batch subscribe error: " + std::string(e.what()));
        return false;
    }
}

bool BinanceWebsocketClient::unsubscribe(const std::string& channel) {
    if (!isConnected()) {
        logger_->error("Cannot unsubscribe: not connected");
        return false;
    }

    try {
        Json::Value request;
        request["method"] = "UNSUBSCRIBE";
        request["params"].append(channel);
        request["id"] = std::time(nullptr);

        Json::FastWriter writer;
        std::string message = writer.write(request);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks_.erase(channel);
        }

        client_.send(connection_, message, websocketpp::frame::opcode::text);
        logger_->info("Unsubscription request sent for channel: " + channel);
        return true;

    } catch (const std::exception& e) {
        logger_->error("Unsubscribe error: " + std::string(e.what()));
        return false;
    }
}

void BinanceWebsocketClient::handleHeartbeat() {
    while (running_ && isConnected()) {
        try {
            auto now = std::chrono::steady_clock::now();
            auto last_pong = std::chrono::steady_clock::time_point(
                std::chrono::nanoseconds(last_pong_time_)
            );

            // 检查最后一次pong时间
            if (last_pong_time_ > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now - last_pong
                ).count();

                if (elapsed > config_.pong_timeout_sec) {
                    logger_->warn("Pong timeout detected, initiating reconnect");
                    reconnect();
                    break;
                }
            }

            // 发送 ping
            client_.ping(connection_, "");
            logger_->debug("Ping sent");

            // 等待到下一个心跳间隔
            std::this_thread::sleep_for(
                std::chrono::seconds(config_.ping_interval_sec)
            );

        } catch (const std::exception& e) {
            logger_->error("Error in heartbeat thread: " + std::string(e.what()));
            break;
        }
    }
}

void BinanceWebsocketClient::onOpen(ConnectionHdl hdl) {
    logger_->info("Connection opened");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_ = hdl;
    }
    cv_.notify_one();

    if (on_connected_callback_) {
        try {
            on_connected_callback_();
        } catch (const std::exception& e) {
            logger_->error("Connected callback error: " + std::string(e.what()));
        }
    }
}

void BinanceWebsocketClient::onClose(ConnectionHdl hdl) {
    logger_->info("Connection closed");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_.reset();
    }

    if (on_disconnected_callback_) {
        try {
            on_disconnected_callback_();
        } catch (const std::exception& e) {
            logger_->error("Disconnected callback error: " + std::string(e.what()));
        }
    }
    
    if (running_ && !reconnecting_) {
        reconnect();
    }
}

void BinanceWebsocketClient::onFail(ConnectionHdl hdl) {
    logger_->error("Connection failed");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connection_.reset();
    }

    if (on_disconnected_callback_) {
        try {
            on_disconnected_callback_();
        } catch (const std::exception& e) {
            logger_->error("Disconnected callback error: " + std::string(e.what()));
        }
    }
    
    if (running_ && !reconnecting_) {
        reconnect();
    }
}

void BinanceWebsocketClient::onMessage(ConnectionHdl hdl, 
                                     WebsocketClient::message_ptr msg) {
    std::string payload = msg->get_payload();
    logger_->debug("Received message: " + payload);
    handleMessage(payload);
}

void BinanceWebsocketClient::onPing(ConnectionHdl hdl, std::string msg) {
    try {
        // 响应服务器的ping请求
        client_.pong(hdl, msg);
        logger_->debug("Received ping, sent pong response");
    } catch (const std::exception& e) {
        logger_->error("Error sending pong response: " + std::string(e.what()));
    }
}

void BinanceWebsocketClient::onPong(ConnectionHdl hdl, std::string msg) {
    last_pong_time_ = std::chrono::steady_clock::now().time_since_epoch().count();
    logger_->debug("Pong received");
}

Context::ptr BinanceWebsocketClient::onTlsInit(ConnectionHdl hdl) {
    auto ctx = std::make_shared<Context>(Context::tlsv12);
    try {
        ctx->set_options(
            Context::default_workarounds |
            Context::no_sslv2 |
            Context::no_sslv3 |
            Context::single_dh_use
        );
    } catch (const std::exception& e) {
        logger_->error("TLS initialization error: " + std::string(e.what()));
    }
    return ctx;
}

void BinanceWebsocketClient::startClientThread() {
    if (!running_) {
        running_ = true;
        client_thread_ = std::thread([this]() {
            try {
                client_.run();
            } catch (const std::exception& e) {
                logger_->error("Client thread error: " + std::string(e.what()));
            }
        });
    }
}

void BinanceWebsocketClient::stopClientThread() {
    if (running_) {
        running_ = false;
        client_.stop();
        if (client_thread_.joinable()) {
            client_thread_.join();
        }
    }
}

void BinanceWebsocketClient::startHeartbeat() {
    heartbeat_thread_ = std::thread([this]() { handleHeartbeat(); });
    heartbeat_thread_.detach();
}

void BinanceWebsocketClient::stopHeartbeat() {
    // Heartbeat thread is detached, will stop automatically when running_ is false
}

bool BinanceWebsocketClient::resubscribeAll() {
    std::map<std::string, MessageCallback> callbacks_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_copy = callbacks_;
        callbacks_.clear();
    }

    if (callbacks_copy.empty()) {
        return true;
    }

    std::vector<std::string> channels;
    for (const auto& callback : callbacks_copy) {
        channels.push_back(callback.first);
    }

    return subscribeBatch(channels, [callbacks_copy](const std::string& data) {
        // 重用原始回调
        auto it = callbacks_copy.find(data);
        if (it != callbacks_copy.end()) {
            it->second(data);
        }
    });
}

std::string BinanceWebsocketClient::buildSubscriptionMessage(
    const std::vector<std::string>& channels) {
    Json::Value request;
    request["method"] = "SUBSCRIBE";
    for (const auto& channel : channels) {
        request["params"].append(channel);
    }
    request["id"] = std::time(nullptr);

    Json::FastWriter writer;
    return writer.write(request);
}

void BinanceWebsocketClient::processSubscriptionResponse(const Json::Value& response) {
    try {
        if (response.isMember("result") && response["result"].isNull()) {
            logger_->info("Subscription confirmed");
        } else if (response.isMember("error")) {
            logger_->error("Subscription error: " + 
                         response["error"].toStyledString());
        }
    } catch (const std::exception& e) {
        logger_->error("Error processing subscription response: " + 
                      std::string(e.what()));
    }
}

void BinanceWebsocketClient::setOnConnectedCallback(const ConnectionCallback& callback) {
    on_connected_callback_ = callback;
}

void BinanceWebsocketClient::setOnDisconnectedCallback(const ConnectionCallback& callback) {
    on_disconnected_callback_ = callback;
}

} // namespace api
} // namespace market
} // namespace funding_arbitrage
