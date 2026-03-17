"""Génération de rapports markdown."""

import os
from datetime import datetime


REPORT_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "data", "analysis"))


def generate_report(coin: str, tf: str, long_results: list, short_results: list,
                    tp_sl_scans: dict = None, n_candles: int = 0,
                    date_range: str = "") -> str:
    """Génère un rapport markdown complet.

    Args:
        coin: Nom du coin (ETH, SOL, etc.)
        tf: Timeframe (1h, 4h, etc.)
        long_results: Liste de (signal_name, stats_dict) triée par EV.
        short_results: Idem pour short.
        tp_sl_scans: Dict {signal_name: list_of_tp_sl_results} pour les heatmaps.
        n_candles: Nombre de bougies analysées.
        date_range: Période couverte.

    Returns:
        Chemin du fichier généré.
    """
    os.makedirs(REPORT_DIR, exist_ok=True)

    lines = []
    lines.append(f"# Analyse Probabiliste — {coin} {tf}")
    lines.append(f"")
    lines.append(f"*Généré le {datetime.now().strftime('%Y-%m-%d %H:%M')}*")
    lines.append(f"")
    lines.append(f"- **Coin** : {coin}")
    lines.append(f"- **Timeframe** : {tf}")
    lines.append(f"- **Bougies analysées** : {n_candles:,}")
    lines.append(f"- **Période** : {date_range}")
    lines.append(f"- **Fees** : 0.06% round-trip (maker entry + taker exit)")
    lines.append(f"")

    # --- Top LONG par EV ---
    lines.append(f"## Top 20 Signaux LONG (par Expected Value)")
    lines.append(f"")
    _add_signal_table(lines, long_results[:20])

    # --- Top SHORT par EV ---
    lines.append(f"## Top 20 Signaux SHORT (par Expected Value)")
    lines.append(f"")
    _add_signal_table(lines, short_results[:20])

    # --- Top par Win Rate (min 50 occurrences) ---
    lines.append(f"## Top 10 Signaux par Win Rate (min 50 occurrences)")
    lines.append(f"")
    high_wr_long = sorted(
        [(n, s) for n, s in long_results if s["count"] >= 50],
        key=lambda x: x[1]["win_rate"],
        reverse=True,
    )[:10]
    if high_wr_long:
        lines.append(f"### LONG")
        lines.append(f"")
        _add_signal_table(lines, high_wr_long)

    high_wr_short = sorted(
        [(n, s) for n, s in short_results if s["count"] >= 50],
        key=lambda x: x[1]["win_rate"],
        reverse=True,
    )[:10]
    if high_wr_short:
        lines.append(f"### SHORT")
        lines.append(f"")
        _add_signal_table(lines, high_wr_short)

    # --- Heatmaps TP/SL ---
    if tp_sl_scans:
        lines.append(f"## Matrices TP/SL (meilleurs signaux)")
        lines.append(f"")
        for signal_name, scan_results in tp_sl_scans.items():
            lines.append(f"### {signal_name}")
            lines.append(f"")
            _add_tp_sl_heatmap(lines, scan_results)
            lines.append(f"")

    # --- Recommandations ---
    lines.append(f"## Recommandations")
    lines.append(f"")
    _add_recommendations(lines, long_results, short_results)

    content = "\n".join(lines)
    filepath = os.path.join(REPORT_DIR, f"{coin}_{tf}_report.md")
    with open(filepath, "w") as f:
        f.write(content)

    return filepath


def _add_signal_table(lines: list, results: list):
    """Ajoute un tableau de résultats de signaux."""
    if not results:
        lines.append("*Aucun signal significatif trouvé.*")
        lines.append("")
        return

    lines.append("| # | Signal | Count | WR% | EV% | PF | Total PnL% | CI 95% | Sharpe |")
    lines.append("|---|--------|-------|-----|-----|----|------------|--------|--------|")

    for i, (name, s) in enumerate(results, 1):
        pf_str = f"{s['profit_factor']:.2f}" if s['profit_factor'] < 100 else "∞"
        lines.append(
            f"| {i} | `{name}` | {s['count']} | {s['win_rate']:.1f} | "
            f"{s['ev']:.3f} | {pf_str} | {s['total_pnl']:.1f} | "
            f"[{s['ci_95_low']:.1f}-{s['ci_95_high']:.1f}] | {s['sharpe']:.3f} |"
        )

    lines.append("")


