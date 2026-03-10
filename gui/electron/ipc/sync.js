const { spawn } = require('child_process');
const path = require('path');
const fs = require('fs');

let syncProcess = null;

module.exports = function registerSyncIPC(ipcMain, projectRoot, getWindow) {
  const binary = path.join(projectRoot, 'build', 'candle_fetcher');
  const dbPath = path.join(projectRoot, 'data', 'candle_cache.db');
  const syncStatePath = path.join(projectRoot, 'data', 'last_sync.json');

  // ── Manual sync ───────────────────────────────────────────────────────
  ipcMain.handle('sync:run', async (event) => {
    if (syncProcess && !syncProcess.killed) {
      return { ok: false, error: 'Sync already running' };
    }

    return new Promise((resolve) => {
      syncProcess = spawn(binary, [
        '--days', '1461',
        '--coins', 'ETH,BTC,SOL,DOGE,HYPE,PUMP',
        '--intervals', '5m,15m,1h,4h,1d',
        '--db', dbPath,
      ], { cwd: projectRoot });

      let stdout = '';

      syncProcess.stdout.on('data', (d) => { stdout += d.toString(); });

      syncProcess.stderr.on('data', (d) => {
        // Forward progress to renderer
        const lines = d.toString().split('\n').filter(Boolean);
        for (const line of lines) {
          const win = event.sender;
          if (!win.isDestroyed()) {
            win.send('sync:progress', line);
          }
        }
      });

      syncProcess.on('exit', (code) => {
        syncProcess = null;

        // Save sync timestamp
        const now = new Date().toISOString();
        try {
          fs.writeFileSync(syncStatePath, JSON.stringify({
            lastSync: now,
            code,
          }) + '\n');
        } catch (_) {}

        if (code === 0) {
          try {
            const result = JSON.parse(stdout);
            resolve({ ok: true, ...result, lastSync: now });
          } catch (_) {
            resolve({ ok: true, lastSync: now });
          }
        } else {
          resolve({ ok: false, error: `candle_fetcher exited with code ${code}` });
        }
      });

      syncProcess.on('error', (err) => {
        syncProcess = null;
        resolve({ ok: false, error: err.message });
      });
    });
  });

  // ── Sync status ───────────────────────────────────────────────────────
  ipcMain.handle('sync:status', async () => {
    // Last sync time
    let lastSync = null;
    try {
      const raw = fs.readFileSync(syncStatePath, 'utf-8');
      const data = JSON.parse(raw);
      lastSync = data.lastSync;
    } catch (_) {}

    // Cache DB size
    let dbSize = 0;
    try {
      const stat = fs.statSync(dbPath);
      dbSize = stat.size;
    } catch (_) {}

    // Check if sync needed (> 7 days since last sync)
    let needsSync = true;
    if (lastSync) {
      const daysSince = (Date.now() - new Date(lastSync).getTime()) / 86400000;
      needsSync = daysSince >= 7;
    }

    return {
      lastSync,
      dbSizeMB: (dbSize / 1048576).toFixed(1),
      needsSync,
      running: syncProcess != null && !syncProcess.killed,
    };
  });

  // ── Auto-sync: check on startup and every 24h ─────────────────────────
  async function checkAutoSync() {
    let lastSync = null;
    try {
      const raw = fs.readFileSync(syncStatePath, 'utf-8');
      const data = JSON.parse(raw);
      lastSync = data.lastSync;
    } catch (_) {}

    if (!lastSync) return; // Don't auto-sync on first run — let user trigger it

    const daysSince = (Date.now() - new Date(lastSync).getTime()) / 86400000;
    if (daysSince >= 7 && !syncProcess) {
      console.log('[auto-sync] Last sync was %.1f days ago, starting sync...', daysSince);

      // Run sync silently in background
      const proc = spawn(binary, [
        '--days', '1461',
        '--coins', 'ETH,BTC,SOL,DOGE,HYPE,PUMP',
        '--intervals', '5m,15m,1h,4h,1d',
        '--db', dbPath,
      ], { cwd: projectRoot, stdio: 'ignore' });

      proc.on('exit', (code) => {
        if (code === 0) {
          try {
            fs.writeFileSync(syncStatePath, JSON.stringify({
              lastSync: new Date().toISOString(),
              code: 0,
              auto: true,
            }) + '\n');
          } catch (_) {}
          console.log('[auto-sync] Complete');

          // Notify renderer
          const win = getWindow();
          if (win && !win.isDestroyed()) {
            win.send('sync:auto-complete');
          }
        }
      });
    }
  }

  // Check on startup (delay 10s to let app settle)
  setTimeout(checkAutoSync, 10000);

  // Check every 24h
  setInterval(checkAutoSync, 86400000);
};
