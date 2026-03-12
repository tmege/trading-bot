const WebSocket = require('ws');
const fs = require('fs');
const path = require('path');

module.exports = function registerWsIPC(ipcMain, projectRoot, getWindow) {
  let ws = null;
  let reconnectTimer = null;
  let walletAddress = null;

  function readWallet() {
    try {
      const envPath = path.join(projectRoot, '.env');
      const content = fs.readFileSync(envPath, 'utf-8');
      for (const line of content.split('\n')) {
        const trimmed = line.trim();
        if (trimmed.startsWith('TB_WALLET_ADDRESS=')) {
          let val = trimmed.slice('TB_WALLET_ADDRESS='.length).trim();
          if ((val.startsWith('"') && val.endsWith('"')) ||
              (val.startsWith("'") && val.endsWith("'"))) {
            val = val.slice(1, -1);
          }
          return val;
        }
      }
    } catch (_) {}
    return null;
  }

  function send(win, channel, data) {
    if (win && !win.isDestroyed()) {
      win.send(channel, data);
    }
  }

  function emitStatus(status) {
    const win = getWindow();
    send(win, 'ws:status', { status });
  }

  function connect() {
    if (ws) {
      try { ws.close(); } catch (_) {}
    }

    emitStatus('reconnecting');
    walletAddress = readWallet();
    ws = new WebSocket('wss://api.hyperliquid.xyz/ws');

    ws.on('open', () => {
      console.log('[ws] connected to Hyperliquid');
      emitStatus('connected');

      // Subscribe to all mid prices
      ws.send(JSON.stringify({
        method: 'subscribe',
        subscription: { type: 'allMids' },
      }));

      // Subscribe to user events (fills, positions) if wallet available
      if (walletAddress) {
        ws.send(JSON.stringify({
          method: 'subscribe',
          subscription: { type: 'userEvents', user: walletAddress },
        }));
      }
    });

    ws.on('message', (raw) => {
      const win = getWindow();
      if (!win || win.isDestroyed()) return;

      try {
        const msg = JSON.parse(raw.toString());
        const channel = msg.channel;
        const data = msg.data;

        if (channel === 'allMids') {
          // { mids: { "ETH": "2035.5", "BTC": "69800", ... } }
          send(win, 'ws:prices', data.mids || data);
        } else if (channel === 'userEvents') {
          // Fills and position updates
          send(win, 'ws:user', data);
        }
      } catch (_) {}
    });

    ws.on('close', () => {
      console.log('[ws] disconnected, reconnecting in 3s');
      emitStatus('disconnected');
      scheduleReconnect();
    });

    ws.on('error', (err) => {
      console.error('[ws] error:', err.message);
      emitStatus('disconnected');
      scheduleReconnect();
    });
  }

  function scheduleReconnect() {
    if (reconnectTimer) return;
    emitStatus('reconnecting');
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      connect();
    }, 3000);
  }

  // Start WebSocket on app launch
  connect();

  // Manual reconnect
  ipcMain.handle('ws:reconnect', async () => {
    connect();
    return { ok: true };
  });
};
