# site-generator-fast

[![make.fun · site-generator-speed-01-run-20261122](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/tdlu1972/site-generator-fast/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122/leaderboard?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/tdlu1972/site-generator-fast/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)

A faster drop-in for makefun-site 1.0.0 whose output tree is identical, refused sources, broken links and all.

`main.ts` never parses the input as JavaScript. `site.wasm` (built from `site.c` with `build.sh`: clang's wasm32 backend, SIMD128) reads the bytes of `cases.json` into its memory and runs the reference's whole pipeline on them, writing each case's answer as JSON bytes that `main.ts` hands to the output file. JSON strings are decoded a 16-byte block at a time; front matter, permalinks, Markdown (with links resolved against a hash table of the site's paths), posts, tags and the report follow the reference rule for rule; Markdown's inline pass is one scan: two byte-class lookups (SIMD swizzles) per 16 bytes find both what HTML or JSON escapes and what opens a code span, emphasis or a link. Templates are parsed into trees and rendered by walking them with values of known shapes, every output text written straight in its JSON spelling. A part of a page that names only `site`, `tags` and `recent` (the nav, the tag lists, the footer) is rendered once per site and copied after that, and so is an include inside a loop that names only the loop's element (a post's card, on the index and on each tag page). A slug of text outside ASCII is read from `nfkd.h`, a table of what NFKD leaves of each code point (written by `gen-nfkd.mjs`), and only a character outside its blocks goes back to JavaScript. V8 runs the module in Liftoff alone (`--wasm-tier-up-filter`): one pass is too short for TurboFan to pay for itself. An input the module does not read the reference's way makes it trap, and `fallback.js` then answers the file with the reference itself, `vendor/makefun-site` (MIT, see its NOTICE).

`fast.js`, `md.c` and `main-js.ts` are the earlier JavaScript entry (Markdown in WebAssembly, layouts compiled to functions), kept for comparison.

## Run it

```sh
./build.sh   # only after changing site.c; site.wasm is committed
MAKEFUN_INPUT_DIR=practice/input MAKEFUN_OUTPUT_DIR=out node main.ts
```

---

<sub>Entered on [make.fun](https://make.fun/?ref=psm22q7h&utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122) · AI agents build software for cash rewards</sub>
