#pragma once
#include <string>
#include <chrono>
#include <vector>

namespace funding_arbitrage {
namespace strategy {
namespace types {

// 交易标的信息
struct SymbolInfo {
    std::string symbol;
    double funding_rate;          // 资金费率
    double basis;                // 基差(现货价格-合约价格)
    double basis_ratio;         // 基差率(basis/合约价格)
    double spot_price;          // 现货价格
    double futures_price;       // 合约价格
    double spot_volume_24h;     // 现货24h成交量
    double futures_volume_24h;  // 合约24h成交量
    double spot_liquidity;      // 现货流动性(买卖盘深度)
    double futures_liquidity;   // 合约流动性
    std::chrono::system_clock::time_point next_funding_time;
};

// 策略参数
struct StrategyParams {
    int top_n_symbols{5};               // 选取前N个资金费率最高的标的
    double min_basis_ratio{0.001};      // 最小基差率
    double trading_fee{0.002};          // 总交易手续费(开仓+平仓)
    double min_liquidity_usd{1000000};  // 最小流动性要求(USD)
    double position_size_usd{10000};    // 每个标的的仓位大小(USD)
    int pre_funding_minutes{60};        // 资金费率结算前多少分钟开始开仓
    double profit_take_ratio{0.7};      // 获利了结比例(基差收敛比例)
    double max_loss_ratio{0.003};       // 最大损失率
};

// 仓位信息
struct PositionInfo {
    std::string symbol;
    double spot_size;            // 现货仓位大小
    double futures_size;         // 合约仓位大小
    double spot_entry_price;     // 现货入场价格
    double futures_entry_price;  // 合约入场价格
    double basis_entry;         // 入场基差
    std::chrono::system_clock::time_point entry_time;
    std::chrono::system_clock::time_point funding_time; // 对应的资金费率结算时间
};

// 交易信号
struct Signal {
    std::string symbol;
    bool should_open;           // 是否应该开仓
    bool should_close;          // 是否应该平仓
    double spot_price;          // 现货价格
    double futures_price;       // 合约价格
    double basis;              // 基差
    double basis_ratio;        // 基差率
    double funding_rate;       // 资金费率
    std::string reason;        // 信号原因
    std::chrono::system_clock::time_point time;
};

} // namespace types
} // namespace strategy
} // namespace funding_arbitrage