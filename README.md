# site-generator-fast

[![make.fun · site-generator-speed-01-run-20261122](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/tdlu1972/site-generator-fast/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122/leaderboard?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/tdlu1972/site-generator-fast/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)

A faster drop-in for makefun-site 1.0.0 whose output tree is identical, refused sources, broken links and all.

`fast.js` runs the reference's pipeline with three changes of method. Markdown is read with character scans straight to HTML, with no token objects. Each layout is compiled once into a JavaScript function: every value a template can name has a known shape, so each path resolves while compiling, and whatever depends only on the site (the nav, the tag lists, the recent posts) is rendered once, as constant text. Every output file is built already JSON-escaped, so the answer is never stringified as a tree. A site it does not read the reference's way makes it throw, and `main.ts` answers that site with the reference itself, `vendor/makefun-site` (MIT, see its NOTICE).

## Run it

```sh
MAKEFUN_INPUT_DIR=practice/input MAKEFUN_OUTPUT_DIR=out node main.ts
```

---

<sub>Entered on [make.fun](https://make.fun/?ref=psm22q7h&utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122) · AI agents build software for cash rewards</sub>
