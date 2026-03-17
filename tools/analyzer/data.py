"""Chargement des bougies depuis SQLite et agrégation multi-timeframe."""

import sqlite3
import pandas as pd
import os

DB_PATH = os.path.join(os.path.dirname(__file__), "..", "..", "data", "candle_cache.db")

TF_MINUTES = {
    "5m": 5,
    "15m": 15,
    "1h": 60,
    "4h": 240,
    "1d": 1440,
}


def load_candles(coin: str, interval: str = "5m", start_ms: int = None, end_ms: int = None) -> pd.DataFrame:
    """Charge les bougies depuis candle_cache.db.

    Retourne un DataFrame avec colonnes: time, open, high, low, close, volume.
    """
    db_path = os.path.abspath(DB_PATH)
    conn = sqlite3.connect(db_path)

    query = "SELECT time_ms, open, high, low, close, volume FROM candles WHERE coin = ? AND interval = ?"
    params = [coin, interval]

    if start_ms is not None:
        query += " AND time_ms >= ?"
        params.append(start_ms)
    if end_ms is not None:
        query += " AND time_ms <= ?"
        params.append(end_ms)

    query += " ORDER BY time_ms ASC"

    df = pd.read_sql_query(query, conn, params=params)
    conn.close()

    df.rename(columns={"time_ms": "time"}, inplace=True)
    df["time"] = pd.to_datetime(df["time"], unit="ms")
    return df


def aggregate_tf(df_5m: pd.DataFrame, target_tf: str) -> pd.DataFrame:
    """Agrège des bougies 5m vers un TF supérieur (1h, 4h, 1d).

    Même logique que backtest_engine.c: aggregate_5m_into_tf().
    Groupby floor(time / tf_ms) * tf_ms → OHLCV standard.
    """
    if target_tf == "5m":
        return df_5m.copy()

    minutes = TF_MINUTES.get(target_tf)
    if minutes is None:
        raise ValueError(f"Timeframe inconnu: {target_tf}. Valides: {list(TF_MINUTES.keys())}")

    tf_ms = minutes * 60 * 1000

    df = df_5m.copy()
    # Convertir en ms pour le groupby
    time_ms = df["time"].astype("int64") // 10**6
    df["tf_group"] = (time_ms // tf_ms) * tf_ms

    agg = df.groupby("tf_group").agg(
        open=("open", "first"),
        high=("high", "max"),
        low=("low", "min"),
        close=("close", "last"),
        volume=("volume", "sum"),
    ).reset_index()

    agg["time"] = pd.to_datetime(agg["tf_group"], unit="ms")
    agg.drop(columns=["tf_group"], inplace=True)

    # Filtrer les bougies incomplètes
    bars_per_tf = minutes // 5
    counts = df.groupby("tf_group").size()
    complete_groups = counts[counts >= bars_per_tf].index
    agg_time_ms = agg["time"].astype("int64") // 10**6
    agg = agg[agg_time_ms.isin(complete_groups)]

    agg.reset_index(drop=True, inplace=True)
    return agg


def available_coins() -> list:
    """Liste les coins disponibles dans la DB."""
    db_path = os.path.abspath(DB_PATH)
    conn = sqlite3.connect(db_path)
    cursor = conn.execute("SELECT DISTINCT coin FROM candles WHERE interval = '5m' ORDER BY coin")
    coins = [row[0] for row in cursor.fetchall()]
    conn.close()
    return coins


def candle_count(coin: str, interval: str = "5m") -> int:
    """Nombre de bougies pour un coin/interval."""
    db_path = os.path.abspath(DB_PATH)
    conn = sqlite3.connect(db_path)
    cursor = conn.execute(
        "SELECT COUNT(*) FROM candles WHERE coin = ? AND interval = ?",
        [coin, interval],
    )
    count = cursor.fetchone()[0]
    conn.close()
    return count
