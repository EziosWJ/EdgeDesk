<script setup>
import { onBeforeUnmount, onMounted, ref } from 'vue';
import { connect, installErrorReporting } from '../vendor/edgedesk-sdk.js';

const client = ref(null);
const info = ref(null);
const count = ref(0);
const error = ref('');

onMounted(async () => {
  try {
    const next = await connect();
    installErrorReporting(next);
    client.value = next;
    info.value = await next.getInfo();
    const stored = await next.storageGet('count').catch(() => null);
    count.value = Number(stored?.value ?? 0);
  } catch (caught) {
    error.value = caught.message;
  }
});

onBeforeUnmount(() => client.value?.dispose?.());

async function increment() {
  if (!client.value) return;
  const next = count.value + 1;
  try {
    await client.value.storageSet('count', next);
    count.value = next;
  } catch (caught) {
    error.value = caught.message;
  }
}
</script>

<template>
  <main>
    <p class="eyebrow">VUE EXTENSION</p>
    <h1>Vue 扩展示例</h1>
    <p v-if="error" class="error">{{ error }}</p>
    <p v-else-if="!client">正在连接 EdgeDesk 宿主…</p>
    <p v-if="info" class="muted">{{ info.appId }} · {{ info.version }} · 宿主 API {{ info.hostApiVersion }}</p>
    <button type="button" :disabled="!client" @click="increment">计数：{{ count }}</button>
    <p class="muted">计数保存在平台隔离 KV 中。</p>
  </main>
</template>
