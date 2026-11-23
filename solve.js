// The contract's solve.js: run(input) is makefun-site 1.0.0's answer for one site. fast.js builds it as JSON text; a
// site it does not read the reference's way is answered by the reference, vendor/makefun-site (MIT, see its NOTICE).

import { buildJson } from './fast.js';
import { build } from './vendor/makefun-site/index.js';

export function run(input) {
  try {
    return JSON.parse(buildJson(input));
  } catch {
    return build(input);
  }
}
