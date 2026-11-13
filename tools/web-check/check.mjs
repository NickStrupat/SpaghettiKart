// Loads the browser build (docs/WEB.md) in a headless Chrome of its own, prints the page's console and runs a list
// of steps: for checking a change without touching anyone's open browser.
//
//   cd tools/web-check && npm install            # once: puppeteer-core, which drives an installed Chrome
//   python3 tools/web-serve.py &                 # the page to check
//   node tools/web-check/check.mjs http://127.0.0.1:8064/ wait:12000 key:Enter wait:2000 shot:/tmp/title.png
//
// Steps, run in order:
//   wait:<ms>          key:<Key>           press and let go (Enter is START, Shift is A, the arrows steer)
//   hold:<Key>         release:<Key>
//   shot:<file.png>    a picture of the page (not of the screen)
//   eval:<JavaScript>  run in the page and print the result; FS is the game's file system
//   pick:<mk64.o2r>    answer the page's request for the player's file, on a server that has none (the public one)
//   reload:
//   click:<selector>   click an element of the page
//   run:<command>      a shell command, such as stopping the server to check the page offline
//   viewport:<W>x<H>[@<scale>][,touch]   resize the page, as turning a phone does (390x844@3,touch is an iPhone)
// CHROME=<path> picks another Chrome than the one in /Applications. Without --use-angle=metal a headless Chrome draws
// with a software renderer, far too slowly for the game.
import { execSync } from 'node:child_process';
import puppeteer from 'puppeteer-core';

const [url, ...steps] = process.argv.slice(2);
if (!url) {
  console.error('usage: node check.mjs <url> [step...]');
  process.exit(2);
}
const kMaxConsoleLines = 300;
// Start-up lists every sound bank and sequence; nobody needs those.
const kNoise = /Loaded bank|Loaded sequence/;

const browser = await puppeteer.launch({
  executablePath: process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  headless: true,
  args: ['--use-angle=metal', '--enable-gpu', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required'],
  defaultViewport: { width: 960, height: 720 },
});
const page = await browser.newPage();
const start = Date.now();
const stamp = () => `[${((Date.now() - start) / 1000).toFixed(1).padStart(5)}]`;
let lines = 0;
page.on('console', (message) => {
  if (!kNoise.test(message.text()) && lines++ < kMaxConsoleLines) {
    console.log(`${stamp()} ${message.type()}: ${message.text().slice(0, 600)}`);
  }
});
page.on('pageerror', (error) => console.log(`${stamp()} PAGE ERROR: ${String(error.stack || error).slice(0, 1500)}`));
page.on('response', (response) => {
  if (response.status() >= 400) {
    console.log(`${stamp()} HTTP ${response.status()}: ${response.url()}`);
  }
});

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
let failed = false;
try {
  await page.goto(url, { waitUntil: 'load', timeout: 120000 });
  for (const step of steps) {
    const colon = step.indexOf(':');
    const [kind, value] = [step.slice(0, colon), step.slice(colon + 1)];
    if (kind === 'wait') {
      await sleep(Number(value));
    } else if (kind === 'key') {
      await page.keyboard.down(value);
      await sleep(120);
      await page.keyboard.up(value);
    } else if (kind === 'hold') {
      await page.keyboard.down(value);
    } else if (kind === 'release') {
      await page.keyboard.up(value);
    } else if (kind === 'shot') {
      await page.screenshot({ path: value });
    } else if (kind === 'eval') {
      console.log(`${stamp()} EVAL: ${JSON.stringify(await page.evaluate(value))}`);
    } else if (kind === 'pick') {
      const picker = await page.waitForSelector('#picker:not([hidden])', { timeout: 120000 });
      await picker.uploadFile(value);
    } else if (kind === 'click') {
      await page.click(value);
    } else if (kind === 'run') {
      execSync(value, { stdio: 'inherit' });
    } else if (kind === 'viewport') {
      const [, w, h, scale, touch] = value.match(/^(\d+)x(\d+)(?:@([\d.]+))?(,touch)?$/);
      await page.setViewport({ width: Number(w), height: Number(h), deviceScaleFactor: Number(scale || 1),
                               isMobile: !!touch, hasTouch: !!touch, isLandscape: Number(w) > Number(h) });
    } else if (kind === 'reload') {
      await page.reload({ waitUntil: 'load' });
    } else {
      throw new Error(`unknown step "${step}"`);
    }
  }
  // What the page shows over the game: empty while it runs, the reason once it has stopped.
  const status = await page.evaluate(() => document.getElementById('status')?.textContent ?? '');
  console.log(`${stamp()} STATUS: ${JSON.stringify(status)}`);
  failed = status !== '';
} catch (error) {
  console.log(`${stamp()} FAILED: ${error.message}`);
  failed = true;
}
await browser.close();
process.exit(failed ? 1 : 0);
