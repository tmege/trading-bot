const { spawn } = require('child_process');
const path = require('path');

module.exports = function registerBacktestIPC(ipcMain, projectRoot) {
  const binary = path.join(projectRoot, 'build', 'backtest_json');

  ipcMain.handle('backtest:run', async (event, params) => {
    const {
      strategy,  // e.g. "scalp_eth.lua"
      coins,     // e.g. ["ETH"] or ["ETH", "BTC", "SOL"]
      endDaysAgo = 0,
      nDays = 30,
      interval = '1h',
    } = params;

    const strategyPath = path.join(projectRoot, 'strategies', strategy);
    const results = [];

    for (const coin of coins) {
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
