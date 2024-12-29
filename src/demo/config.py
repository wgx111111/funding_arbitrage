# config.py
import os
from dataclasses import dataclass
from typing import Optional
import yaml
from pathlib import Path
import logging
from datetime import datetime

# config.py
import os
from dataclasses import dataclass
from typing import Optional
import yaml
from pathlib import Path
import logging
from datetime import datetime

@dataclass
class TradingConfig:
    api_key: str
    api_secret: str
    max_capital: float
    trading_cost: float = 0.1  # 0.1%
    min_open_interval: int = 10  # seconds
    max_position_size: float = 0.005  # 0.5% of capital
    max_total_positions: float = 0.9  # 90% of capital
    basis_history_size: int = 10000
    websocket_timeout: int = 60
    max_retries: int = 3
    log_level: str = "INFO"

class ConfigManager:
    def __init__(self):
        self.config: Optional[TradingConfig] = None
        # Get the directory where config.py is located
        current_dir = Path(__file__).parent
        self.config_path = current_dir / 'config' / 'trading_config.yml'

    def load_config(self) -> TradingConfig:
        """Load configuration from environment variables or config file"""
        # Priority 1: Environment variables
        api_key = os.getenv('BINANCE_API_KEY')
        api_secret = os.getenv('BINANCE_API_SECRET')
        max_capital = float(os.getenv('MAX_CAPITAL', '1000000'))
        
        # If environment variables not set, try config file
        if not (api_key and api_secret):
            if self.config_path.exists():
                print(f"Loading config from: {self.config_path}")  # Debug print
                with open(self.config_path) as f:
                    config_data = yaml.safe_load(f)
                api_key = config_data.get('api_key')
                api_secret = config_data.get('api_secret')
                max_capital = float(config_data.get('max_capital', 1000000))
            else:
                print(f"Config file not found at: {self.config_path}")  # Debug print
                raise ValueError(f"No configuration found. Set environment variables or create config file at {self.config_path}")

        self.config = TradingConfig(
            api_key=api_key,
            api_secret=api_secret,
            max_capital=max_capital
        )
        return self.config

    def setup_logging(self):
        """Setup logging configuration"""
        # Get the directory where config.py is located
        current_dir = Path(__file__).parent
        log_dir = current_dir / 'logs'
        log_dir.mkdir(exist_ok=True)

        # Main logger
        logging.basicConfig(
            level=getattr(logging, self.config.log_level),
            format='%(asctime)s - %(levelname)s - %(message)s',
            handlers=[
                logging.FileHandler(log_dir / f'trading_{datetime.now().strftime("%Y%m%d")}.log'),
                logging.StreamHandler()
            ]
        )

        # Trade logger
        trade_logger = logging.getLogger('trades')
        trade_handler = logging.FileHandler(log_dir / f'trades_{datetime.now().strftime("%Y%m%d")}.log')
        trade_handler.setFormatter(logging.Formatter('%(asctime)s - %(message)s'))
        trade_logger.addHandler(trade_handler)
        trade_logger.setLevel(logging.INFO)

        # Basis logger
        basis_logger = logging.getLogger('basis')
        basis_handler = logging.FileHandler(log_dir / f'basis_{datetime.now().strftime("%Y%m%d")}.log')
        basis_handler.setFormatter(logging.Formatter('%(asctime)s - %(message)s'))
        basis_logger.addHandler(basis_handler)
        basis_logger.setLevel(logging.INFO)

@dataclass
class Position:
    order_id: str
    symbol: str
    entry_time: datetime
    entry_basis: float
    entry_funding_rate: float
    entry_spot_price: Optional[float]
    entry_futures_price: Optional[float]
    quantity: float
    position_type: str  # 'long_basis' or 'short_basis'
    entry_price: float
    asset_class: str    # 'spot' or 'futures'
    has_passed_settlement: bool = False

    def __post_init__(self):
        """Validate position data"""
        if self.quantity <= 0:
            raise ValueError("Quantity must be positive")
        if self.entry_price <= 0:
            raise ValueError("Entry price must be positive")
        if self.asset_class not in ['spot', 'futures']:
            raise ValueError("Invalid asset class")
        if self.position_type not in ['long_basis', 'short_basis']:
            raise ValueError("Invalid position type")