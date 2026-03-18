#!/bin/bash
# Multi-period backtest runner
# Tests all strategies on different market phases
#
# Periods (based on ETH/BTC crypto cycle):
# P1: Last 365 days (full cycle)
# P2: 365-180 days ago (6 months, older period)
# P3: 180-0 days ago (recent 6 months)
# P4: Last 90 days (most recent quarter)

set -e

BUILDDIR="$(cd "$(dirname "$0")/.." && pwd)/build"
BACKTEST="$BUILDDIR/backtest_json"
OUTDIR="$(cd "$(dirname "$0")/.." && pwd)/data/backtest_results"
mkdir -p "$OUTDIR"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTFILE="$OUTDIR/multi_period_${TIMESTAMP}.csv"

# Strategy definitions: file:coin:tf
STRATEGIES=(
    "strategies/sniper_1h.lua:ETH:1h"
    "strategies/sniper_1h.lua:BTC:1h"
    "strategies/sniper_1h.lua:SOL:1h"
)

# Periods: name:n_days:end_days_ago
PERIODS=(
    "P1_365d:365:0"
    "P2_old_180d:180:180"
    "P3_recent_180d:180:0"
    "P4_recent_90d:90:0"
)

# CSV header
echo "period,strategy,coin,tf,trades,return_pct,sharpe,sortino,max_dd,pf,win_rate,verdict" > "$OUTFILE"

TOTAL=$((${#STRATEGIES[@]} * ${#PERIODS[@]}))
COUNT=0

for period_def in "${PERIODS[@]}"; do
    IFS=':' read -r period_name n_days end_days <<< "$period_def"

    echo ""
    echo "=== Period: $period_name (${n_days}d, end=${end_days}d ago) ==="

    for strat_def in "${STRATEGIES[@]}"; do
        IFS=':' read -r strat_file coin tf <<< "$strat_def"
        COUNT=$((COUNT + 1))

        strat_name=$(basename "$strat_file" .lua)
        printf "[%d/%d] %-25s %-4s %-4s %s ... " "$COUNT" "$TOTAL" "$strat_name" "$coin" "$tf" "$period_name"

        # Run backtest, capture JSON from stdout (status messages filtered by python)
        result=$("$BACKTEST" "$strat_file" "$coin" "$end_days" "$n_days" "$tf" 2>/dev/null | python3 -c "
import json, sys
try:
    # Read all input, find last valid JSON object
    raw = sys.stdin.read()
    # Find the last '{' that starts a top-level object
    depth = 0
    last_start = -1
    for i, c in enumerate(raw):
        if c == '{' and depth == 0:
            last_start = i
            depth = 1
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
    if last_start >= 0:
        d = json.loads(raw[last_start:])
    else:
        d = json.loads(raw)
    s = d.get('stats', {})
    v = d.get('verdict', 'UNKNOWN')
    print(f\"{s.get('total_trades',0)},{s.get('return_pct',0):.2f},{s.get('sharpe_ratio',0):.2f},{s.get('sortino_ratio',0):.2f},{s.get('max_drawdown_pct',0):.2f},{s.get('profit_factor',0):.2f},{s.get('win_rate',0):.1f},{v}\")
except Exception as e:
    print(f'0,0.00,0.00,0.00,0.00,0.00,0.0,FAILED')
" 2>/dev/null)

        if [ -z "$result" ]; then
            result="0,0.00,0.00,0.00,0.00,0.00,0.0,FAILED"
        fi

        echo "$period_name,$strat_name,$coin,$tf,$result" >> "$OUTFILE"

        # Extract key info for display
        trades=$(echo "$result" | cut -d',' -f1)
        ret=$(echo "$result" | cut -d',' -f2)
        sharpe=$(echo "$result" | cut -d',' -f3)
        verdict=$(echo "$result" | cut -d',' -f8)
        printf "trades=%s ret=%s%% sharpe=%s [%s]\n" "$trades" "$ret" "$sharpe" "$verdict"
    done
done

echo ""
echo "===== RESULTS SAVED TO: $OUTFILE ====="
echo ""

# Summary table
python3 -c "
import csv, sys
from collections import defaultdict

with open('$OUTFILE') as f:
    reader = csv.DictReader(f)
    rows = list(reader)

# Group by strategy+coin
strats = defaultdict(dict)
for r in rows:
    key = f\"{r['strategy']}_{r['coin']}\"
    period = r['period']
    strats[key][period] = r

periods = ['P1_365d', 'P2_old_180d', 'P3_recent_180d', 'P4_recent_90d']

print()
print(f\"{'Strategy':<30} {'Coin':>4} | {'P1 (365d)':>14} | {'P2 (old 180d)':>14} | {'P3 (new 180d)':>14} | {'P4 (90d)':>14}\")
print('-' * 110)

for key in sorted(strats.keys()):
    data = strats[key]
    first = list(data.values())[0]
    name = first['strategy'][:25]
    coin = first['coin']

    cells = []
    for p in periods:
        if p in data:
            d = data[p]
            t = int(d['trades'])
            ret = float(d['return_pct'])
            v = d['verdict']
            if t == 0:
                cells.append('  0 trades')
            elif v == 'FAILED':
                cells.append('  FAILED')
            else:
                sign = '+' if ret >= 0 else ''
                cells.append(f'{sign}{ret:>6.1f}% ({t:>4}t)')

        else:
            cells.append('  N/A')

    print(f'{name:<30} {coin:>4} | {cells[0]:>14} | {cells[1]:>14} | {cells[2]:>14} | {cells[3]:>14}')

print()
print('Legend: return% (trades)')
"
