#pragma once
#include "websocket_base.h"
#include "websocket_event.h"
#include "../../common/logger/logger.h"
#include "../../common/utils/rate_limiter.h"
#include "../../common/config/config.h"
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <queue>

namespace funding_arbitrage {
namespace market {
namespace api {

using WebsocketClient = websocketpp::client<websocketpp::config::asio_tls_client>;
using ConnectionHdl = websocketpp::connection_hdl;
using Context = websocketpp::lib::asio::ssl::context;

class BinanceWebsocketClient : public WebsocketBase {
public:
    explicit BinanceWebsocketClient(const std::shared_ptr<config::Config>& config);
    ~BinanceWebsocketClient() override;

    // 禁用拷贝
    BinanceWebsocketClient(const BinanceWebsocketClient&) = delete;
    BinanceWebsocketClient& operator=(const BinanceWebsocketClient&) = delete;

    // WebsocketBase interface implementation
    bool connect() override;
    void disconnect() override;
    bool isConnected() const override;
    bool reconnect() override;

    bool subscribe(const std::string& channel, const MessageCallback& callback) override;
    bool unsubscribe(const std::string& channel) override;
    bool subscribeBatch(const std::vector<std::string>& channels,
                       const MessageCallback& callback) override;

    void setOnConnectedCallback(const ConnectionCallback& callback) override;
    void setOnDisconnectedCallback(const ConnectionCallback& callback) override;

    // 事件处理器管理
    void registerEventHandler(std::shared_ptr<WebSocketEventHandler> handler);
    void removeEventHandler(const std::shared_ptr<WebSocketEventHandler>& handler);
    
    // 便利方法：直接设置市场数据回调
    void setMarkPriceCallback(const MarketDataEventHandler::PriceCallback& callback);
    void setFundingRateCallback(const MarketDataEventHandler::FundingRateCallback& callback);

private:
    // WebSocket配置和状态
    std::string ws_url_;
    WebsocketClient client_;
    ConnectionHdl connection_;
    std::shared_ptr<utils::Logger> logger_;

    // 配置参数
    struct {
        int ping_interval_sec;
        int pong_timeout_sec;
        int max_reconnect_attempts;
        int reconnect_interval_sec;
    } config_;

    // 运行状态
    std::atomic<bool> running_;
    std::atomic<bool> reconnecting_;
    std::thread client_thread_;
    std::mutex mutex_;
    std::condition_variable cv_;

    // 心跳相关
    std::atomic<int64_t> last_pong_time_;
    std::thread heartbeat_thread_;

    // 订阅管理
    std::map<std::string, MessageCallback> callbacks_;
    utils::RateLimiter subscription_limiter_;

    // 事件处理
    std::vector<std::shared_ptr<WebSocketEventHandler>> event_handlers_;
    std::shared_ptr<MarketDataEventHandler> market_data_handler_;
    std::mutex handlers_mutex_;

    // 连接回调
    ConnectionCallback on_connected_callback_;
    ConnectionCallback on_disconnected_callback_;

    // WebSocket事件处理器
    void onOpen(ConnectionHdl hdl);
    void onClose(ConnectionHdl hdl);
    void onFail(ConnectionHdl hdl);
    void onMessage(ConnectionHdl hdl, WebsocketClient::message_ptr msg);
    void onPing(ConnectionHdl hdl, std::string msg);
    void onPong(ConnectionHdl hdl, std::string msg);
    Context::ptr onTlsInit(ConnectionHdl hdl);

    // 内部处理方法
    void handleMessage(const std::string& message) override;
    void processEvent(const WebSocketEvent& event);
    void dispatchEvent(const WebSocketEvent& event);
    void processSubscriptionResponse(const Json::Value& response);

    // 线程管理
    void startClientThread();
    void stopClientThread();
    void startHeartbeat();
    void stopHeartbeat();
    void handleHeartbeat();

    // 订阅管理
    bool resubscribeAll();
    std::string buildSubscriptionMessage(const std::vector<std::string>& channels);
};

} // namespace api
} // namespace market
} // namespace funding_arbitrage