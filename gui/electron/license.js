/**
 * License module — Ed25519 verification + AES-256-GCM storage
 *
 * The app only embeds the PUBLIC key. Signing requires the admin's private key.
 * The license file is encrypted with a key derived from the machine ID,
 * so it cannot be decrypted on another machine.
 */

const crypto = require('crypto');
const { execSync } = require('child_process');
const os = require('os');
const fs = require('fs');
const path = require('path');
const { app } = require('electron');

// ─── Ed25519 public key (PEM) ────────────────────────────────
// Generate with: node scripts/license_admin.js generate-keys
const PUBLIC_KEY_PEM = `-----BEGIN PUBLIC KEY-----
MCowBQYDK2VwAyEAl070P8PesnsXKUBUeIYTvtJZNyiYOETExbuUtmxsHyc=
-----END PUBLIC KEY-----`;

// ─── Machine fingerprint ────────────────────────────────────
// Uses OS-level hardware UUID (immutable, survives reboots/network changes)
// - macOS: IOPlatformUUID from IORegistry
// - Linux: /etc/machine-id
// - Windows: BIOS UUID via wmic

function getHardwareUUID() {
  try {
    if (process.platform === 'darwin') {
      const output = execSync(
        'ioreg -rd1 -c IOPlatformExpertDevice',
        { encoding: 'utf-8', timeout: 5000 }
      );
      const match = output.match(/"IOPlatformUUID"\s*=\s*"([^"]+)"/);
      if (match) return match[1];
    } else if (process.platform === 'linux') {
      if (fs.existsSync('/etc/machine-id')) {
        return fs.readFileSync('/etc/machine-id', 'utf-8').trim();
      }
      if (fs.existsSync('/var/lib/dbus/machine-id')) {
        return fs.readFileSync('/var/lib/dbus/machine-id', 'utf-8').trim();
      }
    } else if (process.platform === 'win32') {
      const output = execSync(
        'wmic csproduct get UUID',
        { encoding: 'utf-8', timeout: 5000 }
      );
      const lines = output.trim().split('\n');
      if (lines.length >= 2) {
        const uuid = lines[1].trim();
        if (uuid && uuid !== 'FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF') return uuid;
      }
    }
  } catch {
    // Fall through to fallback
  }
  return null;
}

function getMachineId() {
  const hwid = getHardwareUUID();
  if (hwid) {
    const hash = crypto.createHash('sha256').update(hwid).digest('hex');
    return hash.substring(0, 16);
  }

  // Fallback: CPU + platform + arch + RAM (no network — avoids instability)
  const fallback = [
    os.cpus()[0]?.model || 'unknown-cpu',
    os.platform(),
    os.arch(),
    String(Math.round(os.totalmem() / (1024 * 1024 * 1024))),
  ].join('|');
  const hash = crypto.createHash('sha256').update(fallback).digest('hex');
  return hash.substring(0, 16);
}

// ─── Crypto helpers ─────────────────────────────────────────

function deriveKey(machineId) {
  return crypto.createHash('sha256').update(`license-key:${machineId}`).digest();
}

function encryptData(data, machineId) {
  const key = deriveKey(machineId);
  const iv = crypto.randomBytes(12);
  const cipher = crypto.createCipheriv('aes-256-gcm', key, iv);
  const encrypted = Buffer.concat([cipher.update(data, 'utf-8'), cipher.final()]);
  const tag = cipher.getAuthTag();
  // Format: iv(12) + tag(16) + ciphertext
  return Buffer.concat([iv, tag, encrypted]);
}

function decryptData(buf, machineId) {
  const key = deriveKey(machineId);
  const iv = buf.subarray(0, 12);
  const tag = buf.subarray(12, 28);
  const ciphertext = buf.subarray(28);
  const decipher = crypto.createDecipheriv('aes-256-gcm', key, iv);
  decipher.setAuthTag(tag);
  return decipher.update(ciphertext, null, 'utf-8') + decipher.final('utf-8');
}

function getPublicKey() {
  return crypto.createPublicKey(PUBLIC_KEY_PEM);
}

function getLicensePath() {
  return path.join(app.getPath('userData'), 'license.dat');
}

// ─── Public API ─────────────────────────────────────────────

function checkLicense() {
  const machineId = getMachineId();
  const licensePath = getLicensePath();

  if (!fs.existsSync(licensePath)) {
    return { valid: false, machineId };
  }

  try {
    const encrypted = fs.readFileSync(licensePath);
    const json = decryptData(encrypted, machineId);
    const data = JSON.parse(json);

    // Verify machine ID matches
    if (data.m !== machineId) {
      return { valid: false, machineId, error: 'Machine ID mismatch' };
    }

    // Verify Ed25519 signature
    const message = `${data.c}:${data.m}`;
    const signature = Buffer.from(data.s, 'base64');
    const pubKey = getPublicKey();
    const valid = crypto.verify(null, Buffer.from(message), pubKey, signature);

    if (!valid) {
      return { valid: false, machineId, error: 'Invalid signature' };
    }

    return {
      valid: true,
      machineId,
      code: data.c,
      activatedAt: data.activatedAt,
    };
  } catch (err) {
    return { valid: false, machineId, error: err.message };
  }
}

function activateLicense(token) {
  const machineId = getMachineId();

  // Strip all whitespace (terminal line wraps, copy-paste artifacts)
  const clean = token.replace(/\s+/g, '');

  // Decode base64url token
  let payload;
  try {
    const jsonStr = Buffer.from(clean, 'base64url').toString('utf-8');
    payload = JSON.parse(jsonStr);
  } catch {
    throw new Error('Token invalide (format incorrect)');
  }

  const { c: code, m: tokenMachineId, s: sigBase64 } = payload;

  if (!code || !tokenMachineId || !sigBase64) {
    throw new Error('Token invalide (champs manquants)');
  }

  // Verify machine ID
  if (tokenMachineId !== machineId) {
    throw new Error(`Token genere pour une autre machine (attendu: ${machineId})`);
  }

  // Verify Ed25519 signature
  const message = `${code}:${tokenMachineId}`;
  const signature = Buffer.from(sigBase64, 'base64');
  const pubKey = getPublicKey();
  const valid = crypto.verify(null, Buffer.from(message), pubKey, signature);

  if (!valid) {
    throw new Error('Signature invalide');
  }

  // Save encrypted license
  const licenseData = JSON.stringify({
    c: code,
    m: machineId,
    s: sigBase64,
    activatedAt: new Date().toISOString(),
  });

  const encrypted = encryptData(licenseData, machineId);
  const licensePath = getLicensePath();

  // Ensure directory exists
  const dir = path.dirname(licensePath);
  if (!fs.existsSync(dir)) {
    fs.mkdirSync(dir, { recursive: true });
  }

  fs.writeFileSync(licensePath, encrypted);
  return { code, machineId };
}

function getLicenseInfo() {
  const result = checkLicense();
  if (!result.valid) return null;

  // Mask code: show first 8 chars + "..."
  const maskedCode = result.code
    ? result.code.substring(0, 8) + '...'
    : 'unknown';

  return {
    code: maskedCode,
    activatedAt: result.activatedAt,
    machineId: result.machineId,
  };
}

module.exports = { getMachineId, checkLicense, activateLicense, getLicenseInfo };
