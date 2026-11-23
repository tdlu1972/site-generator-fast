// The program the harness runs (`node /bot/main.ts`, node22): reads every case from /input/cases.json and writes
// /output/answers.json, makefun-site 1.0.0's answer for each, in order. Outside a container MAKEFUN_INPUT_DIR and
// MAKEFUN_OUTPUT_DIR stand in for /input and /output.
//
// site.wasm (built from site.c by build.sh) reads the file's bytes into its own memory and hands back the answers file
// a case at a time, never decoding the input as JavaScript strings. Where it traps (an input outside what it reads the
// reference's way), the file is answered by the reference itself instead, by fallback.js.
//
// Built-in modules come from process.getBuiltinModule rather than import statements: an ES module with none starts
// about as fast as an empty one.

const fs = process.getBuiltinModule('node:fs');
const env = process.env;
const INPUT = (env.MAKEFUN_INPUT_DIR ?? '/input') + '/cases.json';
const OUTPUT = (env.MAKEFUN_OUTPUT_DIR ?? '/output') + '/answers.json';

// run() is called once, so a TurboFan tier-up of its hot functions costs more compile time than it saves: stay in Liftoff.
// --liftoff-only is read when V8 starts and is ignored here; a tier-up filter naming no function is read per module, and
// keeps every function in Liftoff.
process.getBuiltinModule('node:v8').setFlagsFromString('--wasm-tier-up-filter=99999');

/** makefun-site's slug of a heading, title or tag outside ASCII (NFKD, combining marks dropped). */
function slugify(text: string): string {
  const plain = text.normalize('NFKD').replace(/\p{M}/gu, '');
  let out = '';
  let gap = false;
  for (const ch of plain) {
    let keep = null;
    if (ch >= 'A' && ch <= 'Z') keep = ch.toLowerCase();
    else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) keep = ch;
    if (keep === null) {
      gap = out.length > 0;
      continue;
    }
    if (gap) out += '-';
    gap = false;
    out += keep;
  }
  out = out.slice(0, 64).replace(/-+$/, '');
  return out === '' ? 'untitled' : out;
}

let answered = false;
let out = -1;
try {
  let memory: WebAssembly.Memory;
  const DEC = new TextDecoder();
  const imports = {
    env: {
      flush(p: number, n: number) {
        fs.writeSync(out, new Uint8Array(memory.buffer, p, n));
      },
      uslug(p: number, n: number, at: number) {
        const u8 = new Uint8Array(memory.buffer);
        const slug = slugify(DEC.decode(u8.subarray(p, p + n)));
        for (let k = 0; k < slug.length; k++) u8[at + k] = slug.charCodeAt(k);
        return slug.length;
      },
    },
  };
  const wasm = new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(new URL('./site.wasm', import.meta.url))), imports).exports as any;
  memory = wasm.memory;
  const fd = fs.openSync(INPUT, 'r');
  const size = fs.fstatSync(fd).size;
  const at = wasm.input(size); // may grow memory, detaching any buffer taken before it
  const into = new Uint8Array(memory.buffer, at, size);
  let got = 0;
  while (got < size) {
    const k = fs.readSync(fd, into, got, size - got, null);
    if (k === 0) break;
    got += k;
  }
  fs.closeSync(fd);
  if (got === size) {
    out = fs.openSync(OUTPUT, 'w');
    wasm.run();
    fs.closeSync(out);
    answered = true;
  }
} catch {}

if (!answered) import(new URL('./fallback.js', import.meta.url).href);
