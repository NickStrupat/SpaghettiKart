// Makes mk64.o2r from the player's ROM with the archive maker (tools/web/extractor), on a thread of its own so the
// page stays responsive. The page posts the ROM (big-endian, .z64 order) and gets back { archive } or { error }.
import createExtractor from './extractor.js';

self.onmessage = async ({ data: rom }) => {
  const output = [];
  try {
    const extractor = await createExtractor({
      print: (text) => output.push(text),
      printErr: (text) => output.push(text),
    });
    extractor.FS.writeFile('/rom.z64', rom);
    extractor.FS.mkdir('/out');
    // Torch's asset descriptions are in /torch (extractor.data).
    const code = extractor.callMain(['/rom.z64', '/torch', '/out']);
    if (code !== 0) {
      throw new Error(output.slice(-5).join('\n'));
    }
    const archive = extractor.FS.readFile('/out/mk64.o2r').slice();
    self.postMessage({ archive }, [archive.buffer]);
  } catch (error) {
    self.postMessage({ error: String(error.message || error) });
  }
};
