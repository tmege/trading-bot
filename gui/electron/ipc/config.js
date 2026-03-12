const fs = require('fs');
const path = require('path');

module.exports = function registerConfigIPC(ipcMain, projectRoot) {
  const configPath = path.join(projectRoot, 'config', 'bot_config.json');

  ipcMain.handle('config:read', async () => {
    try {
      const raw = fs.readFileSync(configPath, 'utf-8');
      const cfg = JSON.parse(raw);

      // Strip secrets: remove any wallet/private key fields before sending
      // to renderer (defense-in-depth with contextIsolation)
      const safe = { ...cfg };
      delete safe.wallet;
      delete safe.private_key;
      delete safe.secret;
      delete safe.api_key;

      return { ok: true, config: safe };
    } catch (err) {
      return { ok: false, error: err.message };
    }
  });

  // SECURITY: Only these top-level keys can be modified from the renderer.
  // This prevents a compromised renderer (XSS) from injecting arbitrary
  // config like strategies.dir or exchange.rest_url.
  const WRITABLE_KEYS = new Set([
    'mode', 'risk', 'strategies', 'logging',
  ]);

  ipcMain.handle('config:write', async (_event, newConfig) => {
    try {
      const raw = fs.readFileSync(configPath, 'utf-8');
      const existing = JSON.parse(raw);

      // Allowlist merge: only copy permitted top-level keys
      const merged = { ...existing };
      for (const key of Object.keys(newConfig)) {
        if (!WRITABLE_KEYS.has(key)) {
          console.warn(`[config:write] REJECTED key '${key}' — not in allowlist`);
          continue;
        }
        merged[key] = newConfig[key];
      }

      // Always preserve secrets from existing config
      if (existing.wallet) merged.wallet = existing.wallet;
      if (existing.private_key) merged.private_key = existing.private_key;
      if (existing.secret) merged.secret = existing.secret;
      if (existing.api_key) merged.api_key = existing.api_key;

      // Never allow exchange URLs to be modified from renderer
      merged.exchange = existing.exchange;

      // Atomic write: write to .tmp then rename
      const tmpPath = configPath + '.tmp';
      fs.writeFileSync(tmpPath, JSON.stringify(merged, null, 4) + '\n', 'utf-8');
      fs.renameSync(tmpPath, configPath);

      return { ok: true };
    } catch (err) {
      return { ok: false, error: err.message };
    }
  });
};
