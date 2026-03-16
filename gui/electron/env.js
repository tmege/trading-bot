/**
 * Centralized .env reader for the Electron GUI.
 *
 * SECURITY: Only reads TB_WALLET_ADDRESS and TB_ANTHROPIC_API_KEY.
 * TB_PRIVATE_KEY is NEVER loaded into GUI memory — the C engine
 * reads it directly from the process environment via getenv() + unsetenv().
 *
 * The file is read once at require() time (app startup).
 * Call reload() if .env changes at runtime.
 */
const fs = require('fs');
const path = require('path');

// Keys that are safe to cache in the GUI process
const SAFE_KEYS = new Set(['TB_WALLET_ADDRESS', 'TB_ANTHROPIC_API_KEY']);

let cached = {};

function load(projectRoot) {
  cached = {};
  try {
    const envPath = path.join(projectRoot, '.env');
    const content = fs.readFileSync(envPath, 'utf-8');
    for (const line of content.split('\n')) {
      const trimmed = line.trim();
      if (!trimmed || trimmed.startsWith('#')) continue;
      const eqIdx = trimmed.indexOf('=');
      if (eqIdx === -1) continue;
      const key = trimmed.slice(0, eqIdx).trim();
      if (!SAFE_KEYS.has(key)) continue; // Skip all secrets
      let val = trimmed.slice(eqIdx + 1).trim();
      if ((val.startsWith('"') && val.endsWith('"')) ||
          (val.startsWith("'") && val.endsWith("'"))) {
        val = val.slice(1, -1);
      }
      if (val) cached[key] = val;
    }
  } catch (_) {
    // .env not found — not fatal
  }
}

function getWalletAddress() {
  return cached.TB_WALLET_ADDRESS || null;
}

function getAnthropicApiKey() {
  return cached.TB_ANTHROPIC_API_KEY || null;
}

function reload(projectRoot) {
  load(projectRoot);
}

module.exports = { load, reload, getWalletAddress, getAnthropicApiKey };
