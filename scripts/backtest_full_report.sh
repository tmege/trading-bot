#!/bin/bash
# Backtest complet transparent — OOS uniquement, comparaison B&H
# Pas de triche : seuls les resultats OUT-OF-SAMPLE sont evalues
#
# Periodes adaptees au timeframe (limite API Hyperliquid = 5000 candles) :
#   4h/1d : 365d, 180d, 90d  (donnees profondes)
#   1h    : 180d, 90d         (max ~208j de data)
#   15m   : 45d               (max ~52j de data)
#   5m    : 15d               (max ~17j de data)

set -e

ROOTDIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILDDIR="$ROOTDIR/build"
BACKTEST="$BUILDDIR/backtest_json"
OUTDIR="$ROOTDIR/data/backtest_results"
mkdir -p "$OUTDIR"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
CSVFILE="$OUTDIR/full_report_${TIMESTAMP}.csv"
MDFILE="$ROOTDIR/docs/backtest-report.md"

COINS="ETH BTC SOL DOGE"

# CSV header
echo "strategy,coin,tf,period_days,oos_trades,oos_return_pct,oos_sharpe,oos_sortino,oos_max_dd,oos_pf,oos_winrate,bnh_return_pct,alpha_pct,is_sharpe,wf_sharpe_decay,verdict" > "$CSVFILE"

echo ""
echo "=========================================="
echo "  BACKTEST TRANSPARENT — OOS UNIQUEMENT"
echo "  Fees: maker 2bps, taker 5bps, slip 1bp"
echo "  Balance: \$100, Leverage: 5x, Compound 10%"
echo "=========================================="
echo ""

