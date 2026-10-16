import type { MetricsSample } from '../types';

type SampleHandler = (sample: MetricsSample) => void;
type HistoryHandler = (samples: MetricsSample[], intervalMs: number) => void;

/**
 * Metrics WebSocket client with automatic reconnect and a fixed-size local
 * window (the server also caps its own history).
 */
export class MetricsSocket {
  private socket: WebSocket | null = null;
  private closed = false;
  private attempts = 0;
  private timer: number | null = null;

  onSample: SampleHandler = () => {};
  onHistory: HistoryHandler = () => {};

  connect(): void {
    if (this.socket || this.closed) return;
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const socket = new WebSocket(`${protocol}//${window.location.host}/ws/metrics`);
    this.socket = socket;
    socket.onmessage = (event) => {
      let message: Record<string, unknown>;
      try {
        message = JSON.parse(String(event.data));
      } catch {
        return;
      }
      if (message.type === 'snapshot') {
        const history = (message.history as MetricsSample[]) ?? [];
        const intervalMs = Number(message.interval_ms ?? 1000);
        this.attempts = 0;
        this.onHistory(history, intervalMs);
        if (message.sample) this.onSample(message.sample as MetricsSample);
      } else if (message.type === 'metrics') {
        this.onSample(message.sample as MetricsSample);
      }
    };
    socket.onclose = () => {
      this.socket = null;
      if (!this.closed) this.scheduleReconnect();
    };
  }

  private scheduleReconnect(): void {
    if (this.timer !== null) return;
    this.attempts += 1;
    const delay = Math.min(10000, 500 * 2 ** Math.min(this.attempts, 4));
    this.timer = window.setTimeout(() => {
      this.timer = null;
      this.connect();
    }, delay);
  }

  close(): void {
    this.closed = true;
    if (this.timer !== null) {
      window.clearTimeout(this.timer);
      this.timer = null;
    }
    this.socket?.close();
    this.socket = null;
  }
}
