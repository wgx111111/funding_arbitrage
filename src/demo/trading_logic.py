# trading_logic.py
import asyncio
import logging
from datetime import datetime
from typing import List, Dict, Optional, Set
from uuid import uuid4
import json
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
from pathlib import Path
import traceback
from binance.client import Client

from config import Position, TradingConfig
from websocket_manager import WebSocketManager, DataManager
from position_manager import PositionManager

class BinanceFundingTrader:
    def __init__(self, config: TradingConfig):
        self.config = config
        self.client = Client(config.api_key, config.api_secret)
        self.data_manager = DataManager(config.basis_history_size)
        self.position_managers: Dict[str, PositionManager] = {}
        self.monitored_symbols: Set[str] = set()
        self.top_funding_symbols: Set[str] = set()
        self.last_open_time: Dict[str, datetime] = {}
        self.running = False
        self.logger = logging.getLogger(__name__)
        
        # Set up data directories
        self.base_path = Path(__file__).parent
        self.data_dir = self.base_path / 'data'
        self.charts_dir = self.data_dir / 'charts'
        self.trades_dir = self.data_dir / 'trades'
        
        # Create directories
        self.data_dir.mkdir(exist_ok=True)
        self.charts_dir.mkdir(exist_ok=True)
        self.trades_dir.mkdir(exist_ok=True)
        
        self.ws_spot = None
        self.ws_futures = None
        self.symbol_updates = {symbol: {'spot_count': 0, 'futures_count': 0, 'last_update': None} 
                             for symbol in self.monitored_symbols}

    def get_valid_trading_pairs(self) -> List[str]:
        """Get valid trading pairs with proper validation"""
        try:
            self.logger.info("Starting to fetch valid trading pairs...")
            self.logger.info("Fetching futures exchange info...")
            
            futures_info = self.client.futures_exchange_info()
            self.logger.info("Futures exchange info received")
            
            if not futures_info or 'symbols' not in futures_info:
                self.logger.error("Invalid futures exchange info structure")
                raise ValueError("Invalid futures exchange info")
            
            self.logger.info(f"Found {len(futures_info['symbols'])} futures symbols")
            
            # Filter valid futures symbols
            futures_symbols = {s['symbol'] for s in futures_info['symbols'] 
                             if (s['status'] == 'TRADING' and 
                                 s['contractType'] == 'PERPETUAL' and
                                 s['symbol'].endswith('USDT'))}
            
            self.logger.info(f"Filtered {len(futures_symbols)} valid futures symbols")
            
            # Get spot exchange info
            spot_info = self.client.get_exchange_info()
            if not spot_info or 'symbols' not in spot_info:
                self.logger.error("Invalid spot exchange info structure")
                raise ValueError("Invalid spot exchange info")

            valid_pairs = []
            
            for symbol_info in spot_info['symbols']:
                try:
                    symbol = symbol_info['symbol']
                    if (symbol_info['status'] == 'TRADING' and 
                        symbol_info['isSpotTradingAllowed'] and 
                        symbol_info['isMarginTradingAllowed'] and
                        symbol.endswith('USDT') and 
                        symbol in futures_symbols):
                        
                        # Validate spot ticker
                        spot_ticker = self.client.get_symbol_ticker(symbol=symbol)
                        if not spot_ticker or float(spot_ticker['price']) <= 0:
                            continue

                        # Validate futures ticker
                        futures_ticker = self.client.futures_symbol_ticker(symbol=symbol)
                        if not futures_ticker or float(futures_ticker['price']) <= 0:
                            continue

                        valid_pairs.append(symbol)
                        self.logger.info(f"Added valid pair: {symbol}")
                            
                except Exception as e:
                    self.logger.warning(f"Failed to validate {symbol}: {str(e)}")
                    continue
                    
            self.logger.info(f"Found {len(valid_pairs)} valid trading pairs")
            return valid_pairs
            
        except Exception as e:
            self.logger.error(f"Error getting valid trading pairs: {str(e)}")
            return []

    async def get_funding_rates(self, symbol: str, max_retries: int = 3) -> Optional[float]:
        """Get funding rates with retry mechanism"""
        for attempt in range(max_retries):
            try:
                funding_rate = self.client.futures_funding_rate(symbol=symbol, limit=1)
                if funding_rate and len(funding_rate) > 0:
                    return float(funding_rate[0]['fundingRate'])
            except Exception as e:
                self.logger.error(f"Attempt {attempt + 1}/{max_retries} failed: {str(e)}")
                if attempt < max_retries - 1:
                    await asyncio.sleep(2 ** attempt)
        return None

    async def get_top_funding_pairs(self, limit: int = 5) -> List[Dict]:
        """Get top funding rate pairs"""
        try:
            valid_pairs = self.get_valid_trading_pairs()
            funding_data = []
            
            for symbol in valid_pairs:
                funding_rate = await self.get_funding_rates(symbol)
                if funding_rate is not None:
                    abs_rate = abs(funding_rate)
                    funding_data.append({
                        'symbol': symbol,
                        'funding_rate': funding_rate,
                        'abs_rate': abs_rate
                    })
                    await asyncio.sleep(0.1)  # Rate limiting
            
            funding_data.sort(key=lambda x: x['abs_rate'], reverse=True)
            return funding_data[:limit]
            
        except Exception as e:
            self.logger.error(f"Error getting top funding pairs: {str(e)}")
            return []

    async def update_monitored_symbols(self):
        """Update monitored symbols and funding rates"""
        while self.running:
            try:
                current_hour = datetime.utcnow().hour
                self.logger.info(f"Current UTC hour: {current_hour}")
                
                if current_hour in [7, 15, 23]:  # Before funding settlement
                    self.logger.info("Fetching top funding pairs before settlement...")
                    top_pairs = await self.get_top_funding_pairs()
                    self.top_funding_symbols = {p['symbol'] for p in top_pairs}
                    for pair in top_pairs:
                        self.data_manager.update_funding_rate(pair['symbol'], pair['funding_rate'])
                    self.logger.info(f"Updated potential open positions: {self.top_funding_symbols}")
                
                elif current_hour in [8, 16, 0]:  # After funding settlement
                    self.logger.info("Fetching top funding pairs after settlement...")
                    top_pairs = await self.get_top_funding_pairs()
                    new_top_symbols = {p['symbol'] for p in top_pairs}
                    for pair in top_pairs:
                        self.data_manager.update_funding_rate(pair['symbol'], pair['funding_rate'])
                    
                    # Mark positions as settled
                    for manager in self.position_managers.values():
                        await manager.mark_settlement_passed()
                    
                    self.logger.info(f"Updated positions after settlement: {new_top_symbols}")
                    self.top_funding_symbols = new_top_symbols
                
                # Update WebSocket subscriptions
                new_monitored = self.top_funding_symbols | set(self.position_managers.keys())
                if new_monitored != self.monitored_symbols:
                    self.logger.info(f"Updating monitored symbols: {new_monitored}")
                    self.monitored_symbols = new_monitored
                    await self.restart_websockets()
                
                await asyncio.sleep(60)
                
            except Exception as e:
                self.logger.error(f"Error updating symbols: {str(e)}")
                await asyncio.sleep(60)

    def calculate_position_size(self, symbol: str, spot_price: float, futures_price: float) -> float:
        """Calculate position size with risk management"""
        try:
            if not isinstance(spot_price, (int, float)) or not isinstance(futures_price, (int, float)):
                raise ValueError("Invalid price types")
            if spot_price <= 0 or futures_price <= 0:
                raise ValueError("Prices must be positive")

            single_side_cap = self.config.max_capital * self.config.max_position_size
            max_price = max(spot_price, futures_price)
            quantity = single_side_cap / max_price

            # Calculate total exposure
            total_position_value = sum(
                pos.quantity * max(pos.entry_spot_price or 0, pos.entry_futures_price or 0)
                for manager in self.position_managers.values()
                for pos in manager.positions
            )
            
            # Check position limits
            if total_position_value + 2 * single_side_cap > self.config.max_capital * self.config.max_total_positions:
                available_cap = max(0, self.config.max_capital * self.config.max_total_positions - total_position_value)
                single_side_cap = available_cap / 2
                quantity = single_side_cap / max_price
                if quantity <= 0:
                    return 0

            # Round to appropriate precision
            decimals = 8
            return float(f"{quantity:.{decimals}f}")

        except Exception as e:
            self.logger.error(f"Error calculating position size: {str(e)}")
            return 0

    async def process_spot_message(self, message: str):
        """Process spot market WebSocket messages"""
        try:
            data = json.loads(message)
            self.logger.info(f"Raw spot message received: {json.dumps(data)}")
            
            # Handle different message formats
            if isinstance(data, dict):
                ticker_data = data.get('data', data)
                symbol = ticker_data['s']
                price = float(ticker_data['a'])  # Using ask price
                
                # Update symbol tracking
                if symbol in self.symbol_updates:
                    self.symbol_updates[symbol]['spot_count'] += 1
                    self.symbol_updates[symbol]['last_update'] = datetime.now()
                
                # Log update counts for all symbols
                if self.symbol_updates[symbol]['spot_count'] % 10 == 0:  # Log every 10 updates
                    self.logger.info("Symbol update counts:")
                    for sym, counts in self.symbol_updates.items():
                        last_update = counts['last_update'].strftime('%H:%M:%S') if counts['last_update'] else 'Never'
                        self.logger.info(
                            f"{sym}: Spot={counts['spot_count']}, "
                            f"Futures={counts['futures_count']}, "
                            f"Last Update={last_update}"
                        )
                
                self.data_manager.update_price(symbol, price, 'spot')
                await self.update_basis(symbol)
                
        except Exception as e:
            self.logger.error(f"Error processing spot message: {str(e)}\n{traceback.format_exc()}")

    async def process_futures_message(self, message: str):
        """Process futures market WebSocket messages"""
        try:
            data = json.loads(message)
            self.logger.info(f"Raw futures message received: {json.dumps(data)}")
            
            # Handle different message formats
            if isinstance(data, dict):
                ticker_data = data.get('data', data)
                symbol = ticker_data['s']
                price = float(ticker_data['a'])  # Using ask price
                
                # Update symbol tracking
                if symbol in self.symbol_updates:
                    self.symbol_updates[symbol]['futures_count'] += 1
                    self.symbol_updates[symbol]['last_update'] = datetime.now()
                
                # Log update counts for all symbols
                if self.symbol_updates[symbol]['futures_count'] % 10 == 0:  # Log every 10 updates
                    self.logger.info("Symbol update counts:")
                    for sym, counts in self.symbol_updates.items():
                        last_update = counts['last_update'].strftime('%H:%M:%S') if counts['last_update'] else 'Never'
                        self.logger.info(
                            f"{sym}: Spot={counts['spot_count']}, "
                            f"Futures={counts['futures_count']}, "
                            f"Last Update={last_update}"
                        )
                
                self.data_manager.update_price(symbol, price, 'futures')
                await self.update_basis(symbol)
                
        except Exception as e:
            self.logger.error(f"Error processing futures message: {str(e)}\n{traceback.format_exc()}")

    async def update_basis(self, symbol: str):
        """Update basis data and check trading signals"""
        try:
            prices = self.data_manager.get_latest_prices(symbol)
            if not prices:
                return

            spot_price = prices['spot']
            futures_price = prices['futures']

            if spot_price and futures_price:
                basis = (futures_price - spot_price) / spot_price * 100
                timestamp = datetime.now()
                
                basis_data = {
                    'timestamp': timestamp,
                    'basis': basis,
                    'spot_price': spot_price,
                    'futures_price': futures_price
                }
                
                self.data_manager.update_basis(symbol, basis_data)
                # Log basis update
                self.logger.info(
                    f"Updated basis for {symbol}: {basis:.4f}% "
                    f"(spot: {spot_price}, futures: {futures_price})"
                )
                
                await self.check_trading_signals(symbol, basis, spot_price, futures_price)
                
        except Exception as e:
            self.logger.error(f"Error updating basis: {str(e)}\n{traceback.format_exc()}") 

    async def check_trading_signals(self, symbol: str, basis: float, spot_price: float, futures_price: float):
        """Check for trading opportunities"""
        try:
            current_time = datetime.utcnow()
            current_hour = current_time.hour
            
            if current_hour in [7, 15, 23]:  # Before funding settlement
                if symbol in self.top_funding_symbols:
                    last_open = self.last_open_time.get(symbol, datetime.min)
                    time_since_last_open = (current_time - last_open).total_seconds()
                    
                    if time_since_last_open >= self.config.min_open_interval:
                        funding_rate = self.data_manager.get_funding_rate(symbol)
                        if await self.should_open_position(basis, funding_rate):
                            await self.open_position(symbol, basis, funding_rate, 
                                                   spot_price, futures_price)
            
            if symbol in self.position_managers:
                manager = self.position_managers[symbol]
                for position in manager.positions:
                    funding_rate = self.data_manager.get_funding_rate(symbol)
                    if await self.should_close_position(position, basis, funding_rate):
                        await self.close_position(position, basis, funding_rate, 
                                                spot_price, futures_price)
                        
        except Exception as e:
            self.logger.error(f"Error checking trading signals: {str(e)}")

    async def should_open_position(self, basis: float, funding_rate: float) -> bool:
        """Evaluate whether to open a new position"""
        try:
            total_profit = abs(basis) + abs(funding_rate) * 100
            if total_profit <= self.config.trading_cost * 1.5:
                return False
                
            if funding_rate > 0:  # Short futures
                return basis > 0   # Needs positive basis
            else:                 # Long futures
                return basis < 0   # Needs negative basis
                
        except Exception as e:
            self.logger.error(f"Error in should_open_position: {str(e)}")
            return False

    async def should_close_position(self, position: Position, current_basis: float, 
                                  current_funding_rate: float) -> bool:
        """Evaluate whether to close an existing position"""
        try:
            if not position.has_passed_settlement:
                return False
                
            if position.symbol not in self.top_funding_symbols:
                if abs(current_basis) < self.config.trading_cost/2:
                    self.logger.info(
                        f"{position.symbol} out of top5 and basis converged: {current_basis:.4f}"
                    )
                    return True
                    
            return False
            
        except Exception as e:
            self.logger.error(f"Error in should_close_position: {str(e)}")
            return False

    async def open_position(self, symbol: str, basis: float, funding_rate: float, 
                          spot_price: float, futures_price: float):
        """Open a new trading position"""
        try:
            quantity = self.calculate_position_size(symbol, spot_price, futures_price)
            if quantity == 0:
                self.logger.warning(f"Calculated zero position size for {symbol}")
                return

            # Determine position type based on funding rate
            position_type = 'short_basis' if funding_rate > 0 else 'long_basis'

            # Create spot position
            spot_position = Position(
                order_id=str(uuid4()),
                symbol=symbol,
                entry_time=datetime.now(),
                entry_basis=basis,
                entry_funding_rate=funding_rate,
                entry_spot_price=spot_price,
                entry_futures_price=None,
                quantity=quantity,
                position_type=position_type,
                entry_price=spot_price,
                asset_class='spot'
            )

            # Create futures position
            futures_position = Position(
                order_id=str(uuid4()),
                symbol=symbol,
                entry_time=datetime.now(),
                entry_basis=basis,
                entry_funding_rate=funding_rate,
                entry_spot_price=None,
                entry_futures_price=futures_price,
                quantity=quantity,
                position_type=position_type,
                entry_price=futures_price,
                asset_class='futures'
            )

            # Add positions to position manager
            if symbol not in self.position_managers:
                self.position_managers[symbol] = PositionManager(symbol)
            
            manager = self.position_managers[symbol]
            await manager.add_position(spot_position)
            await manager.add_position(futures_position)

            # Update last open time
            self.last_open_time[symbol] = datetime.now()

            # Log trade
            self.logger.info(
                f"Opened {position_type} position for {symbol} | "
                f"Quantity: {quantity:.8f} | Basis: {basis:.4f}% | "
                f"Funding Rate: {funding_rate:.4f}%"
            )

            # Create analysis charts
            await self.plot_entry_analysis(spot_position)

        except Exception as e:
            self.logger.error(f"Error opening position: {str(e)}")

    async def close_position(self, position: Position, exit_basis: float, 
                           exit_funding_rate: float, spot_price: float, 
                           futures_price: float):
        """Close a trading position"""
        try:
            exit_time = datetime.now()
            
            position_manager = self.position_managers.get(position.symbol)
            if not position_manager:
                raise ValueError(f"No position manager found for {position.symbol}")
            
            # Calculate PnL components
            futures_pnl, spot_pnl, funding_pnl = await position_manager.calculate_pnl(
                spot_price, futures_price
            )
            
            # Calculate total PnL
            total_pnl = futures_pnl + spot_pnl + funding_pnl - self.config.trading_cost
            
            # Record trade history
            trade_record = {
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
            }
            
            # Save trade record
            await self.save_trade_record(trade_record)
            
            # Remove position
            await position_manager.remove_position(position)
            if not position_manager.positions:
                del self.position_managers[position.symbol]
            
            # Log trade details
            self.logger.info(
                f"Closed position {position.order_id} for {position.symbol} | "
                f"Futures PnL: {futures_pnl:.4f}% | Spot PnL: {spot_pnl:.4f}% | "
                f"Funding PnL: {funding_pnl:.4f}% | Total PnL: {total_pnl:.4f}%"
            )
            
            # Generate exit analysis
            await self.plot_exit_analysis(position, exit_basis, exit_funding_rate)
            
        except Exception as e:
            self.logger.error(f"Error closing position: {str(e)}")

    async def save_trade_record(self, trade_record: Dict):
        """Save trade record to CSV"""
        try:
            df = pd.DataFrame([trade_record])
            filename = self.trades_dir / f'trade_history_{datetime.now().strftime("%Y%m%d")}.csv'
            
            if filename.exists():
                df.to_csv(filename, mode='a', header=False, index=False)
            else:
                df.to_csv(filename, index=False)
                
        except Exception as e:
            self.logger.error(f"Error saving trade record: {str(e)}")

    async def plot_entry_analysis(self, position: Position):
        """Generate entry analysis plots"""
        try:
            fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 8))
            
            # Plot basis history
            basis_history = self.data_manager.get_basis_history(position.symbol)
            df = pd.DataFrame(basis_history)
            
            ax1.plot(df['timestamp'], df['basis'])
            ax1.axhline(y=0, color='r', linestyle='--')
            ax1.axhline(y=self.config.trading_cost, color='g', linestyle='--', 
                       label=f'Cost {self.config.trading_cost}%')
            ax1.axhline(y=-self.config.trading_cost, color='g', linestyle='--')
            ax1.set_title(f'{position.symbol} Basis History')
            ax1.legend()
            ax1.grid(True)
            
            # Plot funding rates
            funding_data = pd.DataFrame([(s, self.data_manager.get_funding_rate(s))
                                       for s in self.monitored_symbols],
                                      columns=['symbol', 'rate'])
            funding_data['rate'] = funding_data['rate'] * 100
            funding_data = funding_data.sort_values('rate', ascending=False)
            
            ax2.bar(range(len(funding_data)), funding_data['rate'])
            ax2.set_xticks(range(len(funding_data)))
            ax2.set_xticklabels(funding_data['symbol'], rotation=45)
            ax2.set_title('Current Funding Rates (%)')
            ax2.grid(True)
            
            plt.tight_layout()
            plt.savefig(
                self.charts_dir / 
                f'{position.symbol}_entry_{position.entry_time.strftime("%Y%m%d_%H%M%S")}.png'
            )
            plt.close()
            
        except Exception as e:
            self.logger.error(f"Error plotting entry analysis: {str(e)}")

    async def plot_exit_analysis(self, position: Position, exit_basis: float, 
                               exit_funding_rate: float):
        """Generate exit analysis plots"""
        try:
            fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(12, 12))
            
            # Plot basis history
            basis_history = self.data_manager.get_basis_history(position.symbol)
            df = pd.DataFrame(basis_history)
            
            ax1.plot(df['timestamp'], df['basis'])
            ax1.axhline(y=0, color='r', linestyle='--')
            ax1.axvline(x=position.entry_time, color='g', linestyle='--', label='Entry')
            ax1.axvline(x=datetime.now(), color='r', linestyle='--', label='Exit')
            ax1.set_title(f'{position.symbol} Basis History')
            ax1.legend()
            ax1.grid(True)
            
            # Plot price history
            ax2.plot(df['timestamp'], df['spot_price'], label='Spot')
            ax2.plot(df['timestamp'], df['futures_price'], label='Futures')
            ax2.set_title('Price History')
            ax2.legend()
            ax2.grid(True)
            
            # Plot trade history PnL distribution
            trades_file = self.trades_dir / f'trade_history_{datetime.now().strftime("%Y%m%d")}.csv'
            if trades_file.exists():
                trades_df = pd.read_csv(trades_file)
                sns.histplot(data=trades_df['total_pnl'], ax=ax3, bins=20)
                ax3.axvline(x=0, color='r', linestyle='--')
                ax3.set_title('PnL Distribution')
                ax3.grid(True)
            
            plt.tight_layout()
            plt.savefig(
                self.charts_dir / 
                f'{position.symbol}_exit_{datetime.now().strftime("%Y%m%d_%H%M%S")}.png'
            )
            plt.close()
            
        except Exception as e:
            self.logger.error(f"Error plotting exit analysis: {str(e)}")

    async def start_websockets(self):
        """Start WebSocket connections"""
        try:
            if not self.monitored_symbols:
                self.logger.warning("No symbols to monitor")
                return
            
            self.logger.info(f"Starting WebSocket connections for {len(self.monitored_symbols)} symbols")
            
            streams = [f"{symbol.lower()}@bookTicker" for symbol in self.monitored_symbols]
            self.logger.info(f"Preparing to subscribe to streams: {streams}")
           
            spot_url = f"wss://stream.binance.com:9443/stream?streams=" + "/".join(streams)
            futures_url = f"wss://fstream.binance.com/stream?streams=" + "/".join(streams)

            self.logger.info(f"Spot WebSocket URL: {spot_url}")
            self.logger.info(f"Futures WebSocket URL: {futures_url}")

            self.ws_spot = WebSocketManager(
                url=spot_url,
                name="spot",
                on_message=self.process_spot_message
            )
            
            self.ws_futures = WebSocketManager(
                url=futures_url,
                name="futures",
                on_message=self.process_futures_message
            )
            
            await asyncio.gather(
                self.ws_spot.start(),
                self.ws_futures.start()
            )
            
        except Exception as e:
            self.logger.error(f"Error starting websockets: {str(e)}")

    async def restart_websockets(self):
        """Restart WebSocket connections"""
        try:
            if self.ws_spot:
                await self.ws_spot.stop()
            if self.ws_futures:
                await self.ws_futures.stop()
                
            await asyncio.sleep(1)
            await self.start_websockets()
            
        except Exception as e:
            self.logger.error(f"Error restarting websockets: {str(e)}")

    async def start(self):
        """Start the trading system"""
        self.running = True
        self.logger.info(f"Starting trading system with capital: {self.config.max_capital:,.2f} USDT")
        self.logger.info(f"Trading cost: {self.config.trading_cost}%")

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
        """Stop the trading system"""
        self.running = False
        if self.ws_spot:
            await self.ws_spot.stop()
        if self.ws_futures:
            await self.ws_futures.stop()
        self.logger.info("Trading system stopped")