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

  ipcMain.handle('config:write', async (_event, newConfig) => {
    try {
      // Read existing to preserve fields we stripped
      const raw = fs.readFileSync(configPath, 'utf-8');
      const existing = JSON.parse(raw);

      // Merge: keep secret fields from existing, update the rest
      const merged = { ...existing, ...newConfig };
      if (existing.wallet) merged.wallet = existing.wallet;
      if (existing.private_key) merged.private_key = existing.private_key;
      if (existing.secret) merged.secret = existing.secret;
      if (existing.api_key) merged.api_key = existing.api_key;

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
