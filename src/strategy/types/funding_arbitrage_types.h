#pragma once
#include <string>
#include <chrono>

namespace funding_arbitrage {
namespace strategy {

// 交易标的信息
struct InstrumentInfo {
    std::string symbol;
    double spot_price;          // 现货价格
    double futures_price;       // 合约价格
    double funding_rate;        // 资金费率
    double funding_fee;         // 预计资金费率费用
    std::chrono::system_clock::time_point next_funding_time;  // 下次结算时间
    double volume_24h;         // 24小时成交量
    double liquidity_score;    // 流动性评分
    double bid_ask_spread;     // 买卖价差
    double basis;              // 基差率 (futures_price - spot_price) / spot_price
};

// 策略参数
struct FundingArbitrageParams {
    // 交易参数
    int top_n_instruments{5};           // 选取前N个资金费率最高的标的
    double min_basis_ratio{0.0008};     // 最小基差比率 (要大于双向手续费)
    double min_funding_rate{0.0001};    // 最小资金费率
    int pre_funding_minutes{60};        // 资金费率结算前多少分钟开始交易
    double position_size_usd{1000.0};   // 每个标的的开仓金额(USD)
    
    // 风险控制参数
    double max_position_per_symbol{0.1};  // 单个标的最大仓位占比
    double max_total_position{0.5};       // 总仓位最大占比
    double min_liquidity_score{0.7};      // 最小流动性评分
    double max_spread_ratio{0.001};       // 最大买卖价差率
    double min_volume_usd{1000000.0};     // 最小24小时成交量(USD)
    int min_market_impact_minutes{5};      // 市场冲击评估时间(分钟)
    
    // 执行参数
    bool use_twap{true};                  // 是否使用TWAP执行
    int twap_intervals{3};                // TWAP分段数
    int execution_timeout_seconds{30};     // 执行超时时间
    double max_slippage{0.001};           // 最大滑点
    
    // 风控阈值
    double stop_loss_ratio{0.005};        // 止损比例
    double profit_take_ratio{0.003};      // 止盈比例
    double max_drawdown{0.02};            // 最大回撤
    double position_imbalance_tolerance{0.01}; // 现货/合约持仓不平衡容忍度
};

// 策略状态
struct FundingArbitrageState {
    bool is_pre_funding_window;    // 是否在资金费率结算前窗口期
    std::vector<InstrumentInfo> active_instruments;  // 活跃交易标的
    std::map<std::string, double> spot_positions;    // 现货持仓
    std::map<std::string, double> futures_positions; // 合约持仓
    double total_pnl;             // 总收益
    double current_drawdown;      // 当前回撤
    std::chrono::system_clock::time_point last_trade_time;
};

} // namespace strategy
} // namespace funding_arbitrage