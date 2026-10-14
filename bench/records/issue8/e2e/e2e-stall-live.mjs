import { chromium } from 'playwright-core';
const browser = await chromium.launch({ executablePath: process.env.CHROME, args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--no-sandbox'] });
const page = await browser.newPage({ viewport: { width: 1400, height: 800 } });
const logs = [];
page.on('console', (m) => logs.push(m.type() + ': ' + m.text()));
page.on('pageerror', (e) => logs.push('pageerror: ' + e.message));
await page.goto(process.env.URL);
const out = process.env.OUT;
let latched = false;
for (let i = 0; i < 80 && !latched; i++) {
  await page.waitForTimeout(500);
  latched = !(await page.locator('.stall-banner').getAttribute('class')).includes('hidden');
}
console.log('latched:', latched);
if (latched) {
  console.log('banner:', await page.locator('.stall-banner').textContent());
  console.log('rows:', await page.locator('.stall-row').count());
  console.log('first verdict chips:', (await page.locator('.chip-verdict').allTextContents()).slice(0, 6).join(','));
  await page.screenshot({ path: out + '-live-latched.png' });
  await page.keyboard.press(']');
  await page.waitForTimeout(500);
  console.log('after ], selected row:', await page.locator('.stall-row.on .stall-net').textContent());
  await page.screenshot({ path: out + '-live-next.png' });
  const tracksBefore = await page.evaluate(() => document.querySelector('.stall-label')?.textContent);
  console.log('label:', tracksBefore);
  await page.keyboard.press('Space');
  await page.waitForTimeout(1000);
  console.log('resumed, banner class:', await page.locator('.stall-banner').getAttribute('class'));
  await page.screenshot({ path: out + '-live-resumed.png' });
}
console.log(logs.filter((l) => /error/i.test(l)).slice(0, 5).join('\n') || 'no console errors');
await browser.close();
