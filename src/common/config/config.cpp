#include "config.h"
#include <fstream>
#include <stdexcept>

namespace funding_arbitrage {
namespace common {
namespace config {

Config::Config(const std::string& config_path)
    : config_path_(config_path)
    , logger_(std::make_shared<utils::Logger>("Config")) {
    if (!loadConfig()) {
        throw std::runtime_error("Failed to load config file: " + config_path);
    }
}

bool Config::loadConfig() {
    try {
        std::ifstream config_file(config_path_);
        if (!config_file.is_open()) {
            logger_->error("Cannot open config file: " + config_path_);
            return false;
        }

        Json::CharReaderBuilder reader;
        std::string errors;
        if (!Json::parseFromStream(reader, config_file, &root_, &errors)) {
            logger_->error("Failed to parse config file: " + errors);
            return false;
        }

        logger_->info("Successfully loaded config from: " + config_path_);
        return true;
    } catch (const std::exception& e) {
        logger_->error("Exception while loading config: " + std::string(e.what()));
        return false;
    }
}

bool Config::reload() {
    logger_->info("Reloading configuration");
    return loadConfig();
}

std::string Config::getString(const std::string& key, const std::string& default_value) const {
    Json::Value value = getJsonValue(key);
    if (value.isNull()) {
        logger_->debug("Using default value for key: " + key);
        return default_value;
    }
    return value.asString();
}

int Config::getInt(const std::string& key, int default_value) const {
    Json::Value value = getJsonValue(key);
    if (value.isNull()) {
        logger_->debug("Using default value for key: " + key);
        return default_value;
    }
    return value.asInt();
}

double Config::getDouble(const std::string& key, double default_value) const {
    Json::Value value = getJsonValue(key);
    if (value.isNull()) {
        logger_->debug("Using default value for key: " + key);
        return default_value;
    }
    return value.asDouble();
}

bool Config::getBool(const std::string& key, bool default_value) const {
    Json::Value value = getJsonValue(key);
    if (value.isNull()) {
        logger_->debug("Using default value for key: " + key);
        return default_value;
    }
    return value.asBool();
}

std::shared_ptr<Config> Config::getSubConfig(const std::string& key) const {
    Json::Value value = getJsonValue(key);
    if (value.isNull() || !value.isObject()) {
        logger_->warn("No sub-config found for key: " + key);
        return nullptr;
    }

    // 创建临时配置文件
    std::string temp_path = config_path_ + "." + key + ".tmp";
    std::ofstream temp_file(temp_path);
    if (!temp_file.is_open()) {
        logger_->error("Failed to create temporary config file: " + temp_path);
        return nullptr;
    }

    Json::StreamWriterBuilder builder;
    std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
    writer->write(value, &temp_file);
    temp_file.close();

    return std::make_shared<Config>(temp_path);
}

bool Config::hasKey(const std::string& key) const {
    return !getJsonValue(key).isNull();
}

Json::Value Config::getJsonValue(const std::string& key) const {
    std::istringstream iss(key);
    std::string token;
    Json::Value current = root_;

    while (std::getline(iss, token, '.')) {
        if (current.isObject() && current.isMember(token)) {
            current = current[token];
        } else {
            return Json::Value();
        }
    }

    return current;
}

} // namespace config
} // namespace common
} // namespace funding_arbitrage