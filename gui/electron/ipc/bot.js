const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');

let botProcess = null;
let startTime = null;

/**
 * Parse .env file into key-value pairs (safe: no shell execution).
 * Supports KEY=VALUE, KEY="VALUE", KEY='VALUE', comments (#), blank lines.
 */
function loadDotEnv(filePath) {
  const env = {};
  try {
    const content = fs.readFileSync(filePath, 'utf-8');
    for (const line of content.split('\n')) {
      const trimmed = line.trim();
      if (!trimmed || trimmed.startsWith('#')) continue;
      const eqIdx = trimmed.indexOf('=');
      if (eqIdx === -1) continue;
      const key = trimmed.slice(0, eqIdx).trim();
      let val = trimmed.slice(eqIdx + 1).trim();
      // Strip surrounding quotes
      if ((val.startsWith('"') && val.endsWith('"')) ||
          (val.startsWith("'") && val.endsWith("'"))) {
        val = val.slice(1, -1);
      }
      env[key] = val;
    }
  } catch (_) {
    // .env not found — not fatal, env vars may be set externally
  }
  return env;
}

module.exports = function registerBotIPC(ipcMain, projectRoot) {
  const botBinary = path.join(projectRoot, 'build', 'trading_bot');
  const configPath = path.join(projectRoot, 'config', 'bot_config.json');
  const envPath = path.join(projectRoot, '.env');

  ipcMain.handle('bot:start', async (event) => {
    if (botProcess && !botProcess.killed) {
      return { ok: false, error: 'Bot already running' };
    }

    try {
      // Load .env and merge with current process env
      const dotEnv = loadDotEnv(envPath);
      const childEnv = { ...process.env, ...dotEnv };

      botProcess = spawn(botBinary, [configPath], {
        cwd: projectRoot,
        stdio: ['ignore', 'pipe', 'pipe'],
        env: childEnv,
      });

      // SECURITY: Wipe private key from JS objects after spawn.
      // The C engine reads it from getenv() then calls unsetenv().
      // We can't truly zero JS strings, but removing references
      // makes them eligible for GC immediately.
      if (dotEnv.TB_PRIVATE_KEY) dotEnv.TB_PRIVATE_KEY = '';
      if (childEnv.TB_PRIVATE_KEY) childEnv.TB_PRIVATE_KEY = '';

      startTime = Date.now();

      botProcess.stdout.on('data', (data) => {
        const win = event.sender;
        if (!win.isDestroyed()) {
          win.send('bot:log', data.toString());
        }
      });

      botProcess.stderr.on('data', (data) => {
        const win = event.sender;
        if (!win.isDestroyed()) {
          win.send('bot:log', data.toString());
        }
      });

      botProcess.on('exit', (code, signal) => {
        const win = event.sender;
        if (!win.isDestroyed()) {
          win.send('bot:log',
            `\n[Process exited: code=${code} signal=${signal}]\n`);
        }
        botProcess = null;
        startTime = null;
      });

      return { ok: true, pid: botProcess.pid };
    } catch (err) {
      return { ok: false, error: err.message };
    }
  });

  ipcMain.handle('bot:stop', async () => {
    if (!botProcess || botProcess.killed) {
      return { ok: false, error: 'Bot not running' };
    }

    return new Promise((resolve) => {
      // Graceful shutdown with SIGTERM
      botProcess.kill('SIGTERM');

      const timeout = setTimeout(() => {
        if (botProcess && !botProcess.killed) {
          botProcess.kill('SIGKILL');
        }
        resolve({ ok: true, forced: true });
      }, 5000);

      botProcess.once('exit', () => {
        clearTimeout(timeout);
        resolve({ ok: true, forced: false });
      });
    });
  });

  ipcMain.handle('bot:status', async () => {
    const running = botProcess != null && !botProcess.killed;
    return {
      running,
      pid: running ? botProcess.pid : null,
      uptimeSec: running ? Math.floor((Date.now() - startTime) / 1000) : 0,
    };
  });
};
