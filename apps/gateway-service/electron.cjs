const { app, BrowserWindow, shell } = require('electron');
const { spawn } = require('child_process');
const path = require('path');
const fs = require('fs');

let gatewayProcess = null;
let mainWindow = null;

const isDev = !app.isPackaged;

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1200,
    height: 800,
    webPreferences: {
      nodeIntegration: false,
      contextIsolation: true,
    },
    title: 'R9700 AI Gateway',
  });

  if (isDev) {
    mainWindow.loadURL('http://localhost:11434');
    mainWindow.webContents.openDevTools();
  } else {
    mainWindow.loadFile(path.join(__dirname, 'public', 'dashboard', 'index.html'));
  }

  mainWindow.on('closed', () => {
    mainWindow = null;
  });

  mainWindow.webContents.setWindowOpenHandler(({ url }) => {
    shell.openExternal(url);
    return { action: 'deny' };
  });
}

function startGateway() {
  const gatewayPath = path.join(__dirname, 'dist', 'main.js');

  if (!fs.existsSync(gatewayPath)) {
    console.error('Gateway not built. Run: pnpm build');
    return;
  }

  const configPath = path.join(__dirname, 'config', 'gateway.local.yaml');
  const configArg = fs.existsSync(configPath) ? configPath : path.join(__dirname, '..', '..', 'config', 'gateway.local.yaml');

  gatewayProcess = spawn(process.execPath, [gatewayPath, '--config', configArg], {
    stdio: 'inherit',
    env: { ...process.env, NODE_ENV: 'production' }
  });

  gatewayProcess.on('close', (code) => {
    console.log(`Gateway exited with code ${code}`);
    app.quit();
  });
}

app.whenReady().then(() => {
  console.log('Starting R9700 AI Gateway...');
  startGateway();
  createWindow();

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      createWindow();
    }
  });
});

app.on('window-all-closed', () => {
  if (gatewayProcess) {
    gatewayProcess.kill();
  }
  if (process.platform !== 'darwin') {
    app.quit();
  }
});

app.on('before-quit', () => {
  if (gatewayProcess) {
    gatewayProcess.kill();
  }
});