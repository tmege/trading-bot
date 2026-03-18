const { app, BrowserWindow, ipcMain, session } = require('electron');
const path = require('path');

const registerBotIPC = require('./ipc/bot');
const registerConfigIPC = require('./ipc/config');
const registerStrategiesIPC = require('./ipc/strategies');
const registerBacktestIPC = require('./ipc/backtest');
const registerDbIPC = require('./ipc/db');
const registerLogsIPC = require('./ipc/logs');
const registerSyncIPC = require('./ipc/sync');
const registerWsIPC = require('./ipc/ws');
const registerMarketIPC = require('./ipc/market');
const registerLicenseIPC = require('./ipc/license');
const registerAiDigestIPC = require('./ipc/ai-digest');
const env = require('./env');

const PROJECT_ROOT = path.resolve(__dirname, '..', '..');
env.load(PROJECT_ROOT);
const isDev = !app.isPackaged;

let mainWindow;

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1400,
    height: 900,
    minWidth: 1000,
    minHeight: 700,
    backgroundColor: '#0d1117',
    titleBarStyle: 'hiddenInset',
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      webviewTag: false,
      allowRunningInsecureContent: false,
    },
  });

  if (isDev) {
    mainWindow.loadURL('http://localhost:5173');
  } else {
    mainWindow.loadFile(path.join(__dirname, '..', 'dist', 'index.html'));
  }

  /* Prevent navigation to external URLs (XSS escalation protection) */
  mainWindow.webContents.on('will-navigate', (event, url) => {
    const allowed = isDev
      ? 'http://localhost:5173'
      : `file://${path.join(__dirname, '..', 'dist')}`;
    if (!url.startsWith(allowed)) {
      event.preventDefault();
    }
  });

  /* Block new window creation */
  mainWindow.webContents.setWindowOpenHandler(() => {
    return { action: 'deny' };
  });

  mainWindow.on('closed', () => {
    mainWindow = null;
  });
}

app.whenReady().then(() => {
  /* Content-Security-Policy — defense-in-depth against XSS */
  session.defaultSession.webRequest.onHeadersReceived((details, callback) => {
    const scriptSrc = isDev
      ? "script-src 'self' 'unsafe-inline'; "   // Vite HMR needs inline scripts
      : "script-src 'self'; ";
    callback({
      responseHeaders: {
        ...details.responseHeaders,
        'Content-Security-Policy': [
          "default-src 'self'; " +
          scriptSrc +
          "style-src 'self' 'unsafe-inline'; " +
          "connect-src 'self' https://api.hyperliquid.xyz https://api.coingecko.com https://api.alternative.me https://api.frankfurter.app wss://api.hyperliquid.xyz; " +
          "img-src 'self' data:; " +
          "font-src 'self';"
        ],
      },
    });
  });

  createWindow();

  // Register all IPC handlers
  registerLicenseIPC(ipcMain);
  registerBotIPC(ipcMain, PROJECT_ROOT);
  registerConfigIPC(ipcMain, PROJECT_ROOT);
  registerStrategiesIPC(ipcMain, PROJECT_ROOT);
  registerBacktestIPC(ipcMain, PROJECT_ROOT);
  registerDbIPC(ipcMain, PROJECT_ROOT);
  registerLogsIPC(ipcMain, PROJECT_ROOT, () => mainWindow);
  registerSyncIPC(ipcMain, PROJECT_ROOT, () => mainWindow);
  registerWsIPC(ipcMain, PROJECT_ROOT, () => mainWindow);
  registerMarketIPC(ipcMain, PROJECT_ROOT);
  registerAiDigestIPC(ipcMain, PROJECT_ROOT);

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});
