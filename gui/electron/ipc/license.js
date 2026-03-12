const { getMachineId, checkLicense, activateLicense } = require('../license');

module.exports = function registerLicenseIPC(ipcMain) {
  ipcMain.handle('license:check', async () => {
    try {
      const result = checkLicense();
      return { ok: true, valid: result.valid, machineId: result.machineId };
    } catch (err) {
      return { ok: false, valid: false, error: err.message };
    }
  });

  ipcMain.handle('license:activate', async (_event, token) => {
    try {
      activateLicense(token);
      return { ok: true };
    } catch (err) {
      return { ok: false, error: err.message };
    }
  });

  ipcMain.handle('license:machineId', async () => {
    try {
      const machineId = getMachineId();
      return { ok: true, machineId };
    } catch (err) {
      return { ok: false, error: err.message };
    }
  });
};
