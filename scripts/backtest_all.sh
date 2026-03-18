#!/bin/bash
#
# Batch Backtest Runner — Run all strategies on ETH + BTC, 1 year
#
# Usage: ./scripts/backtest_all.sh [n_days] [end_days_ago]
#   n_days      : backtest period in days (default: 365)
#   end_days_ago: how many days ago to end (default: 0 = today)
#
# Requires: build/backtest_json (cmake --build build)
# Output: comparative table sorted by OOS Sharpe

set -euo pipefail
cd "$(dirname "$0")/.."

N_DAYS="${1:-365}"
END_DAYS="${2:-0}"
BT="./build/backtest_json"

# ── Build ────────────────────────────────────────────────────────────────
echo "=== Building project ==="
cmake --build build --parallel 2>/dev/null || {
    echo "Build failed. Run: cmake -B build && cmake --build build"
    exit 1
}

if [ ! -x "$BT" ]; then
    echo "ERROR: $BT not found"
    exit 1
fi

# ── Strategy definitions: name, file, interval ──────────────────────────
declare -a STRATS=(
    "btc_sniper_1h|strategies/btc_sniper_1h.lua|1h"
)

COINS="ETH BTC"
RESULTS_DIR="./data/backtest_results"
mkdir -p "$RESULTS_DIR"

# ── Run backtests ────────────────────────────────────────────────────────
echo ""
echo "=== Running backtests: ${#STRATS[@]} strategies x 2 coins x ${N_DAYS}d ==="
echo ""

SUMMARY_FILE="$RESULTS_DIR/summary_$(date +%Y%m%d_%H%M%S).csv"
echo "strategy,coin,interval,return_pct,sharpe_oos,sortino_oos,dd_pct,pf,win_rate,trades,alpha,verdict" > "$SUMMARY_FILE"

PASS=0
FAIL=0

for entry in "${STRATS[@]}"; do
    IFS='|' read -r name file interval <<< "$entry"

    if [ ! -f "$file" ]; then
        echo "  SKIP $name — $file not found"
        continue
    fi

    for coin in $COINS; do
        label="${name}_${coin}"
        outfile="$RESULTS_DIR/${label}.json"

        printf "  %-35s ... " "$label"

        # Run backtest, capture JSON stdout, stderr to /dev/null
        if $BT "$file" "$coin" "$END_DAYS" "$N_DAYS" "$interval" > "$outfile" 2>/dev/null; then
            # Extract key metrics from JSON using python (available on macOS)
            read -r ret sharpe sortino dd pf wr trades alpha verdict <<< $(python3 -c "
import json, sys
try:
    d = json.load(open('$outfile'))
    oos = d.get('stats_oos', d.get('stats', {}))
    print(
        f\"{oos.get('return_pct', 0):.2f}\",
        f\"{oos.get('sharpe_ratio', 0):.2f}\",
        f\"{oos.get('sortino_ratio', 0):.2f}\",
        f\"{oos.get('max_drawdown_pct', 0):.1f}\",
        f\"{min(oos.get('profit_factor', 0), 99.99):.2f}\",
        f\"{oos.get('win_rate', 0):.1f}\",
        f\"{oos.get('total_trades', 0)}\",
        f\"{d.get('alpha', 0):.2f}\",
        d.get('verdict', 'ERROR')
    )
except Exception as e:
    print('0 0 0 0 0 0 0 0 ERROR', file=sys.stdout)
" 2>/dev/null)

            # Color verdict
            case "$verdict" in
                DEPLOYABLE)   vc="\033[32m" ;;
                A_OPTIMISER)  vc="\033[33m" ;;
                MARGINAL)     vc="\033[36m" ;;
                INSUFFISANT)  vc="\033[90m" ;;
                *)            vc="\033[31m" ;;
            esac

            printf "ret=%6s%%  sharpe=%5s  PF=%5s  WR=%5s%%  trades=%4s  ${vc}%s\033[0m\n" \
                "$ret" "$sharpe" "$pf" "$wr" "$trades" "$verdict"

            echo "$name,$coin,$interval,$ret,$sharpe,$sortino,$dd,$pf,$wr,$trades,$alpha,$verdict" >> "$SUMMARY_FILE"
            ((PASS++))
        else
            printf "\033[31mFAILED\033[0m\n"
            ((FAIL++))
        fi
    done
done

# ── Summary ──────────────────────────────────────────────────────────────
echo ""
echo "════════════════════════════════════════════════════════════════════════"
echo "  RESULTS: $PASS passed, $FAIL failed"
echo "  CSV: $SUMMARY_FILE"
echo ""

# Sort by OOS Sharpe descending (skip header)
echo "  TOP 10 by OOS Sharpe:"
echo "  ─────────────────────"
tail -n +2 "$SUMMARY_FILE" | sort -t',' -k5 -rn | head -10 | while IFS=',' read -r name coin interval ret sharpe sortino dd pf wr trades alpha verdict; do
    printf "  %-25s %3s  sharpe=%5s  ret=%6s%%  PF=%5s  %s\n" \
        "$name" "$coin" "$sharpe" "$ret" "$pf" "$verdict"
done

echo ""
echo "  JSON files: $RESULTS_DIR/"
echo "════════════════════════════════════════════════════════════════════════"
