#pragma once
#include <chrono>
#include <vector>
#include <string>
#include <map>

namespace funding_arbitrage {
namespace strategy {

// 交易统计
struct TradeStats {
    int total_trades;                     // 总交易次数
    int profitable_trades;                // 盈利交易次数
    double win_rate;                     // 胜率
    double avg_profit;                   // 平均利润
    double avg_loss;                     // 平均损失
    double profit_factor;                // 盈利因子
    double sharpe_ratio;                 // 夏普比率
    double max_drawdown;                 // 最大回撤
    double largest_win;                  // 最大单笔盈利
    double largest_loss;                 // 最大单笔亏损
    std::chrono::seconds avg_hold_time;  // 平均持仓时间
};

// 资金管理状态
struct CapitalInfo {
    double initial_capital;              // 初始资金
    double current_capital;              // 当前资金
    double allocated_capital;            // 已分配资金
    double available_capital;            // 可用资金
    double margin_used;                  // 已用保证金
    double unrealized_pnl;              // 未实现盈亏
    double realized_pnl;                // 已实现盈亏
    std::map<std::string, double> allocation;  // 各品种资金分配
};

// 回测结果
struct BacktestResult {
    TradeStats trade_stats;
    CapitalInfo capital_info;
    std::vector<double> equity_curve;
    std::vector<double> drawdown_curve;
    std::map<std::string, double> symbol_returns;
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point end_time;
};

} // namespace strategy
} // namespace funding_arbitrage