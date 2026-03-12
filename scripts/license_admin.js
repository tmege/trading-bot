#!/usr/bin/env node
/**
 * License Admin CLI — Ed25519 license management tool
 *
 * Commands:
 *   generate-keys          Generate Ed25519 key pair
 *   generate-codes [n=10]  Generate n UUID license codes
 *   sign --code <UUID> --machine <ID>  Sign a license token
 *   list                   List all codes and their status
 */

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const readline = require('readline');

const SCRIPTS_DIR = __dirname;
const PRIVATE_KEY_PATH = path.join(SCRIPTS_DIR, 'license_private.pem');
const PUBLIC_KEY_PATH = path.join(SCRIPTS_DIR, 'license_public.pem');
const CODES_PATH = path.join(SCRIPTS_DIR, 'license_codes.txt');

// ─── Helpers ─────────────────────────────────────────────────

function loadPrivateKey() {
  if (!fs.existsSync(PRIVATE_KEY_PATH)) {
    console.error('Erreur: cle privee non trouvee. Executez "generate-keys" d\'abord.');
    process.exit(1);
  }
  return crypto.createPrivateKey(fs.readFileSync(PRIVATE_KEY_PATH, 'utf-8'));
}

function loadCodes() {
  if (!fs.existsSync(CODES_PATH)) return [];
  return fs.readFileSync(CODES_PATH, 'utf-8')
    .split('\n')
    .filter(line => line.trim())
    .map(line => {
      const parts = line.split('\t');
      return {
        code: parts[0],
        status: parts[1] || 'available',
        machine: parts[2] || '',
        date: parts[3] || '',
      };
    });
}

function saveCodes(codes) {
  const lines = codes.map(c =>
    [c.code, c.status, c.machine, c.date].join('\t')
  );
  fs.writeFileSync(CODES_PATH, lines.join('\n') + '\n', 'utf-8');
}

function generateUUID() {
  return crypto.randomUUID();
}

// ─── Commands ────────────────────────────────────────────────

function cmdGenerateKeys() {
  if (fs.existsSync(PRIVATE_KEY_PATH)) {
    console.error('Erreur: les cles existent deja. Supprimez-les manuellement pour regenerer.');
    process.exit(1);
  }

  const { publicKey, privateKey } = crypto.generateKeyPairSync('ed25519', {
    publicKeyEncoding: { type: 'spki', format: 'pem' },
    privateKeyEncoding: { type: 'pkcs8', format: 'pem' },
  });

  fs.writeFileSync(PRIVATE_KEY_PATH, privateKey, { mode: 0o600 });
  fs.writeFileSync(PUBLIC_KEY_PATH, publicKey, { mode: 0o644 });

  console.log('Cles Ed25519 generees avec succes.');
  console.log(`  Cle privee: ${PRIVATE_KEY_PATH}`);
  console.log(`  Cle publique: ${PUBLIC_KEY_PATH}`);
  console.log('');
  console.log('Copiez le contenu de la cle publique dans gui/electron/license.js (PUBLIC_KEY_PEM) :');
  console.log(publicKey.trim());
}

function cmdGenerateCodes(n) {
  const count = parseInt(n, 10) || 10;
  const existing = loadCodes();
  const newCodes = [];

  for (let i = 0; i < count; i++) {
    const code = generateUUID();
    newCodes.push({ code, status: 'available', machine: '', date: '' });
  }

  saveCodes([...existing, ...newCodes]);
  console.log(`${count} codes generes dans ${CODES_PATH} :`);
  newCodes.forEach(c => console.log(`  ${c.code}`));
}

function cmdSign(args) {
  const codeIdx = args.indexOf('--code');
  const machineIdx = args.indexOf('--machine');

  if (codeIdx === -1 || machineIdx === -1) {
    console.error('Usage: sign --code <UUID> --machine <machineId>');
    process.exit(1);
  }

  const code = args[codeIdx + 1];
  const machineId = args[machineIdx + 1];

  if (!code || !machineId) {
    console.error('Usage: sign --code <UUID> --machine <machineId>');
    process.exit(1);
  }

  // Validate machineId format (16 hex chars)
  if (!/^[0-9a-f]{16}$/.test(machineId)) {
    console.error('Erreur: machineId doit etre 16 caracteres hexadecimaux.');
    process.exit(1);
  }

  const codes = loadCodes();
  const entry = codes.find(c => c.code === code);

  if (!entry) {
    console.error(`Erreur: code "${code}" non trouve dans ${CODES_PATH}.`);
    process.exit(1);
  }

  if (entry.status === 'used') {
    console.error(`Erreur: code "${code}" deja utilise (machine: ${entry.machine}, date: ${entry.date}).`);
    process.exit(1);
  }

  const privateKey = loadPrivateKey();
  const message = `${code}:${machineId}`;
  const signature = crypto.sign(null, Buffer.from(message), privateKey);

  // Build token
  const tokenPayload = JSON.stringify({
    c: code,
    m: machineId,
    s: signature.toString('base64'),
  });
  const token = Buffer.from(tokenPayload).toString('base64url');

  // Mark code as used
  entry.status = 'used';
  entry.machine = machineId;
  entry.date = new Date().toISOString().split('T')[0];
  saveCodes(codes);

  console.log('Token genere avec succes :');
  console.log('');
  console.log(token);
  console.log('');
  console.log(`Code ${code} marque comme utilise.`);
}

function cmdList() {
  const codes = loadCodes();
  if (codes.length === 0) {
    console.log('Aucun code. Executez "generate-codes" d\'abord.');
    return;
  }

  console.log('Codes de licence :');
  console.log('─'.repeat(80));
  for (const c of codes) {
    const status = c.status === 'used'
      ? `UTILISE  machine=${c.machine}  date=${c.date}`
      : 'DISPONIBLE';
    console.log(`  ${c.code}  ${status}`);
  }
  console.log('─'.repeat(80));
  const used = codes.filter(c => c.status === 'used').length;
  console.log(`Total: ${codes.length} | Utilises: ${used} | Disponibles: ${codes.length - used}`);
}

// ─── Main ────────────────────────────────────────────────────

const [,, command, ...rest] = process.argv;

switch (command) {
  case 'generate-keys':
    cmdGenerateKeys();
    break;
  case 'generate-codes':
    cmdGenerateCodes(rest[0]);
    break;
  case 'sign':
    cmdSign(rest);
    break;
  case 'list':
    cmdList();
    break;
  default:
    console.log('License Admin CLI');
    console.log('');
    console.log('Commandes :');
    console.log('  generate-keys           Generer une paire de cles Ed25519');
    console.log('  generate-codes [n=10]   Generer n codes de licence');
    console.log('  sign --code <UUID> --machine <ID>   Signer un token de licence');
    console.log('  list                    Lister tous les codes et leur statut');
    break;
}
