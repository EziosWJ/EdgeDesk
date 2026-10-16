/**
 * Minimal browser globals for unit tests. The stores only need window metrics
 * and localStorage; no DOM environment (jsdom) is required.
 */
const storage = new Map<string, string>();

const windowStub = {
  innerWidth: 1440,
  innerHeight: 900,
  localStorage: {
    getItem: (key: string) => storage.get(key) ?? null,
    setItem: (key: string, value: string) => void storage.set(key, value),
    removeItem: (key: string) => void storage.delete(key),
  },
};

(globalThis as unknown as { window: typeof windowStub }).window = windowStub;

// React 19 testing flag (used by the DOM interaction tests).
(globalThis as unknown as { IS_REACT_ACT_ENVIRONMENT: boolean }).IS_REACT_ACT_ENVIRONMENT = true;
