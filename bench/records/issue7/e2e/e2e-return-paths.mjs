// Headless Chromium (Playwright's headless shell, SwiftShader WebGL2) against `tracemaker return-paths BOARD --nets ... --view`:
// the Return paths list, the overlay, and a click on a net that selects it and zooms onto it. Run: CHROME=<headless shell> URL=http://127.0.0.1:PORT/ OUT=<prefix> node e2e-return-paths.mjs
import { chromium } from 'playwright-core';
const browser = await chromium.launch({ executablePath: process.env.CHROME, args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--no-sandbox'] });
const page = await browser.newPage({ viewport: { width: 1400, height: 800 } });
const logs = [];
page.on('console', (m) => logs.push(m.type() + ': ' + m.text()));
page.on('pageerror', (e) => logs.push('pageerror: ' + e.message));
await page.goto(process.env.URL);
const out = process.env.OUT;
for (let i = 0; i < 60 && (await page.locator('.stall-row').count()) === 0; i++) await page.waitForTimeout(500);
const rows = await page.locator('.stall-row').count();
console.log('rows in the Return paths list:', rows);
console.log('first rows:', (await page.locator('.stall-row').evaluateAll((els) => els.slice(0, 4).map((e) => e.textContent))).join(' | '));
await page.waitForTimeout(500);
await page.screenshot({ path: out + '-all.png' });
const want = page.locator('.stall-row', { hasText: 'TRG_SWDIO_NRF52' }).first();
console.log('row title:', await want.getAttribute('title'));
await want.click();
await page.waitForTimeout(800);
console.log('selected row classes:', await want.getAttribute('class'));
await page.screenshot({ path: out + '-selected.png' });
await page.keyboard.press('p');
await page.waitForTimeout(300);
await page.screenshot({ path: out + '-overlay-off.png' });
await page.keyboard.press('p');
await page.waitForTimeout(300);
console.log(logs.filter((l) => /error/i.test(l)).slice(0, 5).join('\n') || 'no console errors');
await browser.close();
