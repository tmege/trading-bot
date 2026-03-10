const fs = require('fs');
const path = require('path');

module.exports = function registerStrategiesIPC(ipcMain, projectRoot) {
  const strategiesDir = path.join(projectRoot, 'strategies');
  const configPath = path.join(projectRoot, 'config', 'bot_config.json');

  ipcMain.handle('strategies:list', async () => {
    try {
      const files = fs.readdirSync(strategiesDir)
        .filter(f => f.endsWith('.lua'))
        .sort();

      // Read config to know which are active
      const raw = fs.readFileSync(configPath, 'utf-8');
      const cfg = JSON.parse(raw);
      const active = cfg.strategies?.active || [];

      const strategies = files.map(file => {
        const entry = active.find(a => a.file === file);

        // Read coins from config entry (multi-coin), fallback to Lua regex
        let coins = [];
        if (entry?.coins && Array.isArray(entry.coins)) {
          coins = entry.coins;
        } else {
          try {
            const content = fs.readFileSync(path.join(strategiesDir, file), 'utf-8');
            const match = content.match(/coin\s*=\s*["']([A-Z]+)["']/);
            if (match) coins = [match[1]];
          } catch (_) {}
        }

        return {
          file,
          coins,
          coin: coins[0] || null,  // backward compat
          active: !!entry,
          role: entry?.role || 'inactive',
        };
      });

      return { ok: true, strategies };
    } catch (err) {
      return { ok: false, error: err.message };
    }
  });

  ipcMain.handle('strategies:read', async (_event, filename) => {
    try {
      // Path traversal protection
      if (filename.includes('..') || filename.includes('/') || filename.includes('\\')) {
        return { ok: false, error: 'Invalid filename' };
      }

      const filePath = path.join(strategiesDir, filename);
      if (!filePath.startsWith(strategiesDir)) {
        return { ok: false, error: 'Path traversal rejected' };
      }

      const content = fs.readFileSync(filePath, 'utf-8');
      return { ok: true, content };
    } catch (err) {
      return { ok: false, error: err.message };
    }
  });

  ipcMain.handle('strategies:toggle', async (_event, filename, enabled) => {
    try {
      if (filename.includes('..') || filename.includes('/')) {
        return { ok: false, error: 'Invalid filename' };
      }

      const raw = fs.readFileSync(configPath, 'utf-8');
      const cfg = JSON.parse(raw);

      if (!cfg.strategies) cfg.strategies = {};
      if (!cfg.strategies.active) cfg.strategies.active = [];

      const idx = cfg.strategies.active.findIndex(a => a.file === filename);

      if (enabled && idx === -1) {
        cfg.strategies.active.push({ file: filename, role: 'secondary' });
      } else if (!enabled && idx !== -1) {
        cfg.strategies.active.splice(idx, 1);
      }

      const tmpPath = configPath + '.tmp';
      fs.writeFileSync(tmpPath, JSON.stringify(cfg, null, 4) + '\n', 'utf-8');
      fs.renameSync(tmpPath, configPath);

      return { ok: true };
    } catch (err) {
      return { ok: false, error: err.message };
    }
  });
};
