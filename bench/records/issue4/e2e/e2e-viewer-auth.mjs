// Headless Chromium (Playwright's headless shell) against two live viewers (issue #4):
//  1. a viewer on 0.0.0.0 with a token: the page without the token is refused, with it the viewer connects (LIVE) through the cookie, and the cookie is HttpOnly;
//  2. a viewer on 127.0.0.1: a page from ANOTHER origin that opens its /ws gets nothing (the server refuses the foreign Origin), while the viewer's own page connects.
// Run: CHROME=<headless shell> TOKENURL=http://127.0.0.1:8797/?token=... OPENURL=http://127.0.0.1:8797/ LOOPBACK=http://127.0.0.1:8796/ PROBE=http://127.0.0.1:8798/probe.html OUT=<prefix> node e2e-viewer-auth.mjs
import { chromium } from 'playwright-core';
const browser = await chromium.launch({ executablePath: process.env.CHROME, args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--no-sandbox'] });
const out = process.env.OUT;
const live = async (page) => { for (let i = 0; i < 40; i++) { if ((await page.locator('body').textContent()).toLowerCase().includes('live')) return true; await page.waitForTimeout(250); } return false; };

let ctx = await browser.newContext({ viewport: { width: 1200, height: 700 } });
let page = await ctx.newPage();
const closed = await page.goto(process.env.OPENURL);
console.log('1a. page without the token: HTTP', closed.status(), '|', (await page.locator('h2').textContent()));
await page.screenshot({ path: out + '-no-token.png' });
const res = await page.goto(process.env.TOKENURL);
console.log('1b. page with the token: HTTP', res.status(), '| LIVE:', await live(page));
await page.screenshot({ path: out + '-with-token.png' });
const cookies = await ctx.cookies();
console.log('1c. cookie:', cookies.map((c) => `${c.name} httpOnly=${c.httpOnly} sameSite=${c.sameSite}`).join('; '));
await page.goto(process.env.OPENURL); // the cookie alone is enough from now on
console.log('1d. later visit without the token in the URL: LIVE:', await live(page));
await ctx.close();

ctx = await browser.newContext();
page = await ctx.newPage();
await page.goto(process.env.LOOPBACK);
console.log('2a. the loopback viewer\'s own page: LIVE:', await live(page));
const probe = await ctx.newPage();
await probe.goto(process.env.PROBE + '?ws=' + encodeURIComponent(process.env.LOOPBACK.replace('http', 'ws') + 'ws'));
await probe.waitForTimeout(2500);
console.log('2b. a page from another origin opening the same /ws:', (await probe.locator('#out').textContent()));
await ctx.close();
await browser.close();
