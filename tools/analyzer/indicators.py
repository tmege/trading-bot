"""Calcul des indicateurs techniques via la librairie 'ta'."""

import pandas as pd
import ta
from ta.momentum import RSIIndicator, StochRSIIndicator, WilliamsRIndicator
from ta.trend import EMAIndicator, SMAIndicator, MACD, ADXIndicator, CCIIndicator
from ta.volatility import BollingerBands, AverageTrueRange, KeltnerChannel
from ta.volume import OnBalanceVolumeIndicator, ChaikinMoneyFlowIndicator, MFIIndicator
from ta.trend import PSARIndicator


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

    # --- CMF (Chaikin Money Flow) ---
    df["cmf"] = ChaikinMoneyFlowIndicator(high, low, close, volume, window=20).chaikin_money_flow()

    # --- MFI (Money Flow Index) ---
    df["mfi"] = MFIIndicator(high, low, close, volume, window=14).money_flow_index()

    # --- Squeeze Momentum (LazyBear) ---
    # Squeeze = BB inside KC
    kc_sq = KeltnerChannel(high, low, close, window=20, window_atr=10, multiplier=1.5)
    kc_sq_lower = kc_sq.keltner_channel_lband()
    kc_sq_upper = kc_sq.keltner_channel_hband()
    bb_sq = BollingerBands(close, window=20, window_dev=2)
    bb_sq_lower = bb_sq.bollinger_lband()
    bb_sq_upper = bb_sq.bollinger_hband()
    df["squeeze_on"] = ((bb_sq_lower > kc_sq_lower) & (bb_sq_upper < kc_sq_upper)).astype(int)

    # Momentum = linear regression of (close - midline)
    dc_high = high.rolling(20).max()
    dc_low = low.rolling(20).min()
    midline = (df["sma_20"] + (dc_high + dc_low) / 2) / 2
    delta = close - midline
    # Simple approximation: use rolling mean of delta as momentum proxy
    df["squeeze_mom"] = delta.rolling(20).mean()

    # --- ROC (Rate of Change, 12 periods) ---
    df["roc"] = close.pct_change(12) * 100

    # --- Z-Score (20 periods) ---
    sma20_zs = close.rolling(20).mean()
    std20 = close.rolling(20).std()
    df["zscore"] = (close - sma20_zs) / std20.replace(0, float("nan"))

    # --- FVG (Fair Value Gap) ---
    df["fvg_bull"] = (high.shift(2) < low).astype(int)
    df["fvg_bear"] = (low.shift(2) > high).astype(int)
    fvg_bull_size = (low - high.shift(2)) / ((low + high.shift(2)) / 2) * 100
    fvg_bear_size = (low.shift(2) - high) / ((low.shift(2) + high) / 2) * 100
    df["fvg_size"] = fvg_bull_size.where(df["fvg_bull"] == 1, fvg_bear_size.where(df["fvg_bear"] == 1, 0))

    # --- Supertrend (ATR 10, mult 3.0) ---
    atr_st = AverageTrueRange(high, low, close, window=10).average_true_range()
    hl2 = (high + low) / 2
    upper_band = hl2 + 3.0 * atr_st
    lower_band = hl2 - 3.0 * atr_st
    supertrend = pd.Series(0.0, index=df.index)
    st_up = pd.Series(True, index=df.index)
    prev_upper = upper_band.iloc[0] if len(upper_band) > 0 else 0
    prev_lower = lower_band.iloc[0] if len(lower_band) > 0 else 0
    prev_uptrend = True
    for i in range(1, len(df)):
        cur_upper = upper_band.iloc[i]
        cur_lower = lower_band.iloc[i]
        prev_close = close.iloc[i - 1]
        if cur_upper < prev_upper or prev_close > prev_upper:
            prev_upper = cur_upper
        else:
            cur_upper = prev_upper
        if cur_lower > prev_lower or prev_close < prev_lower:
            prev_lower = cur_lower
        else:
            cur_lower = prev_lower
        if prev_uptrend:
            if close.iloc[i] < cur_lower:
                prev_uptrend = False
        else:
            if close.iloc[i] > cur_upper:
                prev_uptrend = True
        supertrend.iloc[i] = cur_lower if prev_uptrend else cur_upper
        st_up.iloc[i] = prev_uptrend
        prev_upper = cur_upper
        prev_lower = cur_lower
    df["supertrend"] = supertrend
    df["supertrend_up"] = st_up.astype(int)

    # --- Parabolic SAR ---
    psar_ind = PSARIndicator(high, low, close, step=0.02, max_step=0.20)
    df["psar"] = psar_ind.psar()
    df["psar_up"] = (close > df["psar"]).astype(int)

    return df
