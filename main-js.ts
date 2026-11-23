// The program the harness runs (`node /bot/main.ts`, node22): reads every case from /input/cases.json and writes
// /output/answers.json, makefun-site 1.0.0's answer for each, in order. Outside a container MAKEFUN_INPUT_DIR and
// MAKEFUN_OUTPUT_DIR stand in for /input and /output.
//
// fast.js writes each answer straight to JSON text. A case it does not read the reference's way (it throws) is
// answered by the reference itself, vendor/makefun-site, as the house baseline answers it.

const fs = process.getBuiltinModule('node:fs');
const env = process.env;
const INPUT = (env.MAKEFUN_INPUT_DIR ?? '/input') + '/cases.json';
const OUTPUT = (env.MAKEFUN_OUTPUT_DIR ?? '/output') + '/answers.json';

// One pass over three sites is too short for TurboFan's compiles to pay for themselves: stay in Maglev.
const v8 = process.getBuiltinModule('node:v8');
v8.setFlagsFromString('--no-incremental-marking');
v8.setFlagsFromString('--no-turbofan');
for (const f of (env.SG_V8 ?? '').split(' ')) if (f) v8.setFlagsFromString(f);

const { buildJson } = (await import(new URL('./fast.js', import.meta.url).href)) as { buildJson: (input: unknown) => string };

const cases = JSON.parse(fs.readFileSync(INPUT, 'utf8')) as { id: string; input: any }[];
let out = '[';
for (let k = 0; k < cases.length; k++) {
  const input = cases[k].input;
  let json: string;
  try {
    json = buildJson(input);
  } catch {
    const { build } = (await import(new URL('./vendor/makefun-site/index.js', import.meta.url).href)) as { build: (input: unknown) => unknown };
    json = JSON.stringify(build(input));
  }
  out += (k === 0 ? '' : ',') + json;
}
fs.writeFileSync(OUTPUT, out + ']');
