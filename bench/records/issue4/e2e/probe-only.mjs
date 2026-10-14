import { chromium } from 'playwright-core';
const browser = await chromium.launch({ executablePath: process.env.CHROME, args: ['--no-sandbox'] });
const page = await browser.newPage();
await page.goto(process.env.PROBE + '?ws=' + encodeURIComponent(process.env.WS));
await page.waitForTimeout(2500);
console.log(process.env.LABEL, '->', await page.locator('#out').textContent());
await browser.close();
