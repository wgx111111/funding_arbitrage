# position_manager.py
from typing import List, Dict, Optional, Tuple
import asyncio
from config import Position
import logging

class PositionManager:
    def __init__(self, symbol: str):
        self.symbol = symbol
        self.positions: List[Position] = []
        self.total_quantity = 0
        self.avg_spot_price = 0
        self.avg_futures_price = 0
        self._lock = asyncio.Lock()
        self.logger = logging.getLogger(__name__)

    async def add_position(self, position: Position):
        """Add a new position with validation and average price updates"""
        async with self._lock:
            try:
                # Validate position
                if position.quantity <= 0:
                    raise ValueError("Position quantity must be positive")
                if position.entry_price <= 0:
                    raise ValueError("Entry price must be positive")

                self.positions.append(position)
                
                # Update weighted average prices
                if position.asset_class == 'spot':
                    old_value = self.total_quantity * self.avg_spot_price
                    new_value = position.quantity * position.entry_price
                    self.total_quantity += position.quantity
                    self.avg_spot_price = (old_value + new_value) / self.total_quantity if self.total_quantity > 0 else 0
                elif position.asset_class == 'futures':
                    old_value = self.total_quantity * self.avg_futures_price
                    new_value = position.quantity * position.entry_price
                    self.total_quantity += position.quantity
                    self.avg_futures_price = (old_value + new_value) / self.total_quantity if self.total_quantity > 0 else 0

            except Exception as e:
                self.logger.error(f"Error adding position: {str(e)}")
                raise

    async def remove_position(self, position: Position):
        """Remove a position and recalculate averages"""
        async with self._lock:
            try:
                if position in self.positions:
                    self.positions.remove(position)
                    await self._recalculate_averages()
            except Exception as e:
                self.logger.error(f"Error removing position: {str(e)}")
                raise

    async def _recalculate_averages(self):
        """Recalculate average prices after position changes"""
        self.total_quantity = 0
        self.avg_spot_price = 0
        self.avg_futures_price = 0
        
        spot_positions = [p for p in self.positions if p.asset_class == 'spot']
        futures_positions = [p for p in self.positions if p.asset_class == 'futures']
        
        if spot_positions:
            total_spot_value = sum(p.quantity * p.entry_price for p in spot_positions)
            total_spot_quantity = sum(p.quantity for p in spot_positions)
            self.avg_spot_price = total_spot_value / total_spot_quantity
            
        if futures_positions:
            total_futures_value = sum(p.quantity * p.entry_price for p in futures_positions)
            total_futures_quantity = sum(p.quantity for p in futures_positions)
            self.avg_futures_price = total_futures_value / total_futures_quantity
            
        self.total_quantity = sum(p.quantity for p in self.positions)

    async def calculate_pnl(self, current_spot_price: float, current_futures_price: float) -> Tuple[float, float, float]:
        """Calculate PnL components for the position"""
        async with self._lock:
            try:
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
                
                # Calculate funding rate PnL
                funding_pnl = sum(
                    abs(pos.entry_funding_rate) * 100 if pos.has_passed_settlement else 0
                    for pos in self.positions
                )
                
                return futures_pnl, spot_pnl, funding_pnl
                
            except Exception as e:
                self.logger.error(f"Error calculating PnL: {str(e)}")
                return 0, 0, 0 
            
    async def get_position_summary(self) -> Dict:
        """Get summary of current positions"""
        async with self._lock:
            return {
                'symbol': self.symbol,
                'total_quantity': self.total_quantity,
                'avg_spot_price': self.avg_spot_price,
                'avg_futures_price': self.avg_futures_price,
                'position_count': len(self.positions),
                'spot_positions': len([p for p in self.positions if p.asset_class == 'spot']),
                'futures_positions': len([p for p in self.positions if p.asset_class == 'futures'])
            }
            
    async def mark_settlement_passed(self):
        """Mark all positions as having passed settlement"""
        async with self._lock:
            for position in self.positions:
                position.has_passed_settlement = True

    async def update_funding_rates(self, new_funding_rate: float):
        """Update funding rates for all positions"""
        async with self._lock:
            for position in self.positions:
                position.entry_funding_rate = new_funding_rate

    async def get_positions_by_type(self, asset_class: str) -> List[Position]:
        """Get all positions of a specific asset class"""
        async with self._lock:
            return [pos for pos in self.positions if pos.asset_class == asset_class]

    async def validate_positions(self) -> bool:
        """Validate position integrity"""
        async with self._lock:
            try:
                # Check spot-futures balance
                spot_positions = await self.get_positions_by_type('spot')
                futures_positions = await self.get_positions_by_type('futures')
                
                if len(spot_positions) != len(futures_positions):
                    self.logger.error(f"Position imbalance detected for {self.symbol}")
                    return False
                
                # Check quantity matching
                spot_quantity = sum(p.quantity for p in spot_positions)
                futures_quantity = sum(p.quantity for p in futures_positions)
                
                if abs(spot_quantity - futures_quantity) > 1e-8:  # Allow for small floating point differences
                    self.logger.error(f"Quantity mismatch detected for {self.symbol}")
                    return False
                
                return True
                
            except Exception as e:
                self.logger.error(f"Error validating positions: {str(e)}")
                return False

    async def total_exposure(self) -> float:
        """Calculate total position exposure"""
        async with self._lock:
            return sum(
                pos.quantity * pos.entry_price
                for pos in self.positions
            )