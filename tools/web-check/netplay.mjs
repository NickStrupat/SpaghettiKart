// A netplay race between browser builds (docs/WEB.md), like a tools/netplay-sim.py scenario: a local relay and one
// headless Chrome per player, each its own process as two players' browsers would be. Bots drive; the host creates a
// room and the others join it once the host's room code exists.
//
//   python3 tools/web-serve.py &
//   node tools/web-check/netplay.mjs [options]
//
// Options, as environment variables:
//   PLAYERS=2       2 to 4
//   MODE=vs         vs, gp or battle
//   TRACK=mk:luigi_raceway   VS and Battle
//   SERIES=1        races before the players leave the series
//   SPEED=10        SK_FAST: draw one frame in this many
//   DIRECT=1        0 keeps every player on the relay (gNetplay direct connections off)
//   BLOCK_DIRECT_MS=<ms>   the last player's direct connections stop working this long after joining, as if a
//                   router closed them: it has to go on through the relay. A fast-forwarded race starts about 1 s
//                   after joining and lasts about 5 s: 3000 lands in the middle of it.
//   NATIVE=<game executable>   make one player a desktop game instead of a browser
//   NATIVE_PLAYER=<n>   which one (default: the last; 0 makes the desktop game the host)
//   PAGE=http://127.0.0.1:8064/   RELAY=build-relay/netplay-relay
//   CHROME_ARGS=... more command-line switches for each Chrome
//   VERBOSE=1       print every console line of the browsers as it comes, not only the netplay summary
// Prints each player's netplay log lines and exit code. Exits with 0 when every player finished in sync.
import { execFile, spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import puppeteer from 'puppeteer-core';

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const page = process.env.PAGE || 'http://127.0.0.1:8064/';
const players = Number(process.env.PLAYERS || 2);
const relayPath = process.env.RELAY || path.join(repo, 'build-relay/netplay-relay');
const native = process.env.NATIVE;
const nativePlayer = native ? Number(process.env.NATIVE_PLAYER ?? players - 1) : -1;
const kTimeoutMs = 600 * 1000;
const port = 20000 + Math.floor(Math.random() * 20000);

const relay = spawn(relayPath, ['--port', String(port), '--no-rate-limits'], { stdio: ['ignore', 'pipe', 'inherit'] });
let relayLog = '';
relay.stdout.on('data', (data) => { relayLog += data; });
await new Promise((resolve) => setTimeout(resolve, 300));

function settings(index, roomCode) {
  const values = {
    SK_FAST: process.env.SPEED || '10',
    SK_NETPLAY_RELAY: `127.0.0.1:${port}`,
    SK_NETPLAY_HOST: index === 0 ? '1' : '0',
    SK_NETPLAY_PLAYERS: String(players),
    SK_NETPLAY_RACES: '1',
    SK_NETPLAY_MODE: process.env.MODE || 'vs',
    SK_NETPLAY_BOT: '1',
    SK_NETPLAY_FRAMES: '0',
    SK_NETPLAY_TEST_SERIES_RACES: process.env.SERIES || '1',
  };
  if ((process.env.MODE || 'vs') !== 'gp') {
    values.SK_NETPLAY_TRACK = process.env.TRACK || 'mk:luigi_raceway';
  }
  if (index === 0) {
    values.SK_NETPLAY_ROOM_FILE = index === nativePlayer ? nativeRoomFile : '/tmp/room';
  } else {
    values.SK_NETPLAY_ROOM = roomCode;
  }
  return values;
}

const nativeRoomFile = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'web-netplay-room-')), 'room');
const results = [];
const browsers = [];

/** One player in its own headless Chrome. @returns { page, done } where done resolves to the exit code. */
async function startBrowserPlayer(index, roomCode) {
  const browser = await puppeteer.launch({
    executablePath: process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
    headless: true,
    // Chrome hides each browser's local addresses behind .local names, which two headless Chromes on one computer
    // cannot look up in each other: WebRTC would never connect. Players on different computers do not need this.
    args: ['--use-angle=metal', '--enable-gpu', '--ignore-gpu-blocklist', '--disable-features=WebRtcHideLocalIpsWithMdns',
      ...(process.env.CHROME_ARGS || '').split(' ').filter((arg) => arg !== '')],
    defaultViewport: { width: 400, height: 300 },
  });
  browsers.push(browser);
  const tab = await browser.newPage();
  const lines = [];
  results[index] = { lines, code: 'timeout' };
  const done = new Promise((resolve) => {
    tab.on('console', (message) => {
      const text = message.text();
      if (process.env.VERBOSE && !/Loaded (bank|sequence)|ImportTexture|null palette/.test(text)) {
        console.log(`p${index} ${new Date().toISOString().slice(11, 23)} ${message.type()}: ${text.slice(0, 300)}`);
      }
      if (text.startsWith('[Netplay]')) {
        lines.push(text);
      }
      const match = text.match(/\[Web\] Finished with exit code (-?\d+)/);
      if (match) {
        results[index].code = match[1];
        resolve();
      }
    });
    tab.on('pageerror', (error) => {
      lines.push(`PAGE ERROR: ${String(error).slice(0, 300)}`);
      results[index].code = 'crash';
      resolve();
    });
    setTimeout(resolve, kTimeoutMs);
  });
  const blockMs = process.env.DIRECT === '0' ? '1' : (index === players - 1 ? process.env.BLOCK_DIRECT_MS : '');
  const directOff = blockMs ? `&SK_NETPLAY_TEST_BLOCK_DIRECT_MS=${blockMs}` : '';
  await tab.goto(`${page}?${new URLSearchParams(settings(index, roomCode))}${directOff}`, { waitUntil: 'load' });
  return { tab, done };
}

