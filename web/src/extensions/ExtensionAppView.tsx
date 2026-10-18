import { useEffect, useRef, useState } from 'react';
import { RotateCw, ShieldAlert } from 'lucide-react';
import { Icon } from '../components/Icon';
import { ApiError } from '../services/api';
import { extensionApi } from './extensionApi';
import { getInstalledExtension } from './extensionRegistry';
import { HostBridge } from './hostBridge';
import type { ExtensionSession } from './extensionTypes';

type Props = {
  appId: string;
  windowId: string;
};

type Phase = 'starting' | 'running' | 'error';

function instanceId(): string {
  if (typeof crypto !== 'undefined' && typeof crypto.randomUUID === 'function') return crypto.randomUUID();
  const bytes = new Uint8Array(16);
  crypto.getRandomValues(bytes);
  return [...bytes].map((value) => value.toString(16).padStart(2, '0')).join('');
}

/**
 * Sandboxed extension window. The iframe never receives allow-same-origin; all
 * privileged calls go through a dedicated MessagePort and the host bridge.
 */
export function ExtensionAppView({ appId, windowId }: Props) {
  const [phase, setPhase] = useState<Phase>('starting');
  const [error, setError] = useState('');
  const [appError, setAppError] = useState('');
  const [session, setSession] = useState<ExtensionSession | null>(null);
  const [attempt, setAttempt] = useState(0);
  const iframeRef = useRef<HTMLIFrameElement | null>(null);
  const bridgeRef = useRef<HostBridge | null>(null);
  const sessionRef = useRef<ExtensionSession | null>(null);
  const meta = getInstalledExtension(appId);

  useEffect(() => {
    let cancelled = false;
    setPhase('starting');
    setError('');
    setAppError('');
    setSession(null);
    sessionRef.current = null;
    const created = instanceId();
    extensionApi
      .createSession(appId, created)
      .then((next) => {
        if (cancelled) {
          void extensionApi.closeSession(next.token);
          return;
        }
        sessionRef.current = next;
        setSession(next);
      })
      .catch((caught) => {
        if (cancelled) return;
        const message =
          caught instanceof ApiError ? caught.message : '无法创建应用运行会话，请稍后重试。';
        setError(message);
        setPhase('error');
      });
    return () => {
      cancelled = true;
      const current = sessionRef.current;
      const bridge = bridgeRef.current;
      bridgeRef.current = null;
      // Removing the runtime session is what terminates PTY sessions created
      // by this instance, including after an upgrade/disable invalidated it.
      bridge?.dispose();
      if (current) void extensionApi.closeSession(current.token);
      sessionRef.current = null;
    };
  }, [appId, attempt]);

  useEffect(() => {
    if (!session || !iframeRef.current) return;
    const bridge = new HostBridge({
      iframe: iframeRef.current,
      runtime: session,
      app: {
        appId: session.app_id,
        name: session.display_name,
        instanceId: session.instance_id,
        revisionId: session.revision_id,
        version: session.version,
        staticBase: session.static_base,
        iconUrl: meta?.icon_url,
      },
      onReady: () => {
        if (!bridgeRef.current) return;
        setPhase('running');
      },
      onAppError: (type, message) => {
        setAppError(`${type}: ${message}`.slice(0, 300));
      },
      onFatal: (message) => {
        setError(message);
        setPhase('error');
        bridge.dispose();
      },
    });
    bridgeRef.current = bridge;
    bridge.start();
    return () => {
      // The session cleanup effect above owns the backend session.
      bridge.dispose();
      if (bridgeRef.current === bridge) bridgeRef.current = null;
    };
  }, [session, meta?.icon_url]);

  const reload = () => {
    const bridge = bridgeRef.current;
    bridgeRef.current = null;
    bridge?.dispose();
    setAppError('');
    setAttempt((value) => value + 1);
  };

  return (
    <div className="ed-extension-surface" data-window-id={windowId} data-app-id={appId}>
      {session && phase !== 'error' && (
        <iframe
          ref={iframeRef}
          className="ed-extension-frame"
          title={`${meta?.display_name ?? appId} 扩展应用`}
          src={session.static_base}
          sandbox="allow-scripts allow-forms allow-downloads allow-popups allow-popups-to-escape-sandbox allow-modals"
          referrerPolicy="no-referrer"
        />
      )}
      {appError && phase === 'running' && (
        <div className="ed-extension-error-banner" role="status">
          <span>应用报告错误：{appError}</span>
          <button type="button" className="ed-link" onClick={reload}>重新加载</button>
          <button type="button" className="ed-link" onClick={() => setAppError('')}>关闭</button>
        </div>
      )}
      {phase === 'starting' && (
        <div className="ed-extension-overlay" role="status">
          <span className="ed-spinner" aria-hidden="true" />
          <p>正在启动 {meta?.display_name ?? appId}…</p>
          <small>沙箱运行时正在完成握手与权限快照。</small>
        </div>
      )}
      {phase === 'error' && (
        <div className="ed-extension-overlay error" role="alert">
          <Icon icon={ShieldAlert} size={28} />
          <strong>应用运行失败</strong>
          <p>{error || '应用未能在沙箱中启动。'}</p>
          <button type="button" className="ed-button primary" onClick={reload}>
            <Icon icon={RotateCw} size={14} />
            重新加载
          </button>
        </div>
      )}
    </div>
  );
}
