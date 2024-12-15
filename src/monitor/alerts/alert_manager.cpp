#include "alert_manager.h"
#include <algorithm>
#include <sstream>
#include <curl/curl.h>

namespace funding_arbitrage {
namespace monitor {
namespace alerts {

AlertManager::AlertManager(const std::shared_ptr<config::Config>& config)
    : logger_(std::make_shared<Logger>("AlertManager")) {
    
    auto alert_config = config->getSubConfig("monitor.alerts");
    if (!alert_config) {
        throw std::runtime_error("Missing alert configuration");
    }

    // 加载告警配置
    config_.email_enabled = alert_config->getBool("email.enabled", false);
    config_.slack_enabled = alert_config->getBool("slack.enabled", false);
    config_.telegram_enabled = alert_config->getBool("telegram.enabled", false);
    config_.email_recipients = alert_config->getString("email.recipients", "");
    config_.slack_webhook = alert_config->getString("slack.webhook", "");
    config_.telegram_bot_token = alert_config->getString("telegram.bot_token", "");
    config_.telegram_chat_id = alert_config->getString("telegram.chat_id", "");
    config_.alert_interval_seconds = alert_config->getInt("interval_seconds", 300);
    config_.max_alerts_per_hour = alert_config->getInt("max_alerts_per_hour", 100);

    logger_->info("AlertManager initialized");
}

void AlertManager::sendAlert(const Alert& alert) {
    if (shouldThrottleAlert(alert)) {
        logger_->debug("Alert throttled: " + alert.message);
        return;
    }

    try {
        // 记录告警
        {
            std::lock_guard<std::mutex> lock(alerts_mutex_);
            
            // 更新活动告警
            auto key = std::make_pair(alert.type, alert.source);
            active_alerts_[key] = alert;
            
            // 添加到历史记录
            alert_history_.push_back(alert);
            
            // 更新计数器
            alert_counts_[alert.type]++;
            last_alert_times_[alert.type] = std::chrono::system_clock::now();
        }

        // 发送告警
        if (config_.email_enabled && 
            alert.level >= AlertLevel::WARNING) {
            sendEmailAlert(alert);
        }
        
        if (config_.slack_enabled && 
            alert.level >= AlertLevel::ERROR) {
            sendSlackAlert(alert);
        }
        
        if (config_.telegram_enabled && 
            alert.level >= AlertLevel::CRITICAL) {
            sendTelegramAlert(alert);
        }

        // 触发回调
        if (alert_callback_) {
            try {
                alert_callback_(alert);
            } catch (const std::exception& e) {
                logger_->error("Error in alert callback: " + std::string(e.what()));
            }
        }

        logger_->info("Alert sent: [" + std::to_string(static_cast<int>(alert.type)) + 
                     "] " + alert.message);
        
        // 清理历史记录
        cleanupAlertHistory();

    } catch (const std::exception& e) {
        logger_->error("Failed to send alert: " + std::string(e.what()));
    }
}

void AlertManager::resolveAlert(AlertType type, const std::string& source) {
    std::lock_guard<std::mutex> lock(alerts_mutex_);
    
    auto key = std::make_pair(type, source);
    auto it = active_alerts_.find(key);
    
    if (it != active_alerts_.end()) {
        it->second.is_resolved = true;
        it->second.resolve_time = std::chrono::system_clock::now();
        active_alerts_.erase(it);
        
        logger_->info("Alert resolved: [" + std::to_string(static_cast<int>(type)) + 
                     "] from " + source);
    }
}

bool AlertManager::shouldThrottleAlert(const Alert& alert) {
    std::lock_guard<std::mutex> lock(alerts_mutex_);
    
    auto now = std::chrono::system_clock::now();
    
    // 检查告警间隔
    auto last_time_it = last_alert_times_.find(alert.type);
    if (last_time_it != last_alert_times_.end()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_time_it->second).count();
        
        if (elapsed < config_.alert_interval_seconds) {
            return true;
        }
    }
    
    // 检查每小时告警数量限制
    if (alert_counts_[alert.type] >= config_.max_alerts_per_hour) {
        return true;
    }
    
