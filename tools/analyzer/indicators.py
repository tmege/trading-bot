"""Calcul des indicateurs techniques via la librairie 'ta'."""

import pandas as pd
import ta
from ta.momentum import RSIIndicator, StochRSIIndicator, WilliamsRIndicator
from ta.trend import EMAIndicator, SMAIndicator, MACD, ADXIndicator, CCIIndicator
from ta.volatility import BollingerBands, AverageTrueRange, KeltnerChannel
from ta.volume import OnBalanceVolumeIndicator


def compute_indicators(df: pd.DataFrame) -> pd.DataFrame:
    """Ajoute tous les indicateurs techniques au DataFrame.

    Entrée: DataFrame avec colonnes time, open, high, low, close, volume.
    Sortie: même DataFrame enrichi des colonnes indicateurs.
    """
    df = df.copy()
    close = df["close"]
    high = df["high"]
    low = df["low"]
    volume = df["volume"]

    # --- Moyennes mobiles ---
    df["sma_20"] = SMAIndicator(close, window=20).sma_indicator()
    df["sma_50"] = SMAIndicator(close, window=50).sma_indicator()
    df["sma_200"] = SMAIndicator(close, window=200).sma_indicator()
    df["ema_12"] = EMAIndicator(close, window=12).ema_indicator()
    df["ema_26"] = EMAIndicator(close, window=26).ema_indicator()

    # --- RSI ---
    df["rsi"] = RSIIndicator(close, window=14).rsi()

    # --- MACD ---
    macd = MACD(close, window_slow=26, window_fast=12, window_sign=9)
    df["macd"] = macd.macd()
    df["macd_signal"] = macd.macd_signal()
    df["macd_hist"] = macd.macd_diff()

    # --- Bollinger Bands ---
    bb = BollingerBands(close, window=20, window_dev=2)
    df["bb_lower"] = bb.bollinger_lband()
    df["bb_middle"] = bb.bollinger_mavg()
    df["bb_upper"] = bb.bollinger_hband()
    df["bb_width"] = bb.bollinger_wband()

    # --- ATR ---
    df["atr"] = AverageTrueRange(high, low, close, window=14).average_true_range()

    # --- ADX ---
    adx = ADXIndicator(high, low, close, window=14)
    df["adx"] = adx.adx()
    df["plus_di"] = adx.adx_pos()
    df["minus_di"] = adx.adx_neg()

    # --- StochRSI ---
    stoch = StochRSIIndicator(close, window=14, smooth1=3, smooth2=3)
    df["stoch_rsi_k"] = stoch.stochrsi_k() * 100  # Normaliser 0-100
    df["stoch_rsi_d"] = stoch.stochrsi_d() * 100

    # --- Keltner Channels ---
    kc = KeltnerChannel(high, low, close, window=20, window_atr=10)
    df["kc_lower"] = kc.keltner_channel_lband()
    df["kc_middle"] = kc.keltner_channel_mband()
    df["kc_upper"] = kc.keltner_channel_hband()

    # --- CCI ---
    df["cci"] = CCIIndicator(high, low, close, window=20).cci()

    # --- Williams %R ---
    df["williams_r"] = WilliamsRIndicator(high, low, close, lbp=14).williams_r()

    # --- OBV ---
    df["obv"] = OnBalanceVolumeIndicator(close, volume).on_balance_volume()
    df["obv_sma"] = SMAIndicator(df["obv"], window=20).sma_indicator()

    # --- Colonnes dérivées ---
    df["ema_trend"] = (df["ema_12"] > df["ema_26"]).astype(int) * 2 - 1  # 1 = bull, -1 = bear

    bb_range = df["bb_upper"] - df["bb_lower"]
    df["bb_position"] = ((df["close"] - df["bb_lower"]) / bb_range).clip(0, 1)

    # bb_squeeze: BB inside Keltner
    df["bb_squeeze"] = ((df["bb_lower"] > df["kc_lower"]) & (df["bb_upper"] < df["kc_upper"])).astype(int)

    df["atr_pct"] = df["atr"] / df["close"]

    # --- Volume avancé ---
    df["vol_sma_20"] = SMAIndicator(volume, window=20).sma_indicator()
    df["vol_ratio"] = volume / df["vol_sma_20"]  # >2 = spike, <0.5 = dry up

    # --- ATR percentile (adaptatif) ---
    df["atr_pct_rank"] = df["atr_pct"].rolling(100).rank(pct=True)

    # --- EMA distance (mean reversion) ---
    df["ema12_dist_pct"] = (close - df["ema_12"]) / df["ema_12"] * 100
    df["sma20_dist_pct"] = (close - df["sma_20"]) / df["sma_20"] * 100

    # --- Candle patterns ---
    body = (close - df["open"]).abs()
    full_range = high - low
    upper_wick = high - close.where(close > df["open"], df["open"])
    lower_wick = close.where(close < df["open"], df["open"]) - low

    df["candle_body_pct"] = body / full_range  # 0=doji, 1=full body
    df["bullish_engulf"] = (
        (close > df["open"]) &                          # current green
        (close.shift(1) < df["open"].shift(1)) &        # prev red
        (close > df["open"].shift(1)) &                 # close above prev open
        (df["open"] < close.shift(1)) &                 # open below prev close
        (body > body.shift(1))                          # bigger body
    ).astype(int)
    df["bearish_engulf"] = (
        (close < df["open"]) &
        (close.shift(1) > df["open"].shift(1)) &
        (close < df["open"].shift(1)) &
        (df["open"] > close.shift(1)) &
        (body > body.shift(1))
    ).astype(int)
    df["hammer"] = (
        (lower_wick > body * 2) &
        (upper_wick < body * 0.5) &
        (body > 0)
    ).astype(int)
    df["shooting_star"] = (
        (upper_wick > body * 2) &
        (lower_wick < body * 0.5) &
        (body > 0)
    ).astype(int)
    df["doji"] = (df["candle_body_pct"] < 0.1).astype(int)

    # --- Consecutive candles ---
    green = (close > df["open"]).astype(int)
    red = (close < df["open"]).astype(int)
    df["consec_green"] = green * (green.groupby((green != green.shift()).cumsum()).cumcount() + 1)
    df["consec_red"] = red * (red.groupby((red != red.shift()).cumsum()).cumcount() + 1)

    # --- MACD momentum ---
    df["macd_hist_accel"] = df["macd_hist"] - df["macd_hist"].shift(1)
    df["macd_hist_increasing"] = (
        (df["macd_hist"] > df["macd_hist"].shift(1)) &
        (df["macd_hist"].shift(1) > df["macd_hist"].shift(2))
    ).astype(int)
    df["macd_hist_decreasing"] = (
        (df["macd_hist"] < df["macd_hist"].shift(1)) &
        (df["macd_hist"].shift(1) < df["macd_hist"].shift(2))
    ).astype(int)

    # --- RSI divergence (5 bars lookback) ---
    lb = 5
    price_lower_low = close < close.rolling(lb).min().shift(1)
    rsi_higher_low = df["rsi"] > df["rsi"].rolling(lb).min().shift(1)
    df["rsi_bull_div"] = (price_lower_low & rsi_higher_low).astype(int)

    price_higher_high = close > close.rolling(lb).max().shift(1)
    rsi_lower_high = df["rsi"] < df["rsi"].rolling(lb).max().shift(1)
    df["rsi_bear_div"] = (price_higher_high & rsi_lower_high).astype(int)

    # --- Range compression ---
    df["range_pct"] = full_range / close * 100
    df["range_pct_rank"] = df["range_pct"].rolling(50).rank(pct=True)

    # --- DI crossover ---
    df["di_bull"] = (df["plus_di"] > df["minus_di"]).astype(int)
    df["di_bear"] = (df["plus_di"] < df["minus_di"]).astype(int)

    return df
