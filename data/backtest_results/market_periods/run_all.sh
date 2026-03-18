#!/bin/bash
# Run backtests across 7 market periods for 3 strategies × 4 coins
# Date reference: 2026-03-16

BT="./build/backtest_json"
DIR="./data/backtest_results/market_periods"

STRATEGIES=(
    "strategies/sniper_1h.lua:1h"
)

COINS=("ETH" "SOL")

# Market periods: name|end_days_ago|n_days|description
PERIODS=(
    "bull_2020_21|1796|197|Bull Run Oct2020-Apr2021"
    "bear_2022|1367|220|Bear Market Nov2021-Jun2022"
    "highvol_2022|1171|275|High Volatility LUNA+FTX Apr-Dec2022"
    "lowvol_2023|898|122|Low Movement Sideways Jun-Sep2023"
    "bull_etf_2024|732|166|Bull Run ETF Oct2023-Mar2024"
    "bull_ath_2024|420|142|Bull ATH Sep2024-Jan2025"
    "recent|0|180|Recent 6 Months Sep2025-Mar2026"
)

TOTAL=0
DONE=0
FAILED=0

# Count total jobs
for _ in "${STRATEGIES[@]}"; do
    for _ in "${COINS[@]}"; do
        for _ in "${PERIODS[@]}"; do
            TOTAL=$((TOTAL + 1))
        done
    done
done

echo "Running $TOTAL backtests..." >&2

for strat_entry in "${STRATEGIES[@]}"; do
    IFS=':' read -r strat_path tf <<< "$strat_entry"
    strat_name=$(basename "$strat_path" .lua)

    for coin in "${COINS[@]}"; do
        for period_entry in "${PERIODS[@]}"; do
            IFS='|' read -r period_name end_ago n_days desc <<< "$period_entry"
            DONE=$((DONE + 1))

            outfile="$DIR/${period_name}_${strat_name}_${coin}.json"

            # Skip if already done
            if [ -f "$outfile" ] && [ -s "$outfile" ]; then
                echo "[$DONE/$TOTAL] SKIP $strat_name $coin $period_name (exists)" >&2
                continue
            fi

            echo "[$DONE/$TOTAL] $strat_name $coin $period_name ($desc)" >&2

            # Run backtest, capture JSON stdout
            timeout 120 $BT "$strat_path" "$coin" "$end_ago" "$n_days" "$tf" > "$outfile" 2>/dev/null
            rc=$?

            if [ $rc -ne 0 ] || [ ! -s "$outfile" ]; then
                echo "  FAILED (rc=$rc)" >&2
                FAILED=$((FAILED + 1))
                rm -f "$outfile"
            else
                # Quick extract of key metrics
                net=$(python3 -c "import json,sys; d=json.load(open('$outfile')); print(f\"{d['stats']['return_pct']:+.1f}%\")" 2>/dev/null || echo "?")
                echo "  OK: $net" >&2
            fi
        done
    done
done

echo "" >&2
echo "=== COMPLETE: $DONE total, $FAILED failed ===" >&2