def _add_tp_sl_heatmap(lines: list, scan_results: list):
    """Ajoute une matrice TP/SL en format markdown."""
    if not scan_results:
        lines.append("*Pas assez de données.*")
        return

    # Extraire les TP et SL uniques
    tps = sorted(set(r["tp"] for r in scan_results))
    sls = sorted(set(r["sl"] for r in scan_results))

    # Construire la matrice
    ev_map = {}
    for r in scan_results:
        ev_map[(r["tp"], r["sl"])] = r["ev"]

    # Header
    header = "| TP \\ SL |" + "|".join(f" {sl}% " for sl in sls) + "|"
    separator = "|---------|" + "|".join("------" for _ in sls) + "|"
    lines.append(header)
    lines.append(separator)

    # Trouver le max EV pour le marquer
    max_ev = max(r["ev"] for r in scan_results)

    for tp in tps:
        row = f"| **{tp}%** |"
        for sl in sls:
            ev = ev_map.get((tp, sl))
            if ev is not None:
                marker = " **" if ev == max_ev else " "
                marker_end = "** " if ev == max_ev else " "
                row += f"{marker}{ev:.3f}{marker_end}|"
            else:
                row += " — |"
        lines.append(row)

    lines.append("")

    # Best combo
    best = max(scan_results, key=lambda x: x["ev"])
    lines.append(
        f"**Optimal** : TP {best['tp']}% / SL {best['sl']}% "
        f"(EV={best['ev']:.3f}%, WR={best['win_rate']:.1f}%, "
        f"PF={best['profit_factor']:.2f}, R:R=1:{best['rr']:.1f})"
    )


def _add_recommendations(lines: list, long_results: list, short_results: list):
    """Ajoute les recommandations basées sur l'analyse."""
    # Filtrer: EV > 0, count >= 30, profit_factor > 1.0
    strong_longs = [
        (n, s) for n, s in long_results
        if s["ev"] > 0 and s["count"] >= 30 and s["profit_factor"] > 1.0
    ][:5]

    strong_shorts = [
        (n, s) for n, s in short_results
        if s["ev"] > 0 and s["count"] >= 30 and s["profit_factor"] > 1.0
    ][:5]

    if strong_longs:
        lines.append("### Meilleures combinaisons LONG à implémenter")
        lines.append("")
        for i, (name, s) in enumerate(strong_longs, 1):
            signals = name.split("+")
            lines.append(f"{i}. **`{name}`**")
            lines.append(f"   - Conditions : {' AND '.join(signals)}")
            lines.append(f"   - Win rate : {s['win_rate']:.1f}% ({s['count']} trades)")
            lines.append(f"   - EV : {s['ev']:.3f}% par trade")
            lines.append(f"   - Profit Factor : {s['profit_factor']:.2f}")
            lines.append(f"   - IC 95% WR : [{s['ci_95_low']:.1f}% - {s['ci_95_high']:.1f}%]")
            lines.append("")

    if strong_shorts:
        lines.append("### Meilleures combinaisons SHORT à implémenter")
        lines.append("")
        for i, (name, s) in enumerate(strong_shorts, 1):
            signals = name.split("+")
            lines.append(f"{i}. **`{name}`**")
            lines.append(f"   - Conditions : {' AND '.join(signals)}")
            lines.append(f"   - Win rate : {s['win_rate']:.1f}% ({s['count']} trades)")
            lines.append(f"   - EV : {s['ev']:.3f}% par trade")
            lines.append(f"   - Profit Factor : {s['profit_factor']:.2f}")
            lines.append(f"   - IC 95% WR : [{s['ci_95_low']:.1f}% - {s['ci_95_high']:.1f}%]")
            lines.append("")

    if not strong_longs and not strong_shorts:
        lines.append("*Aucun signal avec EV positive et >30 occurrences trouvé. "
                      "Essayer d'ajuster les grilles TP/SL ou les timeframes.*")
        lines.append("")
