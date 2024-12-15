#include "websocket_event.h"

namespace funding_arbitrage {
namespace market {
namespace api {

WebSocketEvent WebSocketEvent::parse(const std::string& message) {
    WebSocketEvent event;
    event.raw_data = message;
    event.is_valid = false;

    try {
        Json::Reader reader;
        if (!reader.parse(message, event.data)) {
            event.error_message = "Failed to parse JSON";
            return event;
        }

        // 解析事件类型
        if (event.data.isMember("stream")) {
            std::string stream = event.data["stream"].asString();
            if (stream.find("@markPrice") != std::string::npos) {
                event.type = WebSocketEventType::MARK_PRICE;
            } else if (stream.find("@fundingRate") != std::string::npos) {
                event.type = WebSocketEventType::FUNDING_RATE;
            } else if (stream.find("@bookTicker") != std::string::npos) {
                event.type = WebSocketEventType::BOOK_TICKER;
            }
            // 解析symbol
            size_t pos = stream.find("@");
            if (pos != std::string::npos) {
                event.symbol = stream.substr(0, pos);
            }
        }

        // 设置时间戳
        if (event.data.isMember("data") && event.data["data"].isMember("E")) {
            event.timestamp = event.data["data"]["E"].asInt64();
        }

        event.is_valid = true;
        return event;
    } catch (const std::exception& e) {
        event.error_message = std::string("Parse error: ") + e.what();
        return event;
    }
}

bool MarketDataEventHandler::handleEvent(const WebSocketEvent& event) {
    if (!canHandle(event)) {
        return false;
    }

    try {
        auto& data = event.data["data"];
        switch (event.type) {
            case WebSocketEventType::MARK_PRICE:
                if (price_callback_) {
                    price_callback_(
                        event.symbol,
                        std::stod(data["p"].asString()),
                        event.timestamp
                    );
                }
                break;

            case WebSocketEventType::FUNDING_RATE:
                if (funding_callback_) {
                    funding_callback_(
                        event.symbol,
                        std::stod(data["fundingRate"].asString()),
                        event.timestamp
                    );
                }
                break;

            default:
                return false;
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool MarketDataEventHandler::canHandle(const WebSocketEvent& event) const {
    return event.type == WebSocketEventType::MARK_PRICE ||
           event.type == WebSocketEventType::FUNDING_RATE;
}

} // namespace api
} // namespace market
} // namespace funding_arbitrage