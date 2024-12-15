// src/config/config.h
#pragma once
#include <string>
#include <memory>
#include <map>
#include <any>
#include <json/json.h>
#include "../logger/logger.h"

namespace funding_arbitrage {
namespace common {
namespace config {

class Config {
public:
    explicit Config(const std::string& config_path);
    
    // 基础类型获取方法
    std::string getString(const std::string& key, const std::string& default_value = "") const;
    int getInt(const std::string& key, int default_value = 0) const;
    double getDouble(const std::string& key, double default_value = 0.0) const;
    bool getBool(const std::string& key, bool default_value = false) const;

    // 获取子配置
    std::shared_ptr<Config> getSubConfig(const std::string& key) const;

    // 判断key是否存在
    bool hasKey(const std::string& key) const;

    // 重新加载配置
    bool reload();

private:
    std::string config_path_;
    Json::Value root_;
    std::shared_ptr<utils::Logger> logger_;

    bool loadConfig();
    Json::Value getJsonValue(const std::string& key) const;
};

} // namespace config
} // namespace common
} // namespace funding_arbitrage