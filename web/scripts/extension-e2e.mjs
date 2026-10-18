#!/usr/bin/env node
/**
 * Browser acceptance test for the V1.3 extension runtime and management UI.
 *
 * Prerequisites:
 *   - EdgeDesk dev-disk server running with --apps-root and web/dist
 *   - an installed/enabled sample app with storage/files/terminal permissions
 *     (tools/ext-sdk/templates/vanilla, packed with the SDK CLI)
 *   - EDGEDESK_SAMPLE_V11_ZIP pointing at a 1.1.0 build of the same app
 *
 * Environment:
 *   EDGEDESK_URL          default http://127.0.0.1:19191
 *   EDGEDESK_PASSWORD     required
 *   EDGEDESK_APP_ID       default dev.edgedesk.sample
 *   EDGEDESK_APP_NAME     default EdgeDesk 示例应用
 *   EDGEDESK_SAMPLE_V11_ZIP  optional; enables the upgrade-close assertion
 *   EXT_E2E_OUT           screenshot directory (default: os.tmpdir())
 *
 * Run from web/: node scripts/extension-e2e.mjs
 */
import { chromium } from '@playwright/test';
import os from 'node:os';
import path from 'node:path';

const base = process.env.EDGEDESK_URL ?? 'http://127.0.0.1:19191';
const password = process.env.EDGEDESK_PASSWORD;
const appId = process.env.EDGEDESK_APP_ID ?? 'dev.edgedesk.sample';
const appName = process.env.EDGEDESK_APP_NAME ?? 'EdgeDesk 示例应用';
const v11Zip = process.env.EDGEDESK_SAMPLE_V11_ZIP;
const out = process.env.EXT_E2E_OUT ?? os.tmpdir();

if (!password) {
  console.error('EDGEDESK_PASSWORD is required');
  process.exit(2);
}

const browser = await chromium.launch();
const context = await browser.newContext({ viewport: { width: 1440, height: 900 }, locale: 'zh-CN' });
const page = await context.newPage();
const consoleErrors = [];
const httpErrors = [];
page.on('console', (message) => {
  if (message.type() === 'error') consoleErrors.push(`console: ${message.text()}`);
});
page.on('pageerror', (error) => consoleErrors.push(`pageerror: ${error.message}`));
page.on('response', (response) => {
  if (response.status() >= 400) httpErrors.push(`${response.status()} ${response.url()}`);
});

const fail = (message) => {
  throw new Error(message);
};

await page.goto(base + '/');
await page.locator('input[type="password"]').fill(password);
await page.locator('button[type="submit"]').click();
await page.locator('[data-testid="desktop-shell"]').waitFor({ timeout: 15000 });

// Application manager.
await page.keyboard.press('Alt+KeyK');
await page.locator('.ed-launcher-search input').fill('应用管理');
await page.locator('.ed-launcher-app', { hasText: '应用管理' }).click();
await page.locator('.ed-ext-view').waitFor({ timeout: 15000 });
await page.locator('.ed-ext-item', { hasText: appName }).waitFor({ timeout: 10000 });
await page.screenshot({ path: path.join(out, 'manager-1440.png'), fullPage: true });
await page.locator('.ed-ext-item', { hasText: appName }).click();
await page.locator('.ed-ext-detail h3', { hasText: appName }).waitFor();
await page.screenshot({ path: path.join(out, 'manager-detail-1440.png'), fullPage: true });

// Launch the sandboxed app and exercise the bridge.
await page.keyboard.press('Alt+KeyK');
await page.locator('.ed-launcher-search input').fill('示例');
await page.locator('.ed-launcher-app', { hasText: appName }).click();
const iframeElement = page.locator('iframe.ed-extension-frame');
await iframeElement.waitFor({ timeout: 15000 });
const sandbox = await iframeElement.getAttribute('sandbox');
if (!sandbox || sandbox.includes('allow-same-origin')) fail(`unexpected sandbox: ${sandbox}`);
const frame = page.frameLocator('iframe.ed-extension-frame');
await frame.locator('h1', { hasText: '沙箱示例应用' }).waitFor({ timeout: 15000 });
await frame.locator('#status', { hasText: '已连接' }).waitFor({ timeout: 15000 });
const info = await frame.locator('#app-info').innerText();
const before = await frame.locator('#counter-value').innerText();
await frame.locator('#counter').click();
await page.waitForTimeout(300);
const after = await frame.locator('#counter-value').innerText();
if (before === after) fail(`counter did not change: ${before}`);
await frame.locator('#terminal-create').click();
await frame.locator('#terminal-output', { hasText: '终端已创建' }).waitFor({ timeout: 15000 });
await frame.locator('#terminal-input').fill('echo hello-e2e');
await frame.locator('#terminal-form button[type="submit"]').click();
await frame.locator('#terminal-output', { hasText: 'hello-e2e' }).waitFor({ timeout: 15000 });

