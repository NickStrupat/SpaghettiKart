// The browser build's service worker (docs/WEB.md, "Offline"): keeps a copy of every file the page loads, the archive
// maker's included, and serves the page from it, so the game starts without a network, ROM or no ROM. The player's
// mk64.o2r is not among them: the page keeps that in a database of its own.
//
// tools/web/make-sw.py writes this file next to the page with the list below filled in: each file and the SHA-256 of
// its contents. A new build changes the list and so this script, which is how the browser learns of a new version: it
// checks this script whenever the page opens and when the page asks (on returning to it). The new version's worker
// then saves the files that changed and waits; the page offers a restart, and only then does the new version take over.
const kFiles = {/* the list */};
const kCache = 'spaghettikart-files';
// The folder the page is in: this script's.
const kBase = new URL('./', self.location).href;

// Each version of a file is kept under its own name, so that two versions' workers can share the one cache.
const keyOf = (path) => `${kBase}${path}?sha256=${kFiles[path]}`;

async function sha256(bytes) {
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
  return Array.from(digest, (byte) => byte.toString(16).padStart(2, '0')).join('');
}

/** Saves every file of this version that the cache lacks, one at a time: the largest are 8 MB. */
async function saveFiles() {
  const cache = await caches.open(kCache);
  for (const path of Object.keys(kFiles)) {
    const key = keyOf(path);
    if (await cache.match(key)) {
      continue;
    }
    // Past the browser's own cache, which may hold an older version.
    const response = await fetch(kBase + path, { cache: 'no-cache' });
    if (!response.ok) {
      throw new Error(`${path}: HTTP ${response.status}`);
    }
    const bytes = await response.arrayBuffer();
    // A deploy between this script and the file: this version is not installed, and the next check tries again.
    if (await sha256(bytes) !== kFiles[path]) {
      throw new Error(`${path} on the server is not this version's`);
    }
    // The page uses threads, which need these two headers on everything it loads (docs/WEB.md).
    await cache.put(key, new Response(bytes, {
      headers: {
        'Content-Type': response.headers.get('Content-Type') || 'application/octet-stream',
        'Cross-Origin-Opener-Policy': 'same-origin',
        'Cross-Origin-Embedder-Policy': 'require-corp',
      },
    }));
  }
}

/** Removes every file that is not this version's. */
async function removeOtherVersions() {
  const cache = await caches.open(kCache);
  const keep = new Set(Object.keys(kFiles).map(keyOf));
  for (const request of await cache.keys()) {
    if (!keep.has(request.url)) {
      await cache.delete(request);
    }
  }
}

self.addEventListener('install', (event) => event.waitUntil(saveFiles()));
self.addEventListener('activate', (event) => event.waitUntil(removeOtherVersions()));
// The page's restart button.
self.addEventListener('message', (event) => {
  if (event.data === 'activate') {
    self.skipWaiting();
  }
});

self.addEventListener('fetch', (event) => {
  const url = new URL(event.request.url);
  if (event.request.method !== 'GET' || !url.href.startsWith(kBase)) {
    return;
  }
  // The address's query (the page's test settings) does not change the file.
  const path = url.pathname.slice(new URL(kBase).pathname.length) || 'index.html';
  if (!(path in kFiles)) {
    // mk64.o2r, which the server may have, and anything else: from the network.
    return;
  }
  event.respondWith(caches.open(kCache)
    .then((cache) => cache.match(keyOf(path)))
    .then((saved) => saved || fetch(event.request)));
});
