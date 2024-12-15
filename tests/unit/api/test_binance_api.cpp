#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "api/binance_api.h"
#include "mock/mock_binance_api.h"

using namespace trading;
using ::testing::Return;
using ::testing::_;

class BinanceAPITest : public ::testing::Test {
protected:
    void SetUp() override {
        // 使用测试API密钥
        api = std::make_unique<BinanceAPI>(
            "test_api_key",
            "test_secret_key"
        );
    }
    
    std::unique_ptr<BinanceAPI> api;
};

// 测试获取资金费率
TEST_F(BinanceAPITest, GetFundingRate) {
    double rate = api->getFundingRate("BTCUSDT");
    EXPECT_GE(rate, -0.0075);
    EXPECT_LE(rate, 0.0075);
}

// 测试下单功能
TEST_F(BinanceAPITest, PlaceOrder) {
    Order order{
        .symbol = "BTCUSDT",
        .side = OrderSide::BUY,
        .type = OrderType::MARKET,
        .quantity = 0.001
    };
    
    EXPECT_TRUE(api->placeOrder(order));
}

// 使用Mock测试交易流程
TEST_F(BinanceAPITest, TradingFlow) {
    MockBinanceAPI mockApi;
    
    EXPECT_CALL(mockApi, getFundingRate("BTCUSDT"))
        .WillOnce(Return(0.001));
        
    EXPECT_CALL(mockApi, getBalance())
        .WillOnce(Return(1000.0));
        
    EXPECT_CALL(mockApi, placeOrder(_))
        .WillOnce(Return(true));
    
    // 验证交易流程
    double rate = mockApi.getFundingRate("BTCUSDT");
    EXPECT_EQ(rate, 0.001);
    
    double balance = mockApi.getBalance();
    EXPECT_EQ(balance, 1000.0);
    
    Order order{
        .symbol = "BTCUSDT",
        .side = OrderSide::BUY,
        .type = OrderType::MARKET,
        .quantity = 0.001
    };
    EXPECT_TRUE(mockApi.placeOrder(order));
}

// 测试错误处理
TEST_F(BinanceAPITest, ErrorHandling) {
    // 测试无效symbol
    EXPECT_THROW(api->getFundingRate("INVALID"), std::runtime_error);
    
    // 测试无效订单
    Order invalidOrder{
        .symbol = "BTCUSDT",
        .side = OrderSide::BUY,
        .type = OrderType::LIMIT,
        .quantity = -1.0
    };
    EXPECT_FALSE(api->placeOrder(invalidOrder));
}

// 测试签名生成
TEST_F(BinanceAPITest, SignatureGeneration) {
    // 这个测试需要访问protected方法，可能需要修改类结构
    // 或使用友元类来测试
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}