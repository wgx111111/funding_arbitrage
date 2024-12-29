# websocket_manager.py
import asyncio
import websockets
import logging
from typing import Optional, Callable, Any
import json
from collections import deque
from threading import Lock
import traceback

class WebSocketManager:
    def __init__(self, url: str, name: str, on_message: Callable, on_error: Optional[Callable] = None, 
                 on_close: Optional[Callable] = None):
        self.url = url
        self.name = name
        self.on_message = on_message
        self.on_error = on_error
        self.on_close = on_close
        self.ws: Optional[websockets.WebSocketClientProtocol] = None
        self.running = False
        self.reconnect_delay = 1
        self.max_reconnect_delay = 30
        self.logger = logging.getLogger(f"websocket.{name}")
        self._lock = asyncio.Lock()

    async def connect(self):
        """Establish WebSocket connection with error handling and reconnection logic"""
        while True:
            try:
                self.logger.info(f"Attempting to connect to {self.url}")
                async with self._lock:
                    # Set a timeout for the initial connection
                    try:
                        self.ws = await asyncio.wait_for(
                            websockets.connect(
                                self.url,
                                ping_interval=20,
                                ping_timeout=60,
                                close_timeout=60,
                                max_size=2**23,
                                compression=None
                            ),
                            timeout=10  # 10 seconds timeout for connection
                        )
                    except asyncio.TimeoutError:
                        self.logger.error(f"Connection timeout for {self.name} WebSocket")
                        raise
                    except Exception as conn_error:
                        self.logger.error(f"Failed to connect to {self.name} WebSocket: {str(conn_error)}")
                        raise

                    self.running = True
                    self.reconnect_delay = 1
                    self.logger.info(f"Successfully connected to {self.name} WebSocket")

                # Test the connection with a ping
                try:
                    pong = await self.ws.ping()
                    await asyncio.wait_for(pong, timeout=5)
                    self.logger.info(f"Initial ping successful for {self.name} WebSocket")
                except Exception as ping_error:
                    self.logger.error(f"Initial ping failed for {self.name} WebSocket: {str(ping_error)}")
                    raise

                while self.running:
                    try:
                        self.logger.debug(f"Waiting for message on {self.name}")
                        message = await asyncio.wait_for(self.ws.recv(), timeout=30)
                        await self.on_message(message)
                    except asyncio.TimeoutError:
                        self.logger.warning(f"{self.name} WebSocket timeout, attempting to ping")
                        try:
                            pong = await self.ws.ping()
                            await asyncio.wait_for(pong, timeout=10)
                            self.logger.debug(f"Ping successful for {self.name}")
                            continue
                        except:
                            self.logger.error(f"Ping failed for {self.name}, reconnecting...")
                            break
                    except websockets.ConnectionClosed as cc:
                        self.logger.error(f"{self.name} WebSocket connection closed: code={cc.code}, reason={cc.reason}")
                        break
                    except Exception as e:
                        self.logger.error(f"Error in {self.name} WebSocket loop: {str(e)}\n{traceback.format_exc()}")
                        if self.on_error:
                            await self.on_error(e)
                        break

            except Exception as e:
                self.logger.error(f"Connection error for {self.name}: {str(e)}\n{traceback.format_exc()}")
                if self.on_error:
                    await self.on_error(e)

            finally:
                await self.cleanup()

            if not self.running:
                break

            self.logger.info(f"Waiting {self.reconnect_delay} seconds before reconnecting {self.name}...")
            await asyncio.sleep(self.reconnect_delay)
            self.reconnect_delay = min(self.reconnect_delay * 2, self.max_reconnect_delay)

    async def cleanup(self):
        """Clean up WebSocket resources"""
        if self.ws:
            try:
                self.logger.info(f"Cleaning up {self.name} WebSocket connection")
                await asyncio.wait_for(self.ws.close(), timeout=5.0)
            except Exception as e:
                self.logger.error(f"Error during cleanup: {str(e)}")
            finally:
                self.ws = None

    async def start(self):
        """Start WebSocket connection with automatic reconnection"""
        self.running = True
        while self.running:
            try:
                await self.connect()
            except Exception as e:
                self.logger.error(f"Error in start: {str(e)}\n{traceback.format_exc()}")
                if self.on_error:
                    await self.on_error(e)
                await asyncio.sleep(self.reconnect_delay)
                self.reconnect_delay = min(self.reconnect_delay * 2, self.max_reconnect_delay)

    async def stop(self):
        """Stop WebSocket connection with proper cleanup"""
        self.logger.info(f"Stopping {self.name} WebSocket")
        self.running = False
        await self.cleanup()
        self.logger.info(f"Stopped {self.name} WebSocket")

class DataManager:
    def __init__(self, max_history: int = 10000):
        self.max_history = max_history
        self.basis_history = {}
        self.price_data = {}
        self.funding_rates = {}
        self._lock = Lock()
        self.logger = logging.getLogger("DataManager")

    def initialize_symbol(self, symbol: str):
        """Initialize data structures for a new symbol"""
        with self._lock:
            if symbol not in self.basis_history:
                self.basis_history[symbol] = deque(maxlen=self.max_history)
                self.price_data[symbol] = {'spot': None, 'futures': None}
                self.funding_rates[symbol] = 0.0
                self.logger.info(f"Initialized data structures for {symbol}")

    def update_price(self, symbol: str, price: float, market_type: str):
        """Update price data for a symbol"""
        with self._lock:
            if symbol not in self.price_data:
                self.initialize_symbol(symbol)
            self.price_data[symbol][market_type] = price
            self.logger.debug(f"Updated {market_type} price for {symbol}: {price}")

    def update_basis(self, symbol: str, basis_data: dict):
        """Update basis history for a symbol"""
        with self._lock:
            if symbol not in self.basis_history:
                self.initialize_symbol(symbol)
            self.basis_history[symbol].append(basis_data)
            self.logger.debug(f"Updated basis for {symbol}: {basis_data}")

    def update_funding_rate(self, symbol: str, rate: float):
        """Update funding rate for a symbol"""
        with self._lock:
            self.funding_rates[symbol] = rate
            self.logger.debug(f"Updated funding rate for {symbol}: {rate}")

    def get_latest_prices(self, symbol: str):
        """Get latest prices for a symbol"""
        with self._lock:
            if symbol in self.price_data:
                return self.price_data[symbol].copy()
            return None

    def get_basis_history(self, symbol: str):
        """Get basis history for a symbol"""
        with self._lock:
            if symbol in self.basis_history:
                return list(self.basis_history[symbol])
            return []

    def get_funding_rate(self, symbol: str):
        """Get current funding rate for a symbol"""
        with self._lock:
            return self.funding_rates.get(symbol, 0.0)