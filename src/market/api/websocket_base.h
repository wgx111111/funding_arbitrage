#pragma once
#include "../types/execution_types.h"
#include <functional>
#include <string>
#include <vector>

namespace funding_arbitrage {
namespace market {
namespace api {

class WebsocketBase {
public:
    using MessageCallback = std::function<void(const std::string&)>;
    using ConnectionCallback = std::function<void()>;
    
    virtual ~WebsocketBase() = default;

    // 连接管理
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;
    virtual bool reconnect() = 0;

    // 订阅管理
    virtual bool subscribe(const std::string& channel, const MessageCallback& callback) = 0;
    virtual bool unsubscribe(const std::string& channel) = 0;
    virtual bool subscribeBatch(const std::vector<std::string>& channels, 
                              const MessageCallback& callback) = 0;

    // 回调设置
    virtual void setOnConnectedCallback(const ConnectionCallback& callback) = 0;
    virtual void setOnDisconnectedCallback(const ConnectionCallback& callback) = 0;

protected:
    virtual void handleMessage(const std::string& message) = 0;
};

} // namespace api
} // namespace market
} // namespace funding_arbitrage