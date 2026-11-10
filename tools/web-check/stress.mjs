// Rollback stress run in the browser build: the races of tools/netplay-stress.sh, one determinism-harness race per
// track and arena, rolling back 8 frames after every frame and checking each re-simulated frame against the first run
// (SK_DETERMINISM_ROLLBACK, see src/port/netplay/DeterminismHarness.h).
//
//   python3 tools/web-serve.py &
//   node tools/web-check/stress.mjs [page address]      # default http://127.0.0.1:8064/
//
// Each race gets a fresh browser profile, so no settings or saves carry over. Options, as environment variables:
//   FRAMES=2000   race frames (30 per second)
//   JOBS=2        races at a time: each tab takes about 1.5 GB
//   ONLY=<text>   only races whose name contains it, such as "battle" or "rainbow"
//   NATIVE=<game executable>   also replay each browser race on a desktop build (SK_DETERMINISM_VERIFY), which
//                 checks that the two compute the same game frame by frame. Each replay gets a throwaway settings
//                 folder in the system's temporary folder.
//   O2R=<mk64.o2r>   for NATIVE (default: the macOS game's settings folder)
// Prints one line per race: its exit code (0 = in sync), seconds, and the first failure. Exits with 1 if any failed.
import { execFile } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import puppeteer from 'puppeteer-core';

const url = process.argv[2] || 'http://127.0.0.1:8064/';
const frames = Number(process.env.FRAMES || 2000);
const jobs = Number(process.env.JOBS || 2);
const kTimeoutMs = 900 * 1000;
const native = process.env.NATIVE;
const o2r = process.env.O2R || path.join(os.homedir(), 'Library/Application Support/SpaghettiKart/mk64.o2r');
const nativeDir = native ? fs.mkdtempSync(path.join(os.tmpdir(), 'web-stress-')) : null;
if (native) {
  console.log(`Native replays: ${nativeDir}`);
}

/** Replays a browser race's log on the desktop build. @returns '' when every frame matched, or why not. */
function replayNatively(name, log) {
  const home = path.join(nativeDir, `home-${name}`);
  fs.mkdirSync(home, { recursive: true });
  fs.writeFileSync(path.join(home, 'spaghettify.cfg.json'), JSON.stringify({
    CVars: { gSkipIntro: 3, gVsyncEnabled: 0, gInterpolationFPS: 30 },
    Window: { Width: 400, Height: 300, PositionX: 40, PositionY: 60 },
  }));
  fs.symlinkSync(o2r, path.join(home, 'mk64.o2r'));
  const logPath = path.join(nativeDir, `${name}.skdt`);
  fs.writeFileSync(logPath, log);
  return new Promise((resolve) => {
    const env = { ...process.env, SHIP_HOME: home, SK_FAST: '10', SK_DETERMINISM_VERIFY: logPath };
    execFile(native, ['-ApplePersistenceIgnoreState', 'YES'], { env, timeout: kTimeoutMs, maxBuffer: 1 << 26 },
      (error, stdout, stderr) => {
        fs.writeFileSync(path.join(nativeDir, `${name}.out`), stdout + stderr);
        const verdict = (stdout + stderr).split('\n').find((line) => line.includes('[Determinism]')) || '';
        resolve(error ? `native: ${verdict || error.message}` : '');
      });
  });
}

const races = [];
for (const track of ['luigi_raceway', 'moo_moo_farm', 'koopa_troopa_beach', 'kalimari_desert', 'toads_turnpike',
  'frappe_snowland', 'choco_mountain', 'mario_raceway', 'wario_stadium', 'sherbet_land', 'royal_raceway',
  'bowsers_castle', 'dk_jungle', 'yoshi_valley', 'banshee_boardwalk', 'rainbow_road']) {
  races.push({ mode: 'vs', track, players: 2 });
}
for (const arena of ['big_donut', 'block_fort', 'double_deck', 'skyscraper']) {
  races.push({ mode: 'battle', track: arena, players: 4 });
}
for (const track of ['luigi_raceway', 'frappe_snowland', 'royal_raceway', 'rainbow_road']) {
  races.push({ mode: 'gp', track, players: 2 });
}
const selected = races.filter((race) => !process.env.ONLY || `${race.mode}-${race.track}`.includes(process.env.ONLY));

const browser = await puppeteer.launch({
  executablePath: process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  headless: true,
  args: ['--use-angle=metal', '--enable-gpu', '--ignore-gpu-blocklist'],
  defaultViewport: { width: 400, height: 300 },
});

async function runRace(race) {
  const name = `${race.mode}-${race.track}`;
  const params = new URLSearchParams({
    SK_FAST: '10', SK_DETERMINISM_RECORD: `/tmp/${name}.skdt`, SK_DETERMINISM_FRAMES: String(frames),
    SK_DETERMINISM_MODE: race.mode, SK_DETERMINISM_TRACK: `mk:${race.track}`,
    SK_DETERMINISM_PLAYERS: String(race.players), SK_DETERMINISM_ROLLBACK: '8',
  });
  const context = await browser.createBrowserContext();
  const page = await context.newPage();
  const start = Date.now();
  let failure = '';
  let exitCode = 'timeout';
  const finished = new Promise((resolve) => {
    page.on('console', (message) => {
      const text = message.text();
      if (text.includes('[Determinism] FAIL') && failure === '') {
        failure = text.trim();
      }
      const match = text.match(/\[Web\] Finished with exit code (-?\d+)/);
      if (match) {
        exitCode = match[1];
        resolve();
      }
    });
    page.on('pageerror', (error) => {
      failure ||= `page error: ${String(error).slice(0, 200)}`;
      exitCode = 'crash';
      resolve();
    });
    setTimeout(resolve, kTimeoutMs);
  });
  await page.goto(`${url}?${params}`, { waitUntil: 'load' });
  await finished;
  if (native && exitCode === '0') {
    const log = await page.evaluate((file) => Array.from(FS.readFile(file)), `/tmp/${name}.skdt`);
    failure = await replayNatively(name, Buffer.from(log));
    if (failure) {
      exitCode = 'native';
    }
  }
  await context.close();
  const seconds = Math.round((Date.now() - start) / 1000);
  console.log(`${name.padEnd(26)} exit ${String(exitCode).padEnd(7)} ${String(seconds).padStart(4)}s  ${failure}`);
  return exitCode === '0';
}

const queue = [...selected];
let failed = 0;
await Promise.all(Array.from({ length: jobs }, async () => {
  while (queue.length > 0) {
    if (!(await runRace(queue.shift()))) {
      failed++;
    }
  }
}));
await browser.close();
console.log(`${selected.length - failed} of ${selected.length} races in sync.`);
process.exit(failed > 0 ? 1 : 0);