run_one() {
    local strat_name="$1"
    local strat_file="$2"
    local tf="$3"
    local coin="$4"
    local n_days="$5"

    printf "  %-25s %-4s %-3s %3dd ... " "$strat_name" "$coin" "$tf" "$n_days"

    result=$("$BACKTEST" "$strat_file" "$coin" "0" "$n_days" "$tf" 2>/dev/null | python3 -c "
import json, sys
try:
    raw = sys.stdin.read()
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
    oos = d.get('stats_oos', d.get('stats', {}))
    is_s = d.get('stats_is', d.get('stats', {}))
    bnh = d.get('buy_and_hold', {}).get('oos', d.get('buy_and_hold', {}).get('full', {}))
    wf = d.get('walk_forward', {})
    v = d.get('verdict', 'UNKNOWN')
    alpha = d.get('alpha', oos.get('return_pct', 0) - bnh.get('return_pct', 0))
    oos_trades = oos.get('total_trades', 0)
    oos_ret = oos.get('return_pct', 0)
    oos_sharpe = oos.get('sharpe_ratio', 0)
    oos_sortino = oos.get('sortino_ratio', 0)
    oos_dd = oos.get('max_drawdown_pct', 0)
    oos_pf = min(oos.get('profit_factor', 0), 9999)
    oos_wr = oos.get('win_rate', 0)
    bnh_ret = bnh.get('return_pct', 0)
    is_sharpe = is_s.get('sharpe_ratio', 0)
    wf_decay = wf.get('sharpe_decay_pct', 0)
    print(f'{oos_trades},{oos_ret:.2f},{oos_sharpe:.2f},{oos_sortino:.2f},{oos_dd:.2f},{oos_pf:.2f},{oos_wr:.1f},{bnh_ret:.2f},{alpha:.2f},{is_sharpe:.2f},{wf_decay:.1f},{v}')
except Exception as e:
    print(f'0,0.00,0.00,0.00,0.00,0.00,0.0,0.00,0.00,0.00,0.0,FAILED')
" 2>/dev/null)

    if [ -z "$result" ]; then
        result="0,0.00,0.00,0.00,0.00,0.00,0.0,0.00,0.00,0.00,0.0,FAILED"
    fi

    echo "$strat_name,$coin,$tf,${n_days},$result" >> "$CSVFILE"

    trades=$(echo "$result" | cut -d',' -f1)
    ret=$(echo "$result" | cut -d',' -f2)
    sharpe=$(echo "$result" | cut -d',' -f3)
    bnh_ret=$(echo "$result" | cut -d',' -f8)
    alpha=$(echo "$result" | cut -d',' -f9)
    verdict=$(echo "$result" | cut -d',' -f12)
    printf "OOS: %4s trades  ret=%7s%%  sharpe=%6s  B&H=%7s%%  alpha=%7s%%  [%s]\n" \
        "$trades" "$ret" "$sharpe" "$bnh_ret" "$alpha" "$verdict"
}

run_strat() {
    local strat_name="$1"
    local strat_file="$2"
    local tf="$3"
    shift 3
    local periods="$@"

    echo ""
    echo "--- $strat_name ($tf) ---"

    for n_days in $periods; do
        for coin in $COINS; do
            run_one "$strat_name" "$strat_file" "$tf" "$coin" "$n_days"
        done
    done
}

# 1h strategies (5m simulation, all data available)
run_strat "sniper_1h"  "strategies/sniper_1h.lua"  "1h"  365 180 90

echo ""
echo "=========================================="
echo "  GENERATING REPORT..."
echo "=========================================="

# Generate markdown report
python3 - "$CSVFILE" "$MDFILE" << 'PYEOF'
import csv, sys
from collections import defaultdict
from datetime import datetime

csv_file = sys.argv[1]
md_file = sys.argv[2]

with open(csv_file) as f:
    reader = csv.DictReader(f)
    rows = list(reader)

lines = []
def p(s=""):
    lines.append(s)

p("# Backtest Report — OOS Transparent")
p()
p(f"**Date**: {datetime.now().strftime('%Y-%m-%d %H:%M')}")
p()
p("## Methode")
p()
p("- **Evaluation**: Out-of-Sample uniquement (40% des donnees, jamais vues par la strategie)")
p("- **Walk-forward**: Split automatique 60% IS / 40% OOS")
p("- **Fees**: Maker 0.02% (ALO entries), Taker 0.05% (trigger exits), Slippage 1bp")
p("- **Capital**: $100, Levier 5x, Compound 10% de l'equity par trade")
p("- **Benchmark**: Buy & Hold levier 5x sur la meme periode")
p("- **Verdict OOS**: DEPLOYABLE (Sharpe>=1.5, alpha>0, PF>=1.5, DD<20%, 50+ trades) | A_OPTIMISER | MARGINAL | INSUFFISANT (<30 trades) | ABANDON")
p()

by_strat = defaultdict(list)
for r in rows:
    by_strat[r['strategy']].append(r)

p("## Resultats Detailles (OOS uniquement)")
p()
p("| Strategie | TF | Coin | Jours | Trades | Return | Sharpe | Max DD | PF | WR | B&H | Alpha | Verdict |")
p("|---|---|---|---|---|---|---|---|---|---|---|---|---|")

for strat in sorted(by_strat.keys()):
    for r in sorted(by_strat[strat], key=lambda x: (x['coin'], -int(x['period_days']))):
        trades = int(r['oos_trades'])
        ret = float(r['oos_return_pct'])
        sharpe = float(r['oos_sharpe'])
        dd = float(r['oos_max_dd'])
        pf = float(r['oos_pf'])
        wr = float(r['oos_winrate'])
        bnh = float(r['bnh_return_pct'])
        alpha = float(r['alpha_pct'])
        v = r['verdict']

        ret_s = f"+{ret:.1f}%" if ret >= 0 else f"{ret:.1f}%"
        alpha_s = f"+{alpha:.1f}%" if alpha >= 0 else f"{alpha:.1f}%"
        bnh_s = f"+{bnh:.1f}%" if bnh >= 0 else f"{bnh:.1f}%"
        pf_s = f"{pf:.2f}" if pf < 9999 else "inf"

        p(f"| {r['strategy']} | {r['tf']} | {r['coin']} | {r['period_days']}d | {trades} | {ret_s} | {sharpe:.2f} | {dd:.1f}% | {pf_s} | {wr:.0f}% | {bnh_s} | {alpha_s} | {v} |")

p()
p("## Analyse Overfitting (IS vs OOS)")
p()
p("| Strategie | Coin | Jours | Sharpe IS | Sharpe OOS | Decay | Signal |")
p("|---|---|---|---|---|---|---|")

for strat in sorted(by_strat.keys()):
    for r in sorted(by_strat[strat], key=lambda x: (x['coin'], -int(x['period_days']))):
        is_sh = float(r['is_sharpe'])
        oos_sh = float(r['oos_sharpe'])
        decay = float(r['wf_sharpe_decay'])
        signal = "OVERFIT" if decay < -50 else "SUSPECT" if decay < -30 else "OK"
        p(f"| {r['strategy']} | {r['coin']} | {r['period_days']}d | {is_sh:.2f} | {oos_sh:.2f} | {decay:.0f}% | {signal} |")

p()
p("## Classement Final (moyenne OOS sur tous les coins/periodes)")
p()

avg_sharpe = defaultdict(list)
avg_alpha = defaultdict(list)
avg_ret = defaultdict(list)
avg_dd = defaultdict(list)
verdicts = defaultdict(list)
tfs = {}
for r in rows:
    s = r['strategy']
    tfs[s] = r['tf']
    if r['verdict'] not in ('FAILED', 'INSUFFISANT'):
        avg_sharpe[s].append(float(r['oos_sharpe']))
        avg_alpha[s].append(float(r['alpha_pct']))
        avg_ret[s].append(float(r['oos_return_pct']))
        avg_dd[s].append(float(r['oos_max_dd']))
        verdicts[s].append(r['verdict'])

ranking = []
for s in avg_sharpe:
    sh = sum(avg_sharpe[s]) / len(avg_sharpe[s])
    al = sum(avg_alpha[s]) / len(avg_alpha[s])
    rt = sum(avg_ret[s]) / len(avg_ret[s])
    dd = max(avg_dd[s])
    dep = verdicts[s].count('DEPLOYABLE')
    tot = len(verdicts[s])
    ranking.append((s, sh, al, rt, dd, dep, tot))

ranking.sort(key=lambda x: x[1], reverse=True)

p("| Rang | Strategie | TF | Sharpe Moy | Return Moy | Alpha Moy | Max DD | Deploy |")
p("|---|---|---|---|---|---|---|---|")
for i, (s, sh, al, rt, dd, dep, tot) in enumerate(ranking, 1):
    al_s = f"+{al:.1f}%" if al >= 0 else f"{al:.1f}%"
    rt_s = f"+{rt:.1f}%" if rt >= 0 else f"{rt:.1f}%"
    p(f"| {i} | {s} | {tfs.get(s,'')} | {sh:.2f} | {rt_s} | {al_s} | {dd:.1f}% | {dep}/{tot} |")

non_ranked = set(by_strat.keys()) - set(avg_sharpe.keys())
if non_ranked:
    p()
    p(f"**Non classees** (pas assez de trades ou FAILED): {', '.join(sorted(non_ranked))}")

p()
p("## Notes")
p()
p("- Sharpe > 5 = suspect (overfitting probable ou trop peu de trades)")
p("- Alpha = Return strategie - Return Buy & Hold (meme periode, meme levier 5x)")
p("- Periodes limitees par l'API Hyperliquid: 5m=17j, 15m=52j, 1h=208j, 4h=833j")
p("- Walk-forward: 60% IS (calibration) / 40% OOS (evaluation)")
p("- Les resultats passes ne garantissent pas les performances futures")

with open(md_file, 'w') as f:
    f.write('\n'.join(lines) + '\n')

print(f"Report written to {md_file}")
PYEOF

echo ""
echo "=== DONE ==="
echo "=== Report: $MDFILE ==="
echo "=== CSV: $CSVFILE ==="
