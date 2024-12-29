import asyncio
import websockets
from binance.client import Client
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import seaborn as sns
from datetime import datetime, timedelta
import time
import logging
import json
from typing import List, Dict, Set, Optional, Tuple
from dataclasses import dataclass, field
from collections import defaultdict
import threading
from pathlib import Path
from uuid import uuid4
import traceback

@dataclass
class Position:
    order_id: str
    symbol: str
    entry_time: datetime
    entry_basis: float
    entry_funding_rate: float
    entry_spot_price: float
    entry_futures_price: float
    quantity: float
    position_type: str
    entry_price: float  # 统一使用entry_price
    asset_class: str  # spot or futures    
    has_passed_settlement: bool = False

class PositionManager:
    def __init__(self, symbol: str):
        self.symbol = symbol
        self.positions: List[Position] = []
        self.total_quantity = 0
        self.avg_spot_price = 0
        self.avg_futures_price = 0
        self.entry_spot_price
    
    def add_position(self, position: Position):
        self.positions.append(position)
        # 更新加权平均价格
        if position.asset_class == 'spot':
            old_value = self.total_quantity * self.avg_spot_price
            new_value = position.quantity * position.entry_price
            self.total_quantity += position.quantity
            self.avg_spot_price = (old_value + new_value) / self.total_quantity
        elif position.asset_class == 'futures':
            old_value = self.total_quantity * self.avg_futures_price
            new_value = position.quantity * position.entry_price
            self.total_quantity += position.quantity
            self.avg_futures_price = (old_value + new_value) / self.total_quantity

    def calculate_pnl(self, current_spot_price: float, current_futures_price: float):
        spot_positions = [pos for pos in self.positions if pos.asset_class == 'spot']
        futures_positions = [pos for pos in self.positions if pos.asset_class == 'futures']
        
        if not spot_positions or not futures_positions:
            return 0, 0, 0
        
        if self.positions[0].position_type == 'long_basis':
            futures_pnl = (current_futures_price - self.avg_futures_price) / self.avg_futures_price * 100
            spot_pnl = (current_spot_price - self.avg_spot_price) / self.avg_spot_price * 100
        else:  # short_basis
            futures_pnl = (self.avg_futures_price - current_futures_price) / self.avg_futures_price * 100
            spot_pnl = (self.avg_spot_price - current_spot_price) / self.avg_spot_price * 100
        
        # 计算累积的资金费率收益
        funding_pnl = sum(
            abs(pos.entry_funding_rate) * 100 if pos.has_passed_settlement else 0
            for pos in self.positions
        )
        
        return futures_pnl, spot_pnl, funding_pnl

class WebSocketManager:
    def __init__(self, url: str, name: str, on_message, on_error=None, on_close=None):
        self.url = url
        self.name = name
        self.on_message = on_message
        self.on_error = on_error
        self.on_close = on_close
        self.ws = None
        self.running = False
        self.reconnect_delay = 1
        self.max_reconnect_delay = 30
        self.logger = logging.getLogger(f"websocket.{name}")
        self._lock = asyncio.Lock()  # Add lock for thread safety

    async def connect(self):
        try:
            async with self._lock:  # Use lock for connection management
                self.ws = await asyncio.wait_for(
                    websockets.connect(
                        self.url,
                        ping_interval=20,
                        ping_timeout=60,
                        close_timeout=60,
                        max_size=2**23,  # Increase message size limit
                        compression=None  # Disable compression for better performance
                    ),
                    timeout=60
                )
                self.running = True
                self.reconnect_delay = 1

            while self.running:
                try:
                    message = await asyncio.wait_for(self.ws.recv(), timeout=30)
                    await self.on_message(message)
                except asyncio.TimeoutError:
                    self.logger.warning(f"{self.name} WebSocket timeout, reconnecting...")
                    break
                except websockets.ConnectionClosed:
                    self.logger.warning(f"{self.name} WebSocket connection closed, reconnecting...")
                    break
                except Exception as e:
                    self.logger.error(f"Error in WebSocket loop: {str(e)}")
                    if self.on_error:
                        await self.on_error(e)
                    break

        except Exception as e:
            self.logger.error(f"Connection error: {str(e)}")
            if self.on_error:
                await self.on_error(e)

        finally:
            if self.ws:
                try:
                    await asyncio.wait_for(self.ws.close(), timeout=5.0)
                except:
                    pass
                self.ws = None

    async def stop(self):
        """Improved websocket stopping with proper cleanup"""
        self.running = False
        if self.ws:
            try:
                async with self._lock:
                    await asyncio.wait_for(self.ws.close(), timeout=5.0)
            except asyncio.TimeoutError:
                self.logger.error("Failed to close websocket gracefully")
            except Exception as e:
                self.logger.error(f"Error closing websocket: {str(e)}")
            finally:
                self.ws = None

