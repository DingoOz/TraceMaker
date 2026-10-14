// Headless Chromium (Playwright's headless shell, SwiftShader WebGL2) against a live `tracemaker route ... --si-rules --view --hold` (issue #24):
// the Critical nets panel after the route, and a click on a net that shows its return path in the overlay.
// Run: CHROME=<headless shell> URL=http://127.0.0.1:PORT/ OUT=<prefix> node e2e-critical-nets.mjs
import { chromium } from 'playwright-core';
const browser = await chromium.launch({ executablePath: process.env.CHROME, args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--no-sandbox'] });
const page = await browser.newPage({ viewport: { width: 1400, height: 800 } });
const logs = [];
page.on('console', (m) => logs.push(m.type() + ': ' + m.text()));
page.on('pageerror', (e) => logs.push('pageerror: ' + e.message));
await page.goto(process.env.URL);
const out = process.env.OUT;
const hasHeading = async () => (await page.locator('.label', { hasText: 'Critical nets' }).count()) > 0;
for (let i = 0; i < 60 && !(await hasHeading()); i++) await page.waitForTimeout(500);
console.log('Critical nets panel shown:', await hasHeading());
const rows = page.locator('.stall-row', { has: page.locator('.chip-verdict', { hasText: /^T[123]$/ }) });
console.log('rows:', await rows.count());
console.log('first rows:', (await rows.evaluateAll((els) => els.slice(0, 4).map((e) => e.textContent))).join(' | '));
await page.waitForTimeout(500);
await page.screenshot({ path: out + '-panel.png' });
const first = rows.first();
console.log('row tooltip:', await first.getAttribute('title'));
await first.click();
await page.waitForTimeout(900);
console.log('selected:', await first.getAttribute('class'));
await page.screenshot({ path: out + '-selected.png' });
console.log(logs.filter((l) => /error/i.test(l)).slice(0, 5).join('\n') || 'no console errors');
await browser.close();
