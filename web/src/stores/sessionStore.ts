import { useSyncExternalStore } from 'react';
import type { MetricsSample, TerminalSessionInfo } from '../types';

type Listener = () => void;

/**
 * Server-state mirror for terminal sessions and the metrics history window.
 * UI layout state lives in windowStore; server state lives here.
 */
class SessionStore {
  private terminals: TerminalSessionInfo[] = [];
  private shell = '/bin/bash';
  private shellAvailable = true;
  private maxSessions = 8;
  private listeners = new Set<Listener>();

  private metrics: MetricsSample[] = [];
  private metricsInterval = 1000;
  private metricsListeners = new Set<Listener>();
  private metricsCapacity = 300;

  subscribe = (listener: Listener): (() => void) => {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  };

  getSnapshot = (): readonly TerminalSessionInfo[] => this.terminals;

  private emit(): void {
    this.terminals = [...this.terminals];
    this.listeners.forEach((listener) => listener());
  }

  setSessions(sessions: TerminalSessionInfo[]): void {
    this.terminals = sessions;
    this.emit();
  }

  setShellInfo(info: { shell: string; shellAvailable: boolean; maxSessions: number }): void {
    this.shell = info.shell;
    this.shellAvailable = info.shellAvailable;
    this.maxSessions = info.maxSessions;
  }

  upsert(session: TerminalSessionInfo): void {
    const index = this.terminals.findIndex((item) => item.id === session.id);
    if (index >= 0) {
      this.terminals[index] = session;
    } else {
      this.terminals.push(session);
    }
    this.emit();
  }

  remove(sessionId: string): void {
    this.terminals = this.terminals.filter((item) => item.id !== sessionId);
    this.emit();
  }

  get shellInfo() {
    return { shell: this.shell, available: this.shellAvailable, maxSessions: this.maxSessions };
  }

  // ---- metrics window ----

  metricsSubscribe = (listener: Listener): (() => void) => {
    this.metricsListeners.add(listener);
    return () => this.metricsListeners.delete(listener);
  };

  metricsSnapshot = (): readonly MetricsSample[] => this.metrics;

  setMetricsHistory(samples: MetricsSample[], intervalMs: number): void {
    this.metricsInterval = intervalMs;
    this.metrics = samples.slice(-this.metricsCapacity);
    this.metricsListeners.forEach((listener) => listener());
  }

  pushSample(sample: MetricsSample): void {
    this.metrics = [...this.metrics, sample].slice(-this.metricsCapacity);
    this.metricsListeners.forEach((listener) => listener());
  }

  get intervalMs(): number {
    return this.metricsInterval;
  }
}

export const sessionStore = new SessionStore();

export function useTerminals(): readonly TerminalSessionInfo[] {
  return useSyncExternalStore(sessionStore.subscribe, sessionStore.getSnapshot);
}

export function useMetricsSamples(): readonly MetricsSample[] {
  return useSyncExternalStore(sessionStore.metricsSubscribe, sessionStore.metricsSnapshot);
}
