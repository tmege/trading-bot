#!/usr/bin/env python3
import json, sys
d = json.load(sys.stdin)
s = d["stats"]
trades = d.get("trades", [])
tf = sum(t.get("fee", 0) for t in trades)
tp = sum(t.get("pnl", 0) for t in trades)
exits = [t for t in trades if t.get("pnl", 0) != 0]
r = s["return_pct"]
wr = s["win_rate"]
pf = s["profit_factor"]
sh = s["sharpe_ratio"]
dd = s["max_drawdown_pct"]
print("Ret=%+.1f%% Exits=%d Win=%.0f%% PF=%.2f Sharpe=%.2f DD=%.1f%% PnL=$%.2f Fees=$%.2f" % (r, len(exits), wr, pf, sh, dd, tp, tf))