let upgradedInfo = info;
if (v11Zip) {
  await page.locator('.ed-window[aria-label="应用管理"] .ed-window-title').click({ force: true });
  await page.locator('.ed-ext-item', { hasText: appName }).click();
  const versionSection = page.locator('.ed-ext-section', { hasText: '版本与修订' });
  await versionSection.locator('input[type="file"]').setInputFiles(v11Zip);
  await page.locator('.ed-ext-banner.ok', { hasText: '已升级' }).waitFor({ timeout: 20000 });
  await page.locator('iframe.ed-extension-frame').waitFor({ state: 'detached', timeout: 10000 });

  await page.keyboard.press('Alt+KeyK');
  await page.locator('.ed-launcher-search input').fill('示例');
  await page.locator('.ed-launcher-app', { hasText: appName }).click();
  await page.locator('iframe.ed-extension-frame').waitFor({ timeout: 15000 });
  const upgraded = page.frameLocator('iframe.ed-extension-frame');
  await upgraded.locator('#status', { hasText: '已连接' }).waitFor({ timeout: 15000 });
  upgradedInfo = await upgraded.locator('#app-info').innerText();
  if (!upgradedInfo.includes('1.1.0')) fail(`upgrade not active: ${upgradedInfo}`);

  // Disable closes the window and removes the launcher entry.
  await page.locator('.ed-window[aria-label="应用管理"] .ed-window-title').click({ force: true });
  await page.locator('.ed-ext-item', { hasText: appName }).click();
  await page.locator('.ed-ext-actions button', { hasText: '禁用' }).click();
  await page.locator('.ed-ext-banner.ok', { hasText: '已禁用' }).waitFor({ timeout: 15000 });
  await page.locator('iframe.ed-extension-frame').waitFor({ state: 'detached', timeout: 10000 });
  await page.keyboard.press('Alt+KeyK');
  await page.locator('.ed-launcher-search input').fill('示例');
  await page.waitForTimeout(300);
  if (await page.locator('.ed-launcher-app', { hasText: appName }).count()) {
    fail('disabled app is still in the launcher');
  }
  await page.keyboard.press('Escape');

  await page.locator('.ed-window[aria-label="应用管理"] .ed-window-title').click({ force: true });
  await page.locator('.ed-ext-item', { hasText: appName }).click();
  await page.locator('.ed-ext-actions button', { hasText: '启用' }).click();
  await page.locator('.ed-ext-banner.ok', { hasText: '已启用' }).waitFor({ timeout: 15000 });
}

// Reopen and verify desktop restore creates a fresh runtime instance.
await page.keyboard.press('Alt+KeyK');
await page.locator('.ed-launcher-search input').fill('示例');
await page.locator('.ed-launcher-app', { hasText: appName }).click();
await page.locator('iframe.ed-extension-frame').waitFor({ timeout: 15000 });
await page.frameLocator('iframe.ed-extension-frame').locator('#status', { hasText: '已连接' }).waitFor({ timeout: 15000 });
await page.reload();
await page.locator('[data-testid="desktop-shell"]').waitFor({ timeout: 15000 });
await page.locator('iframe.ed-extension-frame').waitFor({ timeout: 20000 });
await page.frameLocator('iframe.ed-extension-frame').locator('#status', { hasText: '已连接' }).waitFor({ timeout: 20000 });
await page.screenshot({ path: path.join(out, 'extension-window-1440.png'), fullPage: true });

await page.setViewportSize({ width: 390, height: 844 });
await page.waitForTimeout(500);
await page.screenshot({ path: path.join(out, 'extension-narrow-390.png'), fullPage: true });
const overflow = await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth);
if (overflow > 0) fail(`horizontal overflow: ${overflow}px`);

console.log(JSON.stringify({ sandbox, info, upgradedInfo, before, after, consoleErrors, httpErrors, overflow }));
if (consoleErrors.length || httpErrors.length) fail('browser errors detected');
await browser.close();
