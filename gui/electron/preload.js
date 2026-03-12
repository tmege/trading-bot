const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('api', {
  // License management
  license: {
    check:     () => ipcRenderer.invoke('license:check'),
    activate:  (token) => ipcRenderer.invoke('license:activate', token),
    machineId: () => ipcRenderer.invoke('license:machineId'),
  },

  // Bot process management
  bot: {
    start:  ()       => ipcRenderer.invoke('bot:start'),
    stop:   ()       => ipcRenderer.invoke('bot:stop'),
    status: ()       => ipcRenderer.invoke('bot:status'),
    onLog:  (cb)     => {
      const listener = (_e, data) => cb(data);
      ipcRenderer.on('bot:log', listener);
      return () => ipcRenderer.removeListener('bot:log', listener);
    },
  },

  // Config management
  config: {
    read:  ()       => ipcRenderer.invoke('config:read'),
    write: (cfg)    => ipcRenderer.invoke('config:write', cfg),
  },

  // Strategies
  strategies: {
    list:   ()           => ipcRenderer.invoke('strategies:list'),
    read:   (filename)   => ipcRenderer.invoke('strategies:read', filename),
    toggle: (filename, enabled) =>
      ipcRenderer.invoke('strategies:toggle', filename, enabled),
  },

  // Backtest
  backtest: {
    run: (params) => ipcRenderer.invoke('backtest:run', params),
    history: () => ipcRenderer.invoke('backtest:history'),
    clearHistory: () => ipcRenderer.invoke('backtest:clearHistory'),
    onProgress: (cb) => {
      const listener = (_e, data) => cb(data);
      ipcRenderer.on('backtest:progress', listener);
      return () => ipcRenderer.removeListener('backtest:progress', listener);
    },
  },

  // Database queries
  db: {
    positions:      ()       => ipcRenderer.invoke('db:positions'),
    trades:         (n)      => ipcRenderer.invoke('db:trades', n),
    dailyPnl:       ()       => ipcRenderer.invoke('db:dailyPnl'),
    account:        ()       => ipcRenderer.invoke('db:account'),
    filteredTrades: (params) => ipcRenderer.invoke('db:filteredTrades', params),
    tradeFilters:   ()       => ipcRenderer.invoke('db:tradeFilters'),
    equityCurve:    ()       => ipcRenderer.invoke('db:equityCurve'),
    strategyPnl:    ()       => ipcRenderer.invoke('db:strategyPnl'),
    backtestCoins:  ()       => ipcRenderer.invoke('db:backtestCoins'),
    performanceMetrics: ()   => ipcRenderer.invoke('db:performanceMetrics'),
    strategyDetails:    ()   => ipcRenderer.invoke('db:strategyDetails'),
  },

  // Logs
  logs: {
    tail:   (n)  => ipcRenderer.invoke('logs:tail', n),
    onLine: (cb) => {
      const listener = (_e, line) => cb(line);
      ipcRenderer.on('logs:line', listener);
      return () => ipcRenderer.removeListener('logs:line', listener);
    },
  },

  // Real-time WebSocket
  ws: {
    onPrices: (cb) => {
      const listener = (_e, data) => cb(data);
      ipcRenderer.on('ws:prices', listener);
      return () => ipcRenderer.removeListener('ws:prices', listener);
    },
    onUser: (cb) => {
      const listener = (_e, data) => cb(data);
      ipcRenderer.on('ws:user', listener);
      return () => ipcRenderer.removeListener('ws:user', listener);
    },
    onStatus: (cb) => {
      const listener = (_e, data) => cb(data);
      ipcRenderer.on('ws:status', listener);
      return () => ipcRenderer.removeListener('ws:status', listener);
    },
  },

  // Market macro data
  market: {
    data: () => ipcRenderer.invoke('market:data'),
  },

  // Market data sync
  sync: {
    run:    ()  => ipcRenderer.invoke('sync:run'),
    status: ()  => ipcRenderer.invoke('sync:status'),
    onProgress: (cb) => {
      const listener = (_e, data) => cb(data);
      ipcRenderer.on('sync:progress', listener);
      return () => ipcRenderer.removeListener('sync:progress', listener);
    },
    onAutoComplete: (cb) => {
      const listener = () => cb();
      ipcRenderer.on('sync:auto-complete', listener);
      return () => ipcRenderer.removeListener('sync:auto-complete', listener);
    },
  },
});
