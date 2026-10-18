import { connect, installErrorReporting, RpcError } from './vendor/edgedesk-sdk.js';

const statusEl = document.getElementById('status');
const infoEl = document.getElementById('app-info');
const permissionsEl = document.getElementById('permissions');
const counterEl = document.getElementById('counter-value');
const usageEl = document.getElementById('storage-usage');
const entriesEl = document.getElementById('entries');
const terminalOutput = document.getElementById('terminal-output');

let client;
let terminalSession = '';
let counter = 0;

function setStatus(text, kind = '') {
  statusEl.textContent = text;
  statusEl.className = `status ${kind}`.trim();
}

function describe(error) {
  if (error instanceof RpcError) {
    return `${error.message}${error.data?.slug ? ` (${error.data.slug})` : ''}`;
  }
  return error instanceof Error ? error.message : String(error);
}

function appendOutput(text) {
  terminalOutput.textContent += text;
  terminalOutput.scrollTop = terminalOutput.scrollHeight;
}

async function refreshStorage() {
  if (!client.info.permissions.includes('storage')) {
    usageEl.textContent = '未授予 storage 权限。';
    return;
  }
  try {
    const listing = await client.storageList({ limit: 20 });
    const present = listing.entries.some((item) => item.key === 'counter');
    const entry = present ? await client.storageGet('counter') : null;
    counter = Number(entry?.value ?? 0);
    counterEl.textContent = String(counter);
    usageEl.textContent = `${listing.entries.length} 个键 · ${listing.usageBytes} / ${listing.quotaBytes} 字节`;
  } catch (error) {
    usageEl.textContent = describe(error);
  }
}

async function saveCounter() {
  if (!client.info.permissions.includes('storage')) return;
  try {
    counter += 1;
    await client.storageSet('counter', counter);
    counterEl.textContent = String(counter);
    await refreshStorage();
  } catch (error) {
    usageEl.textContent = describe(error);
  }
}

async function listDirectory() {
  if (!client.info.permissions.includes('files.manage')) {
    entriesEl.innerHTML = '<li>未授予 files.manage 权限。</li>';
    return;
  }
  try {
    const path = document.getElementById('path').value || '/';
    const listing = await client.filesList(path, { limit: 100 });
    entriesEl.replaceChildren(
      ...listing.entries.map((entry) => {
        const item = document.createElement('li');
        const name = document.createElement('strong');
        name.textContent = `${entry.type === 'dir' ? '📁' : '📄'} ${entry.name}`;
        const meta = document.createElement('span');
        meta.textContent = entry.type === 'file' ? `${entry.size} B` : entry.type;
        item.append(name, meta);
        return item;
      }),
    );
    if (listing.entries.length === 0) {
      const item = document.createElement('li');
      item.textContent = '目录为空。';
      entriesEl.append(item);
    }
  } catch (error) {
    entriesEl.innerHTML = '';
    const item = document.createElement('li');
    item.className = 'error';
    item.textContent = describe(error);
    entriesEl.append(item);
  }
}

async function uploadFile() {
  if (!client.info.permissions.includes('files.manage')) return;
  const input = document.getElementById('upload');
  const file = input.files?.[0];
  if (!file) {
    usageEl.textContent = '请选择要上传的文件。';
    return;
  }
  const button = document.getElementById('upload-button');
  button.disabled = true;
  try {
    const entry = await client.uploadFile('/sample-upload.bin', file);
    appendOutput(`上传完成：${entry.name} (${entry.size} 字节)\n`);
    await listDirectory();
  } catch (error) {
    appendOutput(`上传失败：${describe(error)}\n`);
  } finally {
    button.disabled = false;
  }
}

async function createTerminal() {
  if (!client.info.permissions.includes('terminal.access')) {
    appendOutput('未授予 terminal.access 权限。\n');
    return;
  }
  try {
    const session = await client.terminalCreate({ title: '示例应用终端', cols: 90, rows: 24 });
    terminalSession = session.id;
    appendOutput(`终端已创建：${session.id}\n`);
  } catch (error) {
    appendOutput(`创建失败：${describe(error)}\n`);
  }
}

async function closeTerminal() {
  if (!terminalSession) return;
  try {
    await client.terminalClose(terminalSession);
  } catch (error) {
    appendOutput(`关闭失败：${describe(error)}\n`);
  }
  terminalSession = '';
}

async function sendTerminalInput(event) {
  event.preventDefault();
  const input = document.getElementById('terminal-input');
  if (!terminalSession || !input.value) return;
  const line = `${input.value}\r`;
  input.value = '';
  try {
    await client.terminalInput(terminalSession, line);
  } catch (error) {
    appendOutput(`发送失败：${describe(error)}\n`);
  }
}

async function main() {
  try {
    client = await connect();
  } catch (error) {
    setStatus('未连接', 'error');
    infoEl.textContent = describe(error);
    return;
  }
  installErrorReporting(client);
  setStatus('已连接', 'ok');
  const info = await client.getInfo();
  infoEl.textContent = `${info.appId} · 版本 ${info.version} · 宿主 API ${info.hostApiVersion} · 修订 ${String(info.revisionId).slice(0, 8)}`;
  permissionsEl.textContent = client.info.permissions.join(', ') || '无';

  client.on('terminal.output', (payload) => {
    if (payload?.sessionId === terminalSession) appendOutput(payload.data ?? '');
  });
  client.on('terminal.exit', (payload) => {
    if (payload?.sessionId === terminalSession) appendOutput(`\n[会话结束 code=${payload.code}]\n`);
  });
  client.on('terminal.guard', (payload) => {
    if (payload?.sessionId === terminalSession) appendOutput(`\n[已拦截] ${payload.reason}\n`);
  });
  client.on('terminal.error', (payload) => {
    if (payload?.sessionId === terminalSession) appendOutput(`\n[错误] ${payload.message}\n`);
  });

  document.getElementById('counter').addEventListener('click', saveCounter);
  document.getElementById('refresh-storage').addEventListener('click', refreshStorage);
  document.getElementById('list').addEventListener('click', listDirectory);
  document.getElementById('upload-button').addEventListener('click', uploadFile);
  document.getElementById('terminal-create').addEventListener('click', createTerminal);
  document.getElementById('terminal-close').addEventListener('click', closeTerminal);
  document.getElementById('terminal-form').addEventListener('submit', sendTerminalInput);
  await Promise.all([refreshStorage(), listDirectory()]);
}

void main();