class BinanceFundingTrader:
    def __init__(self, api_key: str = '', api_secret: str = '', max_capital: float = 1000000):
        self.client = Client(api_key, api_secret)
        self.max_capital = max_capital
        self.position_managers = {}  # symbol -> PositionManager
        self.monitored_symbols: Set[str] = set()
        self.price_data = defaultdict(lambda: {'spot': None, 'futures': None})
        self.funding_rates = {}
        self.basis_history = defaultdict(list)
        self.trade_history = []
        self.top_funding_symbols = set()
        self.trading_cost = 0.1  # 0.1%
        self.running = False
        self.last_open_time = defaultdict(lambda: datetime.min)  # 记录每个标的最后开仓时间
        self.min_open_interval = 10  # 最小开仓间隔(秒)
        self.setup_logging()
        self.setup_data_dir()

        # 设置事件循环
        try:
            self.loop = asyncio.get_event_loop()

        except RuntimeError:
            self.loop = asyncio.new_event_loop()
            asyncio.set_event_loop(self.loop)

        self.ws_spot = None
        self.ws_futures = None

    def setup_logging(self):
        log_dir = Path('logs')
        log_dir.mkdir(exist_ok=True)

        logging.basicConfig(
            level=logging.INFO,
            format='%(asctime)s - %(levelname)s - %(message)s',
            handlers=[
                logging.FileHandler(f'logs/trading_{datetime.now().strftime("%Y%m%d")}.log'),
                logging.StreamHandler()
            ]
        )
        self.logger = logging.getLogger(__name__)

        self.trade_logger = logging.getLogger('trades')
        trade_handler = logging.FileHandler(f'logs/trades_{datetime.now().strftime("%Y%m%d")}.log')
        trade_handler.setFormatter(logging.Formatter('%(asctime)s - %(message)s'))
        self.trade_logger.addHandler(trade_handler)
        self.trade_logger.setLevel(logging.INFO)

        self.basis_logger = logging.getLogger('basis')
        basis_handler = logging.FileHandler(f'logs/basis_{datetime.now().strftime("%Y%m%d")}.log')
        basis_handler.setFormatter(logging.Formatter('%(asctime)s - %(message)s'))
        self.basis_logger.addHandler(basis_handler)
        self.basis_logger.setLevel(logging.INFO)

    def setup_data_dir(self):
        self.data_dir = Path('data')
        self.data_dir.mkdir(exist_ok=True)
        (self.data_dir / 'charts').mkdir(exist_ok=True)
        (self.data_dir / 'trades').mkdir(exist_ok=True)

    def get_valid_trading_pairs(self) -> List[str]:
        """获取同时支持现货、合约和杠杆的交易对"""
        try:
            # 获取合约交易对信息
            futures_info = self.client.futures_exchange_info()
            if not futures_info or 'symbols' not in futures_info:
                self.logger.error("Failed to get futures exchange info")
                return []
                
            futures_symbols = {s['symbol'] for s in futures_info['symbols'] 
                            if (s['status'] == 'TRADING' and 
                                s['contractType'] == 'PERPETUAL' and
                                s['symbol'].endswith('USDT'))}
            
            # 获取现货和杠杆交易对
            spot_info = self.client.get_exchange_info()
            if not spot_info or 'symbols' not in spot_info:
                self.logger.error("Failed to get spot exchange info")
                return []

            valid_pairs = []
            
            for symbol_info in spot_info['symbols']:
                try:
                    symbol = symbol_info['symbol']
                    if (symbol_info['status'] == 'TRADING' and 
                        symbol_info['isSpotTradingAllowed'] and 
                        symbol_info['isMarginTradingAllowed'] and
                        symbol.endswith('USDT') and 
                        symbol in futures_symbols):
                        
                        # 验证是否能获取价格
                        spot_ticker = self.client.get_symbol_ticker(symbol=symbol)
                        futures_ticker = self.client.futures_symbol_ticker(symbol=symbol)
                        
                        if spot_ticker and futures_ticker:
                            valid_pairs.append(symbol)
                            self.logger.info(f"Added valid pair: {symbol}")
                            time.sleep(0.1)  # 避免请求过快
                            
                except Exception as e:
                    self.logger.warning(f"Failed to validate {symbol}: {str(e)}")
                    continue
                    
            self.logger.info(f"Found {len(valid_pairs)} valid trading pairs")
            return valid_pairs
            
        except Exception as e:
            self.logger.error(f"Error getting valid trading pairs: {str(e)}")
            return []

    def get_funding_rates(self, symbol: str) -> float:
        """获取资金费率"""
        try:
            funding_rate = self.client.futures_funding_rate(symbol=symbol, limit=1)
            if funding_rate:
                return float(funding_rate[0]['fundingRate'])
            return 0
        except Exception as e:
            self.logger.error(f"Error getting funding rate for {symbol}: {str(e)}")
            return 0
       
    def calculate_position_size(self, symbol: str, spot_price: float, futures_price: float) -> float:
        try:
            single_side_cap = self.max_capital * 0.005  # 0.5% of max capital per side
            max_price = max(spot_price, futures_price)
            quantity = single_side_cap / max_price

            # Calculate total position value
            total_position_value = sum(pos.quantity * max(pos.entry_spot_price, pos.entry_futures_price)
                                    for manager in self.position_managers.values()
                                    for pos in manager.positions)
            
            # Check if adding new position exceeds 90% of max capital
            if total_position_value + 2 * single_side_cap > self.max_capital * 0.9:
                available_cap = self.max_capital * 0.9 - total_position_value
                single_side_cap = available_cap / 2
                quantity = single_side_cap / max_price
                if quantity <= 0:
                    return 0
            else:
                quantity = single_side_cap / max_price

            # Round down to suitable precision
            decimals = 8  # adjust based on symbol precision
            return float(f"{quantity:.{decimals}f}")
        except Exception as e:
            self.logger.error(f"Error calculating position size: {str(e)}")
            return 0 

    async def process_spot_message(self, message: str):
        """处理现货WebSocket消息"""
        try:
            data = json.loads(message)
            if 'data' in data:
                ticker_data = data['data']
                symbol = ticker_data['s']
                price = float(ticker_data['a'])  # 使用卖一价
                self.price_data[symbol]['spot'] = price
                await self.update_basis(symbol)
        except Exception as e:
            self.logger.error(f"Error processing spot message: {str(e)}\nMessage: {message}")

    async def process_futures_message(self, message: str):
        """处理合约WebSocket消息"""
        try:
            data = json.loads(message)
            if 'data' in data:
                ticker_data = data['data']
                symbol = ticker_data['s']
                price = float(ticker_data['a'])  # 使用卖一价
                self.price_data[symbol]['futures'] = price
                await self.update_basis(symbol)
        except Exception as e:
            self.logger.error(f"Error processing futures message: {str(e)}\nMessage: {message}")

    async def update_basis(self, symbol: str):
        """更新基差数据"""
        spot_price = self.price_data[symbol]['spot']
        futures_price = self.price_data[symbol]['futures']

        if spot_price and futures_price:
            basis = (futures_price - spot_price) / spot_price * 100
            timestamp = datetime.now()
            
            self.basis_history[symbol].append({
                'timestamp': timestamp,
                'basis': basis,
                'spot_price': spot_price,
                'futures_price': futures_price
            })
            
            self.basis_logger.info(
                f"{symbol},{timestamp},{basis:.4f},{spot_price},{futures_price}"
            )
            
            if len(self.basis_history[symbol]) > 10000:  # 保留约1天的数据
                self.basis_history[symbol] = self.basis_history[symbol][-5000:]
            
            await self.check_trading_signals(symbol, basis, spot_price, futures_price)

    async def check_trading_signals(self, symbol: str, basis: float, spot_price: float, 
                                 futures_price: float):
        """检查交易信号"""
        try:
            current_time = datetime.utcnow()
            current_hour = current_time.hour
            
            # 在资金费率结算前一小时检查开仓机会
            if current_hour in [7, 15, 23]:
                # 检查是否在资金费率前5
                if symbol in self.top_funding_symbols:
                    # 检查最小开仓时间间隔
                    time_since_last_open = (current_time - self.last_open_time[symbol]).total_seconds()
                    if time_since_last_open >= self.min_open_interval:
                        funding_rate = self.funding_rates.get(symbol, 0)
                        if await self.should_open_position(basis, funding_rate):
                            await self.open_position(symbol, basis, funding_rate, 
                                                    spot_price, futures_price)
            
            # 检查平仓信号
            if symbol in self.position_managers:
                for position in self.position_managers[symbol].positions:
                    funding_rate = self.funding_rates.get(symbol, 0)
                    if await self.should_close_position(position, basis, funding_rate):
                        await self.close_position(position, basis, funding_rate, 
                                                spot_price, futures_price)
                        
        except Exception as e:
            self.logger.error(f"Error checking trading signals: {str(e)}\n{traceback.format_exc()}")

    async def should_open_position(self, basis: float, funding_rate: float) -> bool:
        """判断是否应该开仓"""
        # 检查收益是否覆盖成本
        total_profit = abs(basis) + abs(funding_rate) * 100
        if total_profit <= self.trading_cost * 1.5:
            return False
            
        # 检查方向是否一致
        if funding_rate > 0:  # 做空合约
            return basis > 0  # 需要基差为正
        else:  # 做多合约
            return basis < 0  # 需要基差为负 

    async def should_close_position(self, position: Position, current_basis: float, 
                                 current_funding_rate: float) -> bool:
        """判断是否应该平仓"""
        # 必须经过至少一次结算
        if not position.has_passed_settlement:
            return False
            
        # 只有当标的跌出资金费率前5，且基差收敛时才平仓
        if position.symbol not in self.top_funding_symbols:
            if abs(current_basis) < self.trading_cost/2:  # 基差收敛到交易成本一半
                self.logger.info(
                    f"{position.symbol} out of top5 and basis converged: {current_basis:.4f}"
                )
                return True

        return False
    
    async def open_position(self, symbol: str, basis: float, funding_rate: float, 
                        spot_price: float, futures_price: float):
        try:
            quantity = self.calculate_position_size(symbol, spot_price, futures_price)
            if quantity == 0:
                return

            # Determine position type based on funding rate
            if funding_rate > 0:
                position_type = 'short_basis'  # short futures, long spot
            else:
                position_type = 'long_basis'   # long futures, short spot

            # Create new positions for spot and futures
            spot_position = Position(
                order_id=str(uuid4()),
                symbol=symbol,
                entry_time=datetime.now(),
                entry_basis=basis,
                entry_funding_rate=funding_rate,
                entry_price=spot_price,
                entry_spot_price=spot_price,  # 设置现货价格
                entry_futures_price=None,     # 期货价格不适用
                quantity=quantity,
                position_type=position_type,
                asset_class='spot'
            )

            futures_position = Position(
                order_id=str(uuid4()),
                symbol=symbol,
                entry_time=datetime.now(),
                entry_basis=basis,
                entry_funding_rate=funding_rate,
                entry_price=futures_price,
                entry_spot_price=None,        # 现货价格不适用
                entry_futures_price=futures_price,  # 设置期货价格
                quantity=quantity,
                position_type=position_type,
                asset_class='futures'
            )

            # Add positions to position manager
            if symbol not in self.position_managers:
                self.position_managers[symbol] = PositionManager(symbol)
            manager = self.position_managers[symbol]
            manager.add_position(spot_position)
            manager.add_position(futures_position)

            # Update last open time
            self.last_open_time[symbol] = datetime.now()

            # Record opening information
            self.trade_logger.info(
                f"OPEN,{spot_position.order_id},{symbol},{spot_position.entry_time},"
                f"{basis:.4f},{funding_rate:.4f},{position_type},{quantity:.8f},"
                f"{spot_price:.8f},{futures_price:.8f}"
            )

            self.logger.info(
                f"Opened position for {symbol} | Type: {position_type} | Quantity: {quantity:.8f} | "
                f"Basis: {basis:.4f}% | Funding Rate: {funding_rate:.4f}%"
            )

            await self.plot_entry_analysis(spot_position)
        except Exception as e:
            self.logger.error(f"Error opening position: {str(e)}\n{traceback.format_exc()}")

    async def close_position(self, position: Position, exit_basis: float, 
                         exit_funding_rate: float, spot_price: float, 
                         futures_price: float):
        try:
            exit_time = datetime.now()
            
            # 获取该标的的仓位管理器
            position_manager = self.position_managers.get(position.symbol)
            if not position_manager:
                self.logger.error(f"No position manager found for {position.symbol}")
                return
            
            # 计算收益
            futures_pnl, spot_pnl, funding_pnl = position_manager.calculate_pnl(
                spot_price, futures_price
            )
            
            # 总收益 = 合约收益 + 现货收益 + 资金费率收益 - 交易成本
            total_pnl = futures_pnl + spot_pnl + funding_pnl - self.trading_cost
            
            # 记录交易历史
            self.trade_history.append({
                'symbol': position.symbol,
                'order_id': position.order_id,
                'entry_time': position.entry_time,
                'exit_time': exit_time,
                'entry_basis': position.entry_basis,
                'exit_basis': exit_basis,
                'entry_funding_rate': position.entry_funding_rate,
                'exit_funding_rate': exit_funding_rate,
                'quantity': position.quantity,
                'entry_price': position.entry_price,
                'exit_spot_price': spot_price,
                'exit_futures_price': futures_price,
                'futures_pnl': futures_pnl,
                'spot_pnl': spot_pnl,
                'funding_pnl': funding_pnl,
                'total_pnl': total_pnl
            })
            
            # 从仓位管理器中移除
            position_manager.positions.remove(position)
            if not position_manager.positions:
                del self.position_managers[position.symbol]
            
            # 记录平仓信息
            self.trade_logger.info(
                f"CLOSE,{position.order_id},{position.symbol},{exit_time},"
                f"{exit_basis:.4f},{exit_funding_rate:.4f},"
                f"{futures_pnl:.4f},{spot_pnl:.4f},{funding_pnl:.4f},{total_pnl:.4f}"
            )
            
            self.logger.info(
                f"Closed position {position.order_id} for {position.symbol} | "
                f"Futures PnL: {futures_pnl:.4f}% | Spot PnL: {spot_pnl:.4f}% | "
                f"Funding PnL: {funding_pnl:.4f}% | Total PnL: {total_pnl:.4f}%"
            )
            
            await self.plot_exit_analysis(position, exit_basis, exit_funding_rate)
            await self.update_trade_history()
        except Exception as e:
            self.logger.error(f"Error closing position: {str(e)}\n{traceback.format_exc()}")

    async def plot_entry_analysis(self, position: Position):
       """绘制开仓分析图表"""
       try:
           fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 8))
           
           # 基差历史
           basis_data = pd.DataFrame(self.basis_history[position.symbol])
           ax1.plot(basis_data['timestamp'], basis_data['basis'])
           ax1.axhline(y=0, color='r', linestyle='--')
           ax1.axhline(y=self.trading_cost, color='g', linestyle='--', 
                      label=f'Cost {self.trading_cost}%')
           ax1.axhline(y=-self.trading_cost, color='g', linestyle='--')
           ax1.set_title(f'{position.symbol} Basis History')
           ax1.legend()
           ax1.grid(True)
           
           # 资金费率排名
           funding_data = pd.DataFrame(list(self.funding_rates.items()),
                                     columns=['symbol', 'rate'])
           funding_data['rate'] = funding_data['rate'] * 100
           funding_data = funding_data.sort_values('rate', ascending=False)
           ax2.bar(range(len(funding_data)), funding_data['rate'])
           ax2.set_xticks(range(len(funding_data)))
           ax2.set_xticklabels(funding_data['symbol'], rotation=45)
           ax2.set_title('Current Funding Rates (%)')
           ax2.grid(True)
           
           plt.tight_layout()
           plt.savefig(f'data/charts/{position.symbol}_entry_{position.entry_time.strftime("%Y%m%d_%H%M%S")}.png')
           plt.close()
           
       except Exception as e:
           self.logger.error(f"Error plotting entry analysis: {str(e)}")

    async def plot_exit_analysis(self, position: Position, exit_basis: float, 
                              exit_funding_rate: float):
        """绘制平仓分析图表"""
        try:
            fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(12, 12))
            
            # 基差历史
            basis_data = pd.DataFrame(self.basis_history[position.symbol])
            ax1.plot(basis_data['timestamp'], basis_data['basis'])
            ax1.axhline(y=0, color='r', linestyle='--')
            ax1.axvline(x=position.entry_time, color='g', linestyle='--', label='Entry')
            ax1.axvline(x=datetime.now(), color='r', linestyle='--', label='Exit')
            ax1.set_title(f'{position.symbol} Basis History')
            ax1.legend()
            ax1.grid(True)
            
            # 价格走势
            ax2.plot(basis_data['timestamp'], basis_data['spot_price'], label='Spot')
            ax2.plot(basis_data['timestamp'], basis_data['futures_price'], label='Futures')
            ax2.set_title('Price History')
            ax2.legend()
            ax2.grid(True)
            
            # 收益分布
            trade_df = pd.DataFrame(self.trade_history)
            if not trade_df.empty:
                sns.histplot(data=trade_df['total_pnl'], ax=ax3, bins=20)
                ax3.axvline(x=0, color='r', linestyle='--')
                ax3.set_title('PnL Distribution')
                ax3.grid(True)
            
            plt.tight_layout()
            plt.savefig(f'data/charts/{position.symbol}_exit_{datetime.now().strftime("%Y%m%d_%H%M%S")}.png')
            plt.close()
            
        except Exception as e:
            self.logger.error(f"Error plotting exit analysis: {str(e)}")    

    async def update_trade_history(self):
       """更新交易历史"""
       try:
           df = pd.DataFrame(self.trade_history)
           if not df.empty:
               filename = f'data/trades/trade_history_{datetime.now().strftime("%Y%m%d")}.csv'
               df.to_csv(filename, index=False)
               self.logger.info(f"Trade history updated: {filename}")
       except Exception as e:
           self.logger.error(f"Error updating trade history: {str(e)}")
        
    async def get_top_funding_pairs(self, limit: int = 5) -> List[Dict]:
        try:
            valid_pairs = self.get_valid_trading_pairs()
            funding_data = []
            
            for symbol in valid_pairs:
                funding_rate = self.get_funding_rates(symbol)
                if funding_rate != 0:  # 确保能获取到资金费率
                    abs_rate = abs(funding_rate)  # 取绝对值
                    funding_data.append({
                        'symbol': symbol,
                        'funding_rate': funding_rate,  # 保留原始值（带正负）
                        'abs_rate': abs_rate  # 用于排序的绝对值
                    })
            
            # 用绝对值排序，但保留原始资金费率的正负
            funding_data.sort(key=lambda x: x['abs_rate'], reverse=True)
            return funding_data[:limit]
            
        except Exception as e:
            self.logger.error(f"Error getting top funding pairs: {str(e)}")
            return []

    async def update_monitored_symbols(self):
        """更新监控的交易对"""
        while self.running:
            try:
                current_hour = datetime.utcnow().hour
                
                # 在资金费率结算前一小时(7,15,23点)更新开仓标的
                if current_hour in [7, 15, 23]:
                    top_pairs = await self.get_top_funding_pairs()
                    self.top_funding_symbols = {p['symbol'] for p in top_pairs}
                    for pair in top_pairs:
                        self.funding_rates[pair['symbol']] = pair['funding_rate']
                    self.logger.info(f"Updated potential open positions: {self.top_funding_symbols}")
                
                # 在资金费率结算时(8,16,0点)更新平仓标的
                elif current_hour in [8, 16, 0]:
                    top_pairs = await self.get_top_funding_pairs()
                    new_top_symbols = {p['symbol'] for p in top_pairs}
                    for pair in top_pairs:
                        self.funding_rates[pair['symbol']] = pair['funding_rate']
                    # 标记经过结算
                    for manager in self.position_managers.values():
                        for position in manager.positions:
                            position.has_passed_settlement = True
                    
                    self.logger.info(f"Updated positions after settlement: {new_top_symbols}")
                    self.top_funding_symbols = new_top_symbols
                
                # 更新WebSocket订阅
                new_monitored = self.top_funding_symbols | {p.symbol for manager in self.position_managers.values() for p in manager.positions}
                if new_monitored != self.monitored_symbols:
                    self.monitored_symbols = new_monitored
                    await self.restart_websockets()
                
                await asyncio.sleep(60)  # 每分钟检查一次
                
            except Exception as e:
                self.logger.error(f"Error updating symbols: {str(e)}")
                await asyncio.sleep(60)

    async def start_websockets(self):
        """启动WebSocket连接"""
        try:
            if not self.monitored_symbols:
                self.logger.warning("No symbols to monitor")
                return

            # 构建多个标的的订阅
            streams = [f"{symbol.lower()}@bookTicker" for symbol in self.monitored_symbols]
            self.logger.info(f"Preparing to subscribe to streams: {streams}")
            
            spot_url = f"wss://stream.binance.com:9443/stream?streams=" + "/".join(streams)
            futures_url = f"wss://fstream.binance.com/stream?streams=" + "/".join(streams)

            self.logger.info("Creating WebSocket connections...")
            
            self.ws_spot = WebSocketManager(
                url=spot_url,
                name="spot",
                on_message=self.process_spot_message,
                on_error=self.handle_websocket_error,
                on_close=self.handle_websocket_close
            )
            
            self.ws_futures = WebSocketManager(
                url=futures_url,
                name="futures",
                on_message=self.process_futures_message,
                on_error=self.handle_websocket_error,
                on_close=self.handle_websocket_close
            )
            
            self.logger.info("Starting WebSocket connections...")
            await asyncio.gather(
                self.ws_spot.start(),
                self.ws_futures.start()
            )
            
        except Exception as e:
            self.logger.error(f"Error starting websockets: {str(e)}")

    async def restart_websockets(self):
        """重启WebSocket连接"""
        try:
            if self.ws_spot:
                self.ws_spot.stop()
            if self.ws_futures:
                self.ws_futures.stop()
            await asyncio.sleep(1)
            await self.start_websockets()
        except Exception as e:
            self.logger.error(f"Error restarting websockets: {str(e)}")

    async def handle_websocket_error(self, error):
        """处理WebSocket错误"""
        self.logger.error(f"WebSocket error: {error}")

    async def handle_websocket_close(self):
        """处理WebSocket关闭"""
        self.logger.info("WebSocket connection closed")

    async def start(self):
        """启动策略"""
        self.running = True
        self.logger.info(f"Starting trading system with capital: {self.max_capital:,.2f} USDT")
        self.logger.info(f"Trading cost: {self.trading_cost}%")

        try:
            await asyncio.gather(
                self.update_monitored_symbols(),
                self.start_websockets()
            )
        except Exception as e:
            self.logger.error(f"Error in main loop: {str(e)}")
        finally:
            await self.stop()

    async def stop(self):
        """停止策略"""
        self.running = False
        if self.ws_spot:
            self.ws_spot.stop()
        if self.ws_futures:
            self.ws_futures.stop()

        await self.update_trade_history()
        self.logger.info("Trading system stopped")

def run():
    """程序入口"""
    async def main():
        trader = BinanceFundingTrader(
            api_key='eFZBPSXPd1VW8yYgAJajtd2KFqzlPZIji3hFTZxxA715dBgXlpJGC2wLJoxEFmnh',
            api_secret='rGLlUKkBKCeTBpwUaiUvvDnRAmpRdcQVrYDI3oYM5VzbyhESupUQxuB1xI08gfCI',
            max_capital=1000000
        )

        await trader.start()

    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass

if __name__ == "__main__":
    run()