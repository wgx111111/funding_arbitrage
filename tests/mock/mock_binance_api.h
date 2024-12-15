#pragma once
#include <gmock/gmock.h>
#include "api/binance_api.h"

namespace trading {

class MockBinanceAPI : public BinanceAPI {
public:
    MockBinanceAPI() : BinanceAPI("mock_key", "mock_secret") {}
    
    MOCK_METHOD(double, getFundingRate, 
                (const std::string& symbol), (override));
    MOCK_METHOD(double, getMarkPrice, 
                (const std::string& symbol), (override));
    MOCK_METHOD(bool, placeOrder, 
                (const Order& order), (override));
    MOCK_METHOD(double, getBalance, 
                (const std::string& asset), (override));
};

} // namespace trading