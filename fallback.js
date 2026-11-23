// The answers file the house way, for an input site.wasm traps on: every case through the reference itself,
// vendor/makefun-site (MIT, see its NOTICE), as the house baseline runs it. writeFileSync replaces whatever main.ts
// wrote before the trap.

import { readFileSync, writeFileSync } from 'node:fs';
import { setFlagsFromString } from 'node:v8';
import { build } from './vendor/makefun-site/index.js';

setFlagsFromString('--no-incremental-marking');

const INPUT = (process.env.MAKEFUN_INPUT_DIR ?? '/input') + '/cases.json';
const OUTPUT = (process.env.MAKEFUN_OUTPUT_DIR ?? '/output') + '/answers.json';
const cases = JSON.parse(readFileSync(INPUT, 'utf8'));
writeFileSync(OUTPUT, JSON.stringify(cases.map((c) => build(c.input))));
