const { app, BrowserWindow, ipcMain } = require('electron');
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

const PROJECT_ROOT = path.resolve(__dirname, '..', '..');
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
      sandbox: false, // needed for better-sqlite3 in preload
    },
  });

  if (isDev) {
    mainWindow.loadURL('http://localhost:5173');
  } else {
    mainWindow.loadFile(path.join(__dirname, '..', 'dist', 'index.html'));
  }

  mainWindow.on('closed', () => {
    mainWindow = null;
  });
}

app.whenReady().then(() => {
  createWindow();

  // Register all IPC handlers
  registerBotIPC(ipcMain, PROJECT_ROOT);
  registerConfigIPC(ipcMain, PROJECT_ROOT);
  registerStrategiesIPC(ipcMain, PROJECT_ROOT);
  registerBacktestIPC(ipcMain, PROJECT_ROOT);
  registerDbIPC(ipcMain, PROJECT_ROOT);
  registerLogsIPC(ipcMain, PROJECT_ROOT, () => mainWindow);
  registerSyncIPC(ipcMain, PROJECT_ROOT, () => mainWindow);
  registerWsIPC(ipcMain, PROJECT_ROOT, () => mainWindow);
  registerMarketIPC(ipcMain, PROJECT_ROOT);

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});
