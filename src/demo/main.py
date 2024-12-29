# main.py
import asyncio
import os
import logging
import sys
from pathlib import Path
from config import ConfigManager
from trading_logic import BinanceFundingTrader

def setup_basic_logging():
    """Setup basic logging configuration"""
    logging.basicConfig(
        level=logging.INFO,
        format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
        handlers=[
            logging.StreamHandler()
        ]
    )
    return logging.getLogger(__name__)

async def main():
    # Setup basic logging first
    logger = setup_basic_logging()
    
    try:
        # Create necessary directories
        Path('logs').mkdir(exist_ok=True)
        Path('data/charts').mkdir(parents=True, exist_ok=True)
        Path('data/trades').mkdir(parents=True, exist_ok=True)

        # Setup config
        config_manager = ConfigManager()
        config = config_manager.load_config()
        
        # Setup detailed logging after config is loaded
        config_manager.setup_logging()
        
        # Initialize trader
        trader = BinanceFundingTrader(config)
        
        try:
            logger.info("Starting trading system...")
            await trader.start()
            
        except KeyboardInterrupt:
            logger.info("Keyboard interrupt received, shutting down...")
            await trader.stop()
            
        except Exception as e:
            logger.error(f"Error in trading system: {str(e)}")
            await trader.stop()
            raise
            
    except Exception as e:
        logger.error(f"Error in initialization: {str(e)}")
        sys.exit(1)

def run():
    """Program entry point"""
    logger = setup_basic_logging()
    
    try:
        if sys.platform == 'win32':
            asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
            
        loop = asyncio.get_event_loop()
        loop.run_until_complete(main())
        
    except KeyboardInterrupt:
        logger.info("Shutdown received...")
        
    except Exception as e:
        logger.error(f"Fatal error: {str(e)}")
        sys.exit(1)
        
    finally:
        if loop and not loop.is_closed():
            loop.close()

if __name__ == "__main__":
    run()