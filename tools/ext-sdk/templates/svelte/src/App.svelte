<script>
  import { onMount } from 'svelte';
  import { connect, installErrorReporting } from '../vendor/edgedesk-sdk.js';

  let client = null;
  let info = null;
  let count = 0;
  let error = '';

  onMount(async () => {
    try {
      client = await connect();
      installErrorReporting(client);
      info = await client.getInfo();
      const stored = await client.storageGet('count').catch(() => null);
      count = Number(stored?.value ?? 0);
    } catch (caught) {
      error = caught.message;
    }
    return () => client?.dispose?.();
  });

  async function increment() {
    if (!client) return;
    const next = count + 1;
    try {
      await client.storageSet('count', next);
      count = next;
    } catch (caught) {
      error = caught.message;
    }
  }
</script>

<main>
  <p class="eyebrow">SVELTE EXTENSION</p>
  <h1>Svelte 扩展示例</h1>
  {#if error}
    <p class="error">{error}</p>
  {:else if !client}
    <p>正在连接 EdgeDesk 宿主…</p>
  {/if}
  {#if info}
    <p class="muted">{info.appId} · {info.version} · 宿主 API {info.hostApiVersion}</p>
  {/if}
  <button type="button" disabled={!client} onclick={increment}>计数：{count}</button>
  <p class="muted">计数保存在平台隔离 KV 中。</p>
</main>
