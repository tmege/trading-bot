const fs = require('fs');
const path = require('path');
let chokidar;
try {
  chokidar = require('chokidar');
} catch (_) {
  chokidar = null;
}

let watcher = null;
let currentLogFile = null;
let lastSize = 0;

function getLatestLogFile(logsDir) {
  try {
    const files = fs.readdirSync(logsDir)
      .filter(f => f.startsWith('bot_') && f.endsWith('.log'))
      .sort()
      .reverse();
    return files.length > 0 ? path.join(logsDir, files[0]) : null;
  } catch (_) {
    return null;
  }
}

module.exports = function registerLogsIPC(ipcMain, projectRoot, getWindow) {
  const logsDir = path.join(projectRoot, 'logs');

  ipcMain.handle('logs:tail', async (_event, lines = 200) => {
    const logFile = getLatestLogFile(logsDir);
    if (!logFile) return { ok: false, error: 'No log file found' };

    try {
      const content = fs.readFileSync(logFile, 'utf-8');
      const allLines = content.split('\n');
      const safeLines = Math.min(Math.max(1, Number(lines) || 200), 2000);
      const tail = allLines.slice(-safeLines).join('\n');
      return { ok: true, content: tail, file: path.basename(logFile) };
    } catch (err) {
      return { ok: false, error: err.message };
    }
  });

  // Start watching for log changes
  if (chokidar) {
    try {
      watcher = chokidar.watch(path.join(logsDir, 'bot_*.log'), {
        persistent: true,
        ignoreInitial: true,
      });

      watcher.on('change', (filePath) => {
        const win = getWindow();
        if (!win || win.isDestroyed()) return;

        try {
          const stat = fs.statSync(filePath);
          if (stat.size <= lastSize && filePath === currentLogFile) return;

          // Read only new content
          const fd = fs.openSync(filePath, 'r');
          const readFrom = (filePath === currentLogFile) ? lastSize : 0;
          const bufSize = stat.size - readFrom;

          if (bufSize <= 0) {
            fs.closeSync(fd);
            return;
          }

          const buf = Buffer.alloc(bufSize);
          fs.readSync(fd, buf, 0, bufSize, readFrom);
          fs.closeSync(fd);

          currentLogFile = filePath;
          lastSize = stat.size;

          win.send('logs:line', buf.toString('utf-8'));
        } catch (_) {
          // File might be rotated — ignore
        }
      });

      watcher.on('add', (filePath) => {
        // New log file — reset tracking
        currentLogFile = filePath;
        lastSize = 0;
      });
    } catch (_) {
      // chokidar init failed — logs won't stream but app works
    }
  }
};
