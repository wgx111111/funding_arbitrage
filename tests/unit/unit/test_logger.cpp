#include <gtest/gtest.h>
#include <fstream>
#include <regex>
#include "utils/logger.h"

using namespace funding_arbitrage::utils;

class LoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        logger = std::make_unique<Logger>("test_logger");
    }

    void TearDown() override {
        // 清理日志文件
        std::filesystem::remove("log/test_logger.log");
    }

    bool findInLogFile(const std::string& pattern) {
        std::ifstream logFile("log/test_logger.log");
        std::string line;
        std::regex reg(pattern);
        
        while (std::getline(logFile, line)) {
            if (std::regex_search(line, reg)) {
                return true;
            }
        }
        return false;
    }

    std::unique_ptr<Logger> logger;
};

TEST_F(LoggerTest, BasicLogging) {
    const std::string testMessage = "Test log message";
    
    logger->debug(testMessage);
    logger->info(testMessage);
    logger->warn(testMessage);
    logger->error(testMessage);
    
    EXPECT_TRUE(findInLogFile("\\[DEBUG\\].*" + testMessage));
    EXPECT_TRUE(findInLogFile("\\[INFO\\].*" + testMessage));
    EXPECT_TRUE(findInLogFile("\\[WARNING\\].*" + testMessage));
    EXPECT_TRUE(findInLogFile("\\[ERROR\\].*" + testMessage));
}

TEST_F(LoggerTest, LogLevelFiltering) {
    const std::string testMessage = "Test log filtering";
    
    logger->setLevel(Logger::Level::WARN);
    
    logger->debug(testMessage);
    logger->info(testMessage);
    logger->warn(testMessage);
    logger->error(testMessage);
    
    EXPECT_FALSE(findInLogFile("\\[DEBUG\\].*" + testMessage));
    EXPECT_FALSE(findInLogFile("\\[INFO\\].*" + testMessage));
    EXPECT_TRUE(findInLogFile("\\[WARNING\\].*" + testMessage));
    EXPECT_TRUE(findInLogFile("\\[ERROR\\].*" + testMessage));
}

TEST_F(LoggerTest, LoggerNameTest) {
    const std::string loggerName = "test_logger";
    EXPECT_EQ(logger->getName(), loggerName);
}

TEST_F(LoggerTest, MultipleInstances) {
    auto logger1 = std::make_unique<Logger>("logger1");
    auto logger2 = std::make_unique<Logger>("logger2");
    
    const std::string message1 = "Message from logger1";
    const std::string message2 = "Message from logger2";
    
    logger1->info(message1);
    logger2->info(message2);
    
    EXPECT_TRUE(std::filesystem::exists("log/logger1.log"));
    EXPECT_TRUE(std::filesystem::exists("log/logger2.log"));
}