import { connect, installErrorReporting, RpcError } from './vendor/edgedesk-sdk.js';

const statusEl = document.getElementById('status');
const healthEl = document.getElementById('health-output');
const notesEl = document.getElementById('notes');
const errorEl = document.getElementById('error');
const inputEl = document.getElementById('note-text');

let client;

function describe(error) {
  if (error instanceof RpcError) {
    const slug = error.data?.slug ? ` (${error.data.slug})` : '';
    if (['backend_not_configured', 'backend_not_running', 'backend_unavailable'].includes(error.data?.slug)) {
      return `${error.message}${slug}. Configure a run user and start the backend in EdgeDesk, then retry.`;
    }
    return `${error.message}${slug}`;
  }
  return error instanceof Error ? error.message : String(error);
}

function showError(error) {
  errorEl.textContent = error ? describe(error) : '';
}

async function backend(method, path, body) {
  const options = body === undefined ? {} : { body };
  return client.backendRequest(method, path, options);
}

async function refresh() {
  showError(null);
  try {
    const [health, notes] = await Promise.all([backend('GET', '/health'), backend('GET', '/notes')]);
    healthEl.textContent = `HTTP ${health.status} ${JSON.stringify(health.body)}`;
    if (notes.status !== 200) throw new Error(`GET /notes returned HTTP ${notes.status}`);
    const items = Array.isArray(notes.body?.notes) ? notes.body.notes : [];
    notesEl.replaceChildren(
      ...items.map((text) => {
        const item = document.createElement('li');
        item.textContent = text;
        return item;
      }),
    );
    statusEl.textContent = 'backend reachable';
    statusEl.className = 'status ok';
    if (items.length === 0) {
      const item = document.createElement('li');
      item.className = 'empty';
      item.textContent = 'No notes yet.';
      notesEl.append(item);
    }
  } catch (error) {
    statusEl.textContent = 'backend unavailable';
    statusEl.className = 'status error';
    showError(error);
  }
}

async function addNote(event) {
  event.preventDefault();
  const text = inputEl.value.trim();
  if (!text) return;
  showError(null);
  try {
    const response = await backend('POST', '/notes', { text });
    if (response.status !== 201) throw new Error(`POST /notes returned HTTP ${response.status}`);
    inputEl.value = '';
    await refresh();
  } catch (error) {
    showError(error);
  }
}

async function main() {
  try {
    client = await connect();
  } catch (error) {
    statusEl.textContent = 'not connected';
    statusEl.className = 'status error';
    showError(error);
    return;
  }
  installErrorReporting(client);
  document.getElementById('health').addEventListener('click', refresh);
  document.getElementById('note-form').addEventListener('submit', addNote);
  await refresh();
}

void main();
