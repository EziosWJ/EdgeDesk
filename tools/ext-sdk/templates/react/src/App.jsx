import { useEffect, useState } from 'react';
import { connect, installErrorReporting } from '../vendor/edgedesk-sdk.js';

export function App() {
  const [client, setClient] = useState(null);
  const [info, setInfo] = useState(null);
  const [count, setCount] = useState(0);
  const [error, setError] = useState('');

  useEffect(() => {
    let active = true;
    connect()
      .then(async (next) => {
        installErrorReporting(next);
        const appInfo = await next.getInfo();
        const stored = await next.storageGet('count').catch(() => null);
        if (!active) return;
        setClient(next);
        setInfo(appInfo);
        setCount(Number(stored?.value ?? 0));
      })
      .catch((caught) => active && setError(caught.message));
    return () => {
      active = false;
      client?.dispose?.();
    };
  }, []);

  const increment = async () => {
    if (!client) return;
    const next = count + 1;
    try {
      await client.storageSet('count', next);
      setCount(next);
    } catch (caught) {
      setError(caught.message);
    }
  };

  return (
    <main>
      <p className="eyebrow">REACT EXTENSION</p>
      <h1>React 扩展示例</h1>
      {error && <p className="error">{error}</p>}
      {!client && !error && <p>正在连接 EdgeDesk 宿主…</p>}
      {info && (
        <p className="muted">
          {info.appId} · {info.version} · 宿主 API {info.hostApiVersion}
        </p>
      )}
      <button type="button" onClick={increment} disabled={!client}>
        计数：{count}
      </button>
      <p className="muted">计数保存在平台隔离 KV 中。</p>
    </main>
  );
}
