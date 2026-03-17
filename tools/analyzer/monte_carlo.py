"""Simulation Monte Carlo pour estimation de risque.

Simule des séquences de trades aléatoires (bootstrap) pour estimer :
- Probabilité de ruine (equity → 0)
- P95 drawdown
- Distribution du rendement final
"""

import numpy as np


def monte_carlo_equity(trades_pnl: np.ndarray, n_sims: int = 10000,
                       capital: float = 672, leverage: float = 5.0,
                       ruin_threshold: float = 0.5) -> dict:
    """Monte Carlo sur une séquence de PnL de trades.

    Bootstrap : tire aléatoirement des trades (avec replacement)
    et simule l'évolution de l'equity sur N simulations.

    Args:
        trades_pnl: Array de PnL en % par trade (ex: [+2.1, -1.5, +3.0, ...]).
        n_sims: Nombre de simulations.
        capital: Capital initial en $.
        leverage: Levier moyen pour le sizing.
        ruin_threshold: Fraction du capital en-dessous de laquelle on considère la ruine.

    Returns:
        Dict avec p_ruin, p95_drawdown, median_return, mean_return,
        p5_return, p95_return, etc.
    """
    n_trades = len(trades_pnl)
    if n_trades < 10:
        return _empty_mc("Pas assez de trades")

    rng = np.random.default_rng(42)
    ruin_level = capital * ruin_threshold

    final_equities = np.zeros(n_sims)
    max_drawdowns = np.zeros(n_sims)
    ruin_count = 0

    for sim in range(n_sims):
        # Bootstrap : tirer n_trades trades aléatoires
        indices = rng.integers(0, n_trades, size=n_trades)
        sampled_pnl = trades_pnl[indices]

        equity = capital
        peak = capital
        max_dd = 0

        for pnl_pct in sampled_pnl:
            # Position size = equity * leverage_fraction (ex: 40% equity * 5x = 2x notional)
            # PnL en $ = equity * position_frac * pnl%/100
            # Simplifié : on utilise directement le PnL% avec le levier implicite
            dollar_pnl = equity * (pnl_pct / 100) * (leverage / 3)  # normalise vs levier 3x de base
            equity += dollar_pnl

            if equity > peak:
                peak = equity
            dd = (peak - equity) / peak * 100 if peak > 0 else 0
            if dd > max_dd:
                max_dd = dd

            if equity <= ruin_level:
                ruin_count += 1
                break

        final_equities[sim] = equity
        max_drawdowns[sim] = max_dd

    returns_pct = (final_equities - capital) / capital * 100

    return {
        "valid": True,
        "n_sims": n_sims,
        "n_trades": n_trades,
        "capital": capital,
        "leverage": leverage,
        "p_ruin": round(ruin_count / n_sims * 100, 2),
        "p95_drawdown": round(np.percentile(max_drawdowns, 95), 2),
        "p99_drawdown": round(np.percentile(max_drawdowns, 99), 2),
        "median_drawdown": round(np.median(max_drawdowns), 2),
        "median_return": round(np.median(returns_pct), 2),
        "mean_return": round(np.mean(returns_pct), 2),
        "p5_return": round(np.percentile(returns_pct, 5), 2),
        "p25_return": round(np.percentile(returns_pct, 25), 2),
        "p75_return": round(np.percentile(returns_pct, 75), 2),
        "p95_return": round(np.percentile(returns_pct, 95), 2),
        "median_final_equity": round(np.median(final_equities), 2),
    }


def regime_transition_mc(regime_trades: dict, regime_durations: dict,
                         n_sims: int = 5000, n_periods: int = 365,
                         capital: float = 672) -> dict:
    """Monte Carlo avec simulation de séquences de régimes.

    Simule l'alternance des régimes et applique les trades correspondants.

    Args:
        regime_trades: Dict {"bull": [pnl_array], "bear": [...], "neutral": [...]}
        regime_durations: Dict {"bull": avg_dur, "bear": avg_dur, "neutral": avg_dur}
        n_sims: Nombre de simulations.
        n_periods: Nombre de périodes (bougies) à simuler.
        capital: Capital initial.

    Returns:
        Dict avec stats par régime et globales.
    """
    regimes = list(regime_trades.keys())
    if not regimes:
        return _empty_mc("Aucun régime avec des trades")

    rng = np.random.default_rng(42)
    final_equities = np.zeros(n_sims)
    max_drawdowns = np.zeros(n_sims)
    ruin_count = 0

    for sim in range(n_sims):
        equity = capital
        peak = capital
        max_dd = 0
        t = 0

        while t < n_periods:
            # Choisir un régime aléatoire
            regime = rng.choice(regimes)
            avg_dur = regime_durations.get(regime, 20)
            duration = max(1, int(rng.exponential(avg_dur)))

            trades = regime_trades.get(regime, np.array([]))
            if len(trades) == 0:
                t += duration
                continue

            # Trades pendant ce régime (environ 1 trade / 20 bougies en 1h)
            n_trades_in_period = max(1, duration // 20)
            for _ in range(n_trades_in_period):
                pnl_pct = rng.choice(trades)
                dollar_pnl = equity * (pnl_pct / 100) * 0.4  # 40% equity sizing
                equity += dollar_pnl

                if equity > peak:
                    peak = equity
                dd = (peak - equity) / peak * 100 if peak > 0 else 0
                if dd > max_dd:
                    max_dd = dd

                if equity <= capital * 0.5:
                    ruin_count += 1
                    break

            if equity <= capital * 0.5:
                break

            t += duration

        final_equities[sim] = equity
        max_drawdowns[sim] = max_dd

    returns_pct = (final_equities - capital) / capital * 100

    return {
        "valid": True,
        "n_sims": n_sims,
        "n_periods": n_periods,
        "p_ruin": round(ruin_count / n_sims * 100, 2),
        "p95_drawdown": round(np.percentile(max_drawdowns, 95), 2),
        "median_return": round(np.median(returns_pct), 2),
        "mean_return": round(np.mean(returns_pct), 2),
        "p5_return": round(np.percentile(returns_pct, 5), 2),
        "p95_return": round(np.percentile(returns_pct, 95), 2),
        "median_final_equity": round(np.median(final_equities), 2),
    }


def _empty_mc(reason: str) -> dict:
    """Résultat vide."""
    return {
        "valid": False,
        "reason": reason,
        "n_sims": 0,
        "p_ruin": 0,
        "p95_drawdown": 0,
        "median_return": 0,
        "mean_return": 0,
        "p5_return": 0,
        "p95_return": 0,
        "median_final_equity": 0,
    }
