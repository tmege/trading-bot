const WebSocket = require('ws');
const env = require('../env');

module.exports = function registerWsIPC(ipcMain, projectRoot, getWindow) {
  let ws = null;
  let reconnectTimer = null;
  let walletAddress = null;

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
    walletAddress = env.getWalletAddress();
    ws = new WebSocket('wss://api.hyperliquid.xyz/ws');

    ws.on('open', () => {
      console.log('[ws] connected to Hyperliquid');
      emitStatus('connected');

      // Subscribe to all mid prices
      ws.send(JSON.stringify({
        method: 'subscribe',
        subscription: { type: 'allMids' },
      }));

      // Subscribe to user fills if wallet available
      if (walletAddress) {
        ws.send(JSON.stringify({
          method: 'subscribe',
          subscription: { type: 'userFills', user: walletAddress },
        }));
        console.log('[ws] subscribed to userFills');
      } else {
        console.warn('[ws] no wallet address — userFills subscription skipped');
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
        } else if (channel === 'userFills') {
          // { isSnapshot: bool, user: "0x...", fills: [...] }
          // Skip initial snapshot to avoid flood of old notifications
          if (data && data.isSnapshot) return;
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