/** The last player as a desktop game, with a throwaway settings folder. */
function startNativePlayer(index, roomCode) {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), 'web-netplay-'));
  fs.writeFileSync(path.join(home, 'spaghettify.cfg.json'), JSON.stringify({
    CVars: { gSkipIntro: 3, gVsyncEnabled: 0, gInterpolationFPS: 30 },
    Window: { Width: 400, Height: 300, PositionX: 40, PositionY: 60 },
  }));
  const o2r = process.env.O2R || path.join(os.homedir(), 'Library/Application Support/SpaghettiKart/mk64.o2r');
  fs.symlinkSync(o2r, path.join(home, 'mk64.o2r'));
  const env = { ...process.env, SHIP_HOME: home, ...settings(index, roomCode) };
  if (process.env.DIRECT === '0') {
    env.SK_NETPLAY_TEST_BLOCK_DIRECT_MS = '1';
  } else if ((index === players - 1) && process.env.BLOCK_DIRECT_MS) {
    env.SK_NETPLAY_TEST_BLOCK_DIRECT_MS = process.env.BLOCK_DIRECT_MS;
  }
  const lines = [];
  results[index] = { lines, code: 'timeout' };
  const done = new Promise((resolve) => {
    execFile(native, ['-ApplePersistenceIgnoreState', 'YES'], { env, timeout: kTimeoutMs, maxBuffer: 1 << 26 },
      (error, stdout) => {
        lines.push(...stdout.split('\n').filter((line) => line.startsWith('[Netplay]')));
        if (process.env.VERBOSE) {
          // With the time of each line, which the browsers' lines get as they come.
          for (const line of stdout.split('\n').filter((text) => text.includes('[NetLog.cpp'))) {
            console.log(`p${index} desktop: ${line.slice(0, 300)}`);
          }
        }
        results[index].code = error ? String(error.code ?? 'error') : '0';
        resolve();
      });
  });
  return { done };
}

const host = nativePlayer === 0 ? startNativePlayer(0) : await startBrowserPlayer(0);
let roomCode = '';
for (let waited = 0; !roomCode && waited < 30000; waited += 250) {
  await new Promise((resolve) => setTimeout(resolve, 250));
  if (nativePlayer === 0) {
    roomCode = fs.existsSync(nativeRoomFile) ? fs.readFileSync(nativeRoomFile, 'utf8').trim() : '';
    continue;
  }
  roomCode = await host.tab.evaluate(() => {
    try {
      return FS.readFile('/tmp/room', { encoding: 'utf8' }).trim();
    } catch (error) {
      return '';
    }
  });
}
const finished = [host.done];
if (!roomCode) {
  console.log('The host made no room.');
} else {
  for (let index = 1; index < players; index++) {
    const isNative = index === nativePlayer;
    finished.push((isNative ? startNativePlayer(index, roomCode) : (await startBrowserPlayer(index, roomCode))).done);
  }
}
await Promise.all(finished);
for (const browser of browsers) {
  await browser.close();
}
relay.kill();

const kInteresting = /WebRTC|Direct connection|Segment \d+ (started|ended)|DESYNC|desync|FAIL|finished|lost|error/i;
let ok = roomCode !== '';
results.forEach((result, index) => {
  const kind = index === nativePlayer ? 'desktop' : 'browser';
  console.log(`player ${index} (${kind}): exit ${result.code}`);
  for (const line of result.lines.filter((text) => kInteresting.test(text)).slice(0, 25)) {
    console.log(`  ${line}`);
  }
  ok &&= result.code === '0';
});
if (!ok) {
  console.log('relay log:\n' + relayLog.split('\n').slice(-15).join('\n'));
}
process.exit(ok ? 0 : 1);
