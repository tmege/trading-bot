#!/usr/bin/env bash
# Grid Search TP/SL — Runs backtest_json across a TP×SL grid
#
# Usage: ./scripts/grid_search.sh <strategy.lua> <coin> [n_days] [interval]
# Example: ./scripts/grid_search.sh strategies/doge_sniper_relaxed_1h.lua DOGE 2000 1h
#
# Output: sorted table by OOS Sharpe ratio (best first)

set -euo pipefail

STRATEGY="${1:?Usage: $0 <strategy.lua> <coin> [n_days] [interval]}"
COIN="${2:?Usage: $0 <strategy.lua> <coin> [n_days] [interval]}"
N_DAYS="${3:-2000}"
INTERVAL="${4:-1h}"
END_DAYS=0

BT="./build/backtest_json"

if [ ! -x "$BT" ]; then
    echo "ERROR: $BT not found — run cmake --build build first" >&2
    exit 1
fi

# Grid definition
TP_VALUES="1.0 1.5 2.0 2.5 3.0 3.5 4.0 4.5 5.0 5.5 6.0"
SL_VALUES="0.5 1.0 1.5 2.0 2.5 3.0 3.5 4.0"

RESULTS_FILE=$(mktemp)
trap "rm -f $RESULTS_FILE" EXIT

TOTAL=0
for tp in $TP_VALUES; do
    for sl in $SL_VALUES; do
        TOTAL=$((TOTAL + 1))
    done
done

echo "Grid search: $STRATEGY $COIN ${N_DAYS}d — ${TOTAL} combos" >&2
echo "TP range: $TP_VALUES" >&2
echo "SL range: $SL_VALUES" >&2
echo "" >&2

COUNT=0
for tp in $TP_VALUES; do
    for sl in $SL_VALUES; do
        COUNT=$((COUNT + 1))

        # Run backtest, capture JSON stdout, suppress stderr progress
        JSON=$("$BT" "$STRATEGY" "$COIN" "$END_DAYS" "$N_DAYS" "$INTERVAL" "$tp" "$sl" 2>/dev/null || echo "{}")

        # Extract OOS stats using python (reliable JSON parsing)
        python3 -c "
import json, sys
try:
    d = json.loads('''$JSON''')
    oos = d.get('stats_oos', {})
    full = d.get('stats', {})
    sharpe = oos.get('sharpe_ratio', 0)
    sortino = oos.get('sortino_ratio', 0)
    pf = oos.get('profit_factor', 0)
    wr = oos.get('win_rate', 0)
    ret = oos.get('return_pct', 0)
    dd = oos.get('max_drawdown_pct', 0)
    trades = oos.get('total_trades', 0)
    f_ret = full.get('return_pct', 0)
    f_sharpe = full.get('sharpe_ratio', 0)
    print(f'$tp\t$sl\t{sharpe:.2f}\t{sortino:.2f}\t{pf:.2f}\t{wr:.1f}\t{ret:.1f}\t{dd:.1f}\t{trades}\t{f_ret:.1f}\t{f_sharpe:.2f}')
except:
    print(f'$tp\t$sl\t0.00\t0.00\t0.00\t0.0\t0.0\t0.0\t0\t0.0\t0.00')
" >> "$RESULTS_FILE"

        printf "\r  [%d/%d] TP=%.1f SL=%.1f" "$COUNT" "$TOTAL" "$tp" "$sl" >&2
    done
done

echo "" >&2
echo "" >&2

# Header + sorted results
printf "%-6s %-6s %-8s %-8s %-6s %-6s %-8s %-6s %-6s %-10s %-8s\n" \
    "TP" "SL" "Sharpe" "Sortino" "PF" "WR%" "Ret%" "DD%" "Trades" "FullRet%" "FullShp"
printf "%-6s %-6s %-8s %-8s %-6s %-6s %-8s %-6s %-6s %-10s %-8s\n" \
    "----" "----" "------" "-------" "----" "----" "------" "----" "------" "--------" "-------"

sort -t$'\t' -k3 -rn "$RESULTS_FILE" | while IFS=$'\t' read -r tp sl sharpe sortino pf wr ret dd trades fret fshp; do
    printf "%-6s %-6s %-8s %-8s %-6s %-6s %-8s %-6s %-6s %-10s %-8s\n" \
        "$tp" "$sl" "$sharpe" "$sortino" "$pf" "$wr" "$ret" "$dd" "$trades" "$fret" "$fshp"
done
