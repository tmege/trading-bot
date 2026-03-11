const { spawn } = require('child_process');
const path = require('path');
const fs = require('fs');

const HISTORY_FILE = 'data/backtest_history.json';

function loadHistory(projectRoot) {
  const fp = path.join(projectRoot, HISTORY_FILE);
  try {
    return JSON.parse(fs.readFileSync(fp, 'utf8'));
  } catch {
    return [];
  }
}

function saveHistory(projectRoot, history) {
  const fp = path.join(projectRoot, HISTORY_FILE);
  fs.mkdirSync(path.dirname(fp), { recursive: true });
  fs.writeFileSync(fp, JSON.stringify(history, null, 2));
}

function appendToHistory(projectRoot, result, params) {
  const history = loadHistory(projectRoot);
  const entry = {
    id: Date.now(),
    date: new Date().toISOString(),
    strategy: params.strategy,
    coin: result.coin || params.coins?.[0] || '?',
    interval: params.interval,
    n_days: params.nDays,
    return_pct: result.stats?.return_pct || 0,
    sharpe: result.stats?.sharpe_ratio || 0,
    max_dd: result.stats?.max_drawdown_pct || 0,
    profit_factor: result.stats?.profit_factor || 0,
    win_rate: result.stats?.win_rate || 0,
    trades: result.stats?.total_trades || 0,
    verdict: result.verdict || 'N/A',
  };
  history.push(entry);
  // Keep last 500 entries
  if (history.length > 500) history.splice(0, history.length - 500);
  saveHistory(projectRoot, history);
}

module.exports = function registerBacktestIPC(ipcMain, projectRoot) {
  const binary = path.join(projectRoot, 'build', 'backtest_json');

  ipcMain.handle('backtest:history', async () => {
    return loadHistory(projectRoot);
  });

  ipcMain.handle('backtest:clearHistory', async () => {
    saveHistory(projectRoot, []);
    return { ok: true };
  });

  ipcMain.handle('backtest:run', async (event, params) => {
    const {
      strategy,  // e.g. "bb_scalp_15m.lua"
      coins,     // e.g. ["ETH"] or ["ETH", "BTC", "SOL"]
      endDaysAgo = 0,
      nDays = 30,
      interval = '1h',
    } = params;

    const strategyPath = path.join(projectRoot, 'strategies', strategy);
    const results = [];

    const VALID_COIN = /^[A-Z]{2,10}$/;
    for (const coin of coins) {
      if (!VALID_COIN.test(coin)) {
        results.push({ coin, ok: false, error: 'Invalid coin symbol' });
        continue;
      }
      const win = event.sender;
      if (!win.isDestroyed()) {
        win.send('backtest:progress', {
          status: 'fetching',
          coin,
          current: results.length + 1,
          total: coins.length,
        });
      }

      try {
        const result = await runSingle(
          binary, strategyPath, coin,
          String(endDaysAgo), String(nDays), interval,
          projectRoot, event
        );
        results.push({ coin, ...result });
        // Save to history
        try { appendToHistory(projectRoot, { coin, ...result }, params); } catch (_) {}
      } catch (err) {
        results.push({ coin, error: err.message });
      }
    }

    const win = event.sender;
    if (!win.isDestroyed()) {
      win.send('backtest:progress', { status: 'complete' });
    }

    return { ok: true, results };
  });
};

function runSingle(binary, strategyPath, coin, endDaysAgo, nDays, interval,
                   projectRoot, event) {
  return new Promise((resolve, reject) => {
    const args = [strategyPath, coin, endDaysAgo, nDays, interval];
    const proc = spawn(binary, args, { cwd: projectRoot });

    let stdout = '';
    let stderr = '';

    proc.stdout.on('data', (d) => { stdout += d.toString(); });

    proc.stderr.on('data', (d) => {
      stderr += d.toString();
      // Forward progress events
      try {
        const lines = d.toString().split('\n').filter(Boolean);
        for (const line of lines) {
          const msg = JSON.parse(line);
          const win = event.sender;
          if (!win.isDestroyed()) {
            win.send('backtest:progress', { ...msg, coin });
          }
        }
      } catch (_) {
        // Not JSON progress — ignore
      }
    });

    proc.on('error', (err) => reject(err));

    proc.on('exit', (code) => {
      if (code !== 0) {
        return reject(new Error(`backtest_json exited with code ${code}: ${stderr}`));
      }
      try {
        const parsed = JSON.parse(stdout);
        resolve(parsed);
      } catch (err) {
        reject(new Error(`Failed to parse JSON output: ${err.message}`));
      }
    });
  });
}
