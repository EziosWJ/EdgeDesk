/**
 * Minimal browser globals for unit tests. The stores only need window metrics
 * and localStorage; no DOM environment (jsdom) is required.
 */
const storage = new Map<string, string>();

// React 19 reads `window.document` at module load to decide whether it can use
// DOM feature detection. Attach the DOM environment when there is one so React
// registers its real input/change listeners instead of the legacy polyfill.
const dom = (globalThis as unknown as { document?: unknown }).document;
const windowEvents = new EventTarget();
const windowStub = {
  innerWidth: 1440,
  innerHeight: 900,
  location: { protocol: 'http:', host: 'localhost' },
  ...(dom ? { document: dom } : {}),
  localStorage: {
    getItem: (key: string) => storage.get(key) ?? null,
    setItem: (key: string, value: string) => void storage.set(key, value),
    removeItem: (key: string) => void storage.delete(key),
  },
  setInterval: globalThis.setInterval.bind(globalThis),
  clearInterval: globalThis.clearInterval.bind(globalThis),
  setTimeout: globalThis.setTimeout.bind(globalThis),
  clearTimeout: globalThis.clearTimeout.bind(globalThis),
  requestAnimationFrame: (callback: FrameRequestCallback) => globalThis.setTimeout(() => callback(Date.now()), 0),
  cancelAnimationFrame: (handle: number) => globalThis.clearTimeout(handle),
  // Components attach global listeners for menus/overlays and shortcuts.
  addEventListener: windowEvents.addEventListener.bind(windowEvents),
  removeEventListener: windowEvents.removeEventListener.bind(windowEvents),
  dispatchEvent: windowEvents.dispatchEvent.bind(windowEvents),
  matchMedia: () => ({ matches: false, addEventListener: () => {}, removeEventListener: () => {} }),
  scrollTo: () => {},
};

(globalThis as unknown as { window: typeof windowStub }).window = windowStub;

// React 19 testing flag (used by the DOM interaction tests).
(globalThis as unknown as { IS_REACT_ACT_ENVIRONMENT: boolean }).IS_REACT_ACT_ENVIRONMENT = true;