    return false;
}

void AlertManager::cleanupAlertHistory() {
    std::lock_guard<std::mutex> lock(alerts_mutex_);
    
    auto now = std::chrono::system_clock::now();
    auto one_day_ago = now - std::chrono::hours(24);
    
    // 清理24小时前的历史记录
    alert_history_.erase(
        std::remove_if(
            alert_history_.begin(),
            alert_history_.end(),
            [one_day_ago](const Alert& alert) {
                return alert.time < one_day_ago;
            }
        ),
        alert_history_.end()
    );
    
    // 重置计数器
    updateAlertCounts();
}

void AlertManager::updateAlertCounts() {
    auto now = std::chrono::system_clock::now();
    auto one_hour_ago = now - std::chrono::hours(1);
    
    std::map<AlertType, int> new_counts;
    
    for (const auto& alert : alert_history_) {
        if (alert.time >= one_hour_ago) {
            new_counts[alert.type]++;
        }
    }
    
    alert_counts_ = std::move(new_counts);
}

void AlertManager::sendEmailAlert(const Alert& alert) {
    if (config_.email_recipients.empty()) {
        return;
    }

    std::stringstream body;
    body << "Alert Type: " << static_cast<int>(alert.type) << "\n"
         << "Level: " << static_cast<int>(alert.level) << "\n"
         << "Source: " << alert.source << "\n"
         << "Message: " << alert.message << "\n"
         << "Details: " << alert.details << "\n"
         << "Current Value: " << alert.current_value << "\n"
         << "Threshold: " << alert.threshold << "\n";

    // TODO: 实现实际的邮件发送逻辑
    logger_->info("Email alert would be sent to: " + config_.email_recipients);
}

void AlertManager::sendSlackAlert(const Alert& alert) {
    if (config_.slack_webhook.empty()) {
        return;
    }

    // 构建 Slack 消息
    Json::Value message;
    message["text"] = "Trading Alert";
    
    Json::Value attachment;
    attachment["color"] = alert.level >= AlertLevel::CRITICAL ? "#FF0000" : "#FFA500";
    attachment["title"] = "Alert: " + alert.message;
    attachment["text"] = alert.details;
    attachment["fields"].append(Json::Value());
    attachment["fields"][0]["title"] = "Type";
    attachment["fields"][0]["value"] = static_cast<int>(alert.type);
    attachment["fields"][0]["short"] = true;
    
    message["attachments"].append(attachment);

    // 发送到 Slack
    CURL* curl = curl_easy_init();
    if (curl) {
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        
        Json::FastWriter writer;
        std::string json_message = writer.write(message);
        
        curl_easy_setopt(curl, CURLOPT_URL, config_.slack_webhook.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_message.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            logger_->error("Failed to send Slack alert: " + 
                          std::string(curl_easy_strerror(res)));
        }
        
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
}

void AlertManager::sendTelegramAlert(const Alert& alert) {
    if (config_.telegram_bot_token.empty() || config_.telegram_chat_id.empty()) {
        return;
    }

    // 构建消息
    std::stringstream message;
    message << "🚨 *Trading Alert*\n"
            << "Type: " << static_cast<int>(alert.type) << "\n"
            << "Level: " << static_cast<int>(alert.level) << "\n"
            << "Message: " << alert.message << "\n"
            << "Details: " << alert.details;

    // 发送到Telegram
    std::string url = "https://api.telegram.org/bot" + config_.telegram_bot_token +
                     "/sendMessage";
    
    CURL* curl = curl_easy_init();
    if (curl) {
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        
        Json::Value json_message;
        json_message["chat_id"] = config_.telegram_chat_id;
        json_message["text"] = message.str();
        json_message["parse_mode"] = "Markdown";
        
        Json::FastWriter writer;
        std::string json_str = writer.write(json_message);
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            logger_->error("Failed to send Telegram alert: " + 
                          std::string(curl_easy_strerror(res)));
        }
        
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
}

std::vector<Alert> AlertManager::getActiveAlerts() const {
    std::lock_guard<std::mutex> lock(alerts_mutex_);
    std::vector<Alert> alerts;
    alerts.reserve(active_alerts_.size());
    
    for (const auto& [_, alert] : active_alerts_) {
        alerts.push_back(alert);
    }
    
    return alerts;
}

std::vector<Alert> AlertManager::getAlertHistory(
    const std::chrono::system_clock::time_point& start_time,
    const std::chrono::system_clock::time_point& end_time) const {
    std::lock_guard<std::mutex> lock(alerts_mutex_);
    std::vector<Alert> alerts;
    
    for (const auto& alert : alert_history_) {
        if (alert.time >= start_time && alert.time <= end_time) {
            alerts.push_back(alert);
        }
    }
    
    return alerts;
}

int AlertManager::getAlertCount(AlertType type) const {
    std::lock_guard<std::mutex> lock(alerts_mutex_);
    auto it = alert_counts_.find(type);
    return it != alert_counts_.end() ? it->second : 0;
}

bool AlertManager::isAlertThrottled(AlertType type) const {
    return shouldThrottleAlert(Alert{.type = type});
}

void AlertManager::setAlertCallback(std::function<void(const Alert&)> callback) {
    alert_callback_ = std::move(callback);
}

void AlertManager::setAlertConfig(const AlertConfig& config) {
    config_ = config;
    logger_->info("Alert configuration updated");
}

} // namespace alerts
} // namespace monitor
} // namespace funding_arbitrage