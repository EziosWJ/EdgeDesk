import { beforeEach, describe, expect, it, vi } from 'vitest';
import { preferencesStore } from './preferencesStore';

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json' },
  });
}

describe('preferencesStore', () => {
  beforeEach(() => {
    preferencesStore.reset();
    vi.unstubAllGlobals();
  });

  it('loads server settings and token state', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn(async () =>
        jsonResponse({
          ok: true,
          api_token_configured: true,
          settings: {
            theme: 'light',
            files_view: 'grid',
            files_show_hidden: false,
            terminal_font_size: 16,
            dashboard_metrics_window: 600,
          },
        }),
      ),
    );
    await preferencesStore.load();
    expect(preferencesStore.getSnapshot().theme).toBe('light');
    expect(preferencesStore.getSnapshot().files_view).toBe('grid');
    expect(preferencesStore.getTokenConfigured()).toBe(true);
  });

  it('optimistically saves and accepts the server snapshot', async () => {
    const fetchMock = vi.fn(async () =>
      jsonResponse({
        ok: true,
        settings: {
          theme: 'dark',
          files_view: 'list',
          files_show_hidden: true,
          terminal_font_size: 18,
          dashboard_metrics_window: 300,
        },
      }),
    );
    vi.stubGlobal('fetch', fetchMock);
    await preferencesStore.save({ terminal_font_size: 18 });
    expect(fetchMock).toHaveBeenCalledWith('/api/settings', expect.objectContaining({ method: 'PUT' }));
    expect(preferencesStore.getSnapshot().terminal_font_size).toBe(18);
  });

  it('keeps the optimistic value when the request fails', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => jsonResponse({ ok: false, error: 'io_error' }, 500)));
    await expect(preferencesStore.save({ theme: 'light' })).rejects.toBeTruthy();
    expect(preferencesStore.getSnapshot().theme).toBe('light');
  });
});
