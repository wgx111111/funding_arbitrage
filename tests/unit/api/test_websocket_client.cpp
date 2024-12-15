#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "api/websocket_client.h"

using namespace trading;
using namespace std::chrono_literals;

class WebSocketClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        client = std::make_unique<WebSocketClient>();
    }
    
    void TearDown() override {
        if (client && client->isConnected()) {
            client->disconnect();
        }
    }
    
    std::unique_ptr<WebSocketClient> client;
};

TEST_F(WebSocketClientTest, ConnectTest) {
    EXPECT_TRUE(client->connect(
        "wss://stream.binance.com:9443/ws"));
    EXPECT_TRUE(client->isConnected());
}

TEST_F(WebSocketClientTest, SubscribeTest) {
    ASSERT_TRUE(client->connect(
        "wss://stream.binance.com:9443/ws"));

    bool messageReceived = false;
    std::condition_variable cv;
    std::mutex mtx;

    client->subscribe("btcusdt@markPrice", 
        [&](const std::string& msg) {
            std::lock_guard<std::mutex> lock(mtx);
            messageReceived = true;
            cv.notify_one();
        });

    // 等待接收消息
    std::unique_lock<std::mutex> lock(mtx);
    EXPECT_TRUE(cv.wait_for(lock, 5s, [&]() {
        return messageReceived;
    }));
}

TEST_F(WebSocketClientTest, UnsubscribeTest) {
    ASSERT_TRUE(client->connect(
        "wss://stream.binance.com:9443/ws"));

    std::atomic<int> messageCount{0};
    
    client->subscribe("btcusdt@markPrice", 
        [&](const std::string& msg) {
            messageCount++;
        });

    // 等待一些消息
    std::this_thread::sleep_for(2s);
    int count1 = messageCount;

    // 取消订阅
    EXPECT_TRUE(client->unsubscribe("btcusdt@markPrice"));
    
    // 再等待一段时间
    std::this_thread::sleep_for(2s);
    int count2 = messageCount;

    // 确保不再收到新消息
    EXPECT_EQ(count1, count2);
}

TEST_F(WebSocketClientTest, ReconnectionTest) {
    ASSERT_TRUE(client->connect(
        "wss://stream.binance.com:9443/ws"));

    // 模拟断开连接
    client->disconnect();
    EXPECT_FALSE(client->isConnected());

    // 重新连接
    EXPECT_TRUE(client->connect(
        "wss://stream.binance.com:9443/ws"));
    EXPECT_TRUE(client->isConnected());
}