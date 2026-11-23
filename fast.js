// The fast build: buildJson(input) returns makefun-site 1.0.0's answer for one site as JSON text (an object of path ->
// text), the same value vendor/makefun-site's build(input) returns, written straight to text.
//
// It runs the same pipeline as the reference (README.md there states every rule), with three changes of method:
// - Markdown is read with character scans in place of regular expressions, straight to HTML (no token objects).
// - Templates are compiled once per layout into a JavaScript function. Every value a template can name has a shape
//   known ahead (site, tags, recent, page, posts, pager, tag), so each path resolves while compiling, and whatever
//   depends only on the site (site, tags, recent: the nav, the tag lists) is rendered then, once, as constant text.
// - Every output text is built already JSON-escaped, so the answer never goes through JSON.stringify as a tree.
//
// It throws Unsupported on input outside what it reads the reference's way (the caller then runs the reference).

export class Unsupported extends Error {}

const byCodeUnits = (a, b) => (a < b ? -1 : a > b ? 1 : 0);

// ---------------------------------------------------------------- text

const HTML_NEED = /[&<>"']/;
/** & < > " ' escaped, nothing else. */
function esc(s) {
  if (!HTML_NEED.test(s)) return s;
  let out = '';
  let run = 0;
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    let r;
    if (c === 38) r = '&amp;';
    else if (c === 60) r = '&lt;';
    else if (c === 62) r = '&gt;';
    else if (c === 34) r = '&quot;';
    else if (c === 39) r = '&#39;';
    else continue;
    out += s.slice(run, i) + r;
    run = i + 1;
  }
  return out + s.slice(run);
}

const JSON_NEED = /["\\\u0000-\u001f\ud800-\udfff]/;
/** `s` as the inside of a JSON string. */
function J(s) {
  return JSON_NEED.test(s) ? JSON.stringify(s).slice(1, -1) : s;
}
/** `s` HTML-escaped, as the inside of a JSON string. */
const JH = (s) => J(esc(s));
/** An array of objects as a template writes it: its items' text ("") joined by ", ". */
const SEP = (n) => (n > 1 ? ', '.repeat(n - 1) : '');

const ASCII = /^[\x00-\x7f]*$/;
/** makefun-site's slug of `text`. */
function slugify(text) {
  const plain = ASCII.test(text) ? text : text.normalize('NFKD').replace(/\p{M}/gu, '');
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

function uniqueSlug(slug, taken) {
  let out = slug;
  for (let n = 2; taken.has(out); n++) out = `${slug}-${n}`;
  taken.add(out);
  return out;
}

const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];
function longDate(date) {
  const [y, m, d] = date.split('-');
  return `${Number(d)} ${MONTHS[Number(m) - 1]} ${y}`;
}

/** Whether char code `c` is whitespace as a JavaScript regular expression's \s reads it. */
function isWS(c) {
  if (c <= 32) return c === 32 || (c >= 9 && c <= 13);
  if (c < 160) return false;
  return c === 160 || c === 5760 || (c >= 8192 && c <= 8202) || c === 8232 || c === 8233 || c === 8239 || c === 8287 || c === 12288 || c === 65279;
}

/** The body's words: runs of characters other than whitespace. */
function wordsOf(s) {
  let n = 0;
  let inWord = false;
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    const ws = c <= 32 ? c === 32 || (c >= 9 && c <= 13) : c >= 160 && isWS(c);
    if (ws) inWord = false;
    else if (!inWord) {
      inWord = true;
      n++;
    }
  }
  return n;
}

// ---------------------------------------------------------------- front matter (the reference's, line by line)

const KEYS = ['title', 'date', 'tags', 'draft', 'layout', 'slug', 'summary'];
const LINE = /^([a-z_]+):(?: (.*))?$/;
const DATE = /^(\d{4})-(\d{2})-(\d{2})$/;
const NAME = /^[a-z0-9-]+$/;
const SLUG = /^[a-z0-9]+(?:-[a-z0-9]+)*$/;
const TAG = /^[^"[\]]+$/;
const BLANKLINE = /^[ \t]*$/;

function daysIn(y, m) {
  if (m === 2) return (y % 4 === 0 && y % 100 !== 0) || y % 400 === 0 ? 29 : 28;
  return [4, 6, 9, 11].includes(m) ? 30 : 31;
}

function stringOf(raw) {
  if (raw.startsWith('"')) {
    if (raw.length < 2 || !raw.endsWith('"')) return { error: 'a quoted string is not closed' };
    let out = '';
    for (let i = 1; i < raw.length - 1; i++) {
      const c = raw[i];
      if (c === '\\') {
        const n = raw[i + 1];
        if ((n !== '"' && n !== '\\') || i + 1 === raw.length - 1) return { error: 'a quoted string holds an escape other than \\" or \\\\' };
        out += n;
        i++;
      } else if (c === '"') return { error: 'a quoted string holds a stray quote' };
      else out += c;
    }
    return { value: out };
  }
  if (raw.startsWith('[')) return { error: 'a bare string opens on [' };
  return { value: raw };
}

function valueOf(key, raw) {
  switch (key) {
    case 'title':
    case 'summary': {
      const s = stringOf(raw);
      return 'error' in s ? { error: `key "${key}": ${s.error}` } : s;
    }
    case 'date': {
      const m = DATE.exec(raw);
      const y = m ? Number(m[1]) : 0;
      const mo = m ? Number(m[2]) : 0;
      const d = m ? Number(m[3]) : 0;
      if (!m || y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > daysIn(y, mo)) return { error: 'key "date": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)' };
      return { value: raw };
    }
    case 'tags': {
      if (!raw.startsWith('[') || !raw.endsWith(']')) return { error: 'key "tags": not a list [a, b]' };
      const inner = raw.slice(1, -1);
      if (inner.trim() === '') return { value: [] };
      const tags = inner.split(',').map((t) => t.trim());
      if (tags.some((t) => t === '' || !TAG.test(t))) return { error: 'key "tags": an empty tag, or one holding " [ or ]' };
      return { value: tags };
    }
    case 'draft':
      return raw === 'true' ? { value: true } : raw === 'false' ? { value: false } : { error: 'key "draft": true or false' };
    case 'layout':
      return NAME.test(raw) ? { value: raw } : { error: 'key "layout": not a template name' };
    case 'slug':
      return SLUG.test(raw) && raw.length <= 64 ? { value: raw } : { error: 'key "slug": not a slug' };
    default:
      return { error: `unknown key "${key}"` };
  }
}

/** { data, body } or { error }, as the reference's parseFrontMatter, reading only up to the closing line. */
function parseFrontMatter(text) {
  let nl = text.indexOf('\n');
  if ((nl === -1 ? text : text.slice(0, nl)) !== '---') return { error: 'no front matter' };
  const lines = [];
  let close = -1;
  let bodyAt = -1;
  let at = nl + 1;
  while (nl !== -1) {
    nl = text.indexOf('\n', at);
    const line = nl === -1 ? text.slice(at) : text.slice(at, nl);
    if (line === '---') {
      close = lines.length + 1;
      bodyAt = nl === -1 ? text.length : nl + 1;
      break;
    }
    lines.push(line);
    at = nl + 1;
  }
  if (close === -1) return { error: 'the front matter is not closed' };
  const data = {};
  for (let k = 0; k < lines.length; k++) {
    const line = lines[k];
    if (BLANKLINE.test(line) || line.startsWith('#')) continue;
    const m = LINE.exec(line);
    if (!m) return { error: `line ${k + 2} is not "key: value"` };
    const key = m[1];
    const raw = (m[2] ?? '').replace(/[ \t]+$/, '');
    if (!KEYS.includes(key)) return { error: `unknown key "${key}"` };
    if (Object.hasOwn(data, key)) return { error: `key "${key}" is given twice` };
    if (raw === '') return { error: `key "${key}" has no value` };
    const v = valueOf(key, raw);
    if ('error' in v) return v;
    data[key] = v.value;
  }
  if (!Object.hasOwn(data, 'title')) return { error: 'no title' };
  return { data, body: text.slice(bodyAt) };
}

// ---------------------------------------------------------------- Markdown: md.wasm (md.c)

const DEC = new TextDecoder();
const ENC = new TextEncoder();
const md = new WebAssembly.Instance(new WebAssembly.Module(process.getBuiltinModule('node:fs').readFileSync(new URL('./md.wasm', import.meta.url))), {
  env: {
    // The slug of a heading's text outside ASCII, written at `out`.
    uslug(p, n, out) {
      const u8 = new Uint8Array(md.memory.buffer);
      const slug = slugify(DEC.decode(u8.subarray(p, p + n)));
      for (let k = 0; k < slug.length; k++) u8[out + k] = slug.charCodeAt(k);
      return slug.length;
    },
  },
}).exports;

/** The text of a JSON string's inside. */
const UNJ = (s) => JSON.parse(`"${s}"`);

// ---------------------------------------------------------------- templates: the reference's parser

const INCLUDE_DEPTH = 8;
const PATH = /^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$/;
const FOR = /^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$/;
const IF = /^if (not )?(\S+)$/;
const INCLUDE = /^include "([a-z0-9-]+)"$/;

class TemplateError extends Error {}

function pathOf(name, text) {
  if (!PATH.test(text)) throw new TemplateError(`template "${name}": ${JSON.stringify(text)} is not a path`);
  return text.split('.');
}

function tokenize(name, src) {
  const tokens = [];
  let text = '';
  let i = 0;
  while (i < src.length) {
    const open = src.indexOf('{', i);
    if (open === -1 || open === src.length - 1) {
      text += src.slice(i);
      break;
    }
    const next = src[open + 1];
    if (next !== '{' && next !== '%') {
      text += src.slice(i, open + 1);
      i = open + 1;
      continue;
    }
    text += src.slice(i, open);
    if (next === '{') {
      const raw = src[open + 2] === '{';
      const close = src.indexOf(raw ? '}}}' : '}}', open + (raw ? 3 : 2));
      if (close === -1) throw new TemplateError(`template "${name}": a ${raw ? '{{{' : '{{'} is not closed`);
      if (text !== '') tokens.push({ t: 'text', text });
      text = '';
      tokens.push({ t: raw ? 'raw' : 'var', path: pathOf(name, src.slice(open + (raw ? 3 : 2), close).trim()) });
      i = close + (raw ? 3 : 2);
      continue;
    }
    const close = src.indexOf('%}', open + 2);
    if (close === -1) throw new TemplateError(`template "${name}": a {% is not closed`);
    const end = close + 2;
    const lineStart = src.lastIndexOf('\n', open - 1) + 1;
    const nl = src.indexOf('\n', end);
    const lineEnd = nl === -1 ? src.length : nl;
    const standalone = /^[ \t]*$/.test(src.slice(lineStart, open)) && /^[ \t]*$/.test(src.slice(end, lineEnd));
    if (standalone) text = text.slice(0, text.length - (open - lineStart));
    if (text !== '') tokens.push({ t: 'text', text });
    text = '';
    tokens.push({ t: 'tag', tag: src.slice(open + 2, close).trim() });
    i = standalone ? (nl === -1 ? src.length : nl + 1) : end;
  }
  if (text !== '') tokens.push({ t: 'text', text });
  return tokens;
}

function parseTemplate(name, src) {
  const root = { kind: 'root', body: [] };
  const stack = [root];
  let body = root.body;
  for (const tok of tokenize(name, src)) {
    if (tok.t === 'text') body.push({ kind: 'text', text: tok.text });
    else if (tok.t === 'var' || tok.t === 'raw') body.push({ kind: tok.t, path: tok.path });
    else {
      const tag = tok.tag;
      const top = stack[stack.length - 1];
      let m;
      if ((m = FOR.exec(tag)) !== null) {
        const node = { kind: 'for', name: m[1], path: pathOf(name, m[2]), body: [] };
        body.push(node);
        stack.push(node);
        body = node.body;
      } else if ((m = IF.exec(tag)) !== null) {
        const node = { kind: 'if', not: m[1] !== undefined, path: pathOf(name, m[2]), then: [], else: null };
        body.push(node);
        stack.push(node);
        body = node.then;
      } else if (tag === 'else') {
        if (top.kind !== 'if' || top.else !== null) throw new TemplateError(`template "${name}": an else outside an if`);
        top.else = [];
        body = top.else;
      } else if (tag === 'endfor' || tag === 'endif') {
        if (top.kind !== (tag === 'endfor' ? 'for' : 'if')) throw new TemplateError(`template "${name}": an ${tag} with no open ${tag.slice(3)}`);
        stack.pop();
        const parent = stack[stack.length - 1];
        body = parent.kind === 'root' ? parent.body : parent.kind === 'for' ? parent.body : parent.else ?? parent.then;
      } else if ((m = INCLUDE.exec(tag)) !== null) body.push({ kind: 'include', name: m[1] });
      else throw new TemplateError(`template "${name}": {% ${tag} %} is not a tag`);
    }
  }
  if (stack.length > 1) throw new TemplateError(`template "${name}": a ${stack[stack.length - 1].kind} is not closed`);
  return root.body;
}

// The reference's value rules, for values known while compiling.
function lookupConst(v, path, k) {
  for (; k < path.length; k++) {
    if (v === null || typeof v !== 'object' || !Object.hasOwn(v, path[k])) return undefined;
    v = v[path[k]];
  }
  return v;
}
function textOf(v) {
  if (typeof v === 'string') return v;
  if (typeof v === 'number') return String(v);
  if (typeof v === 'boolean') return v ? 'true' : 'false';
  if (Array.isArray(v)) return v.map(textOf).join(', ');
  return '';
}
function truthy(v) {
  if (Array.isArray(v)) return v.length > 0;
  return v !== undefined && v !== null && v !== false && v !== '' && v !== 0;
}

// ---------------------------------------------------------------- templates: the compiler
//
// A value while compiling is { c: true, v } (known: its own value) or { c: false, t, e, ... } (known at run time, the
// JavaScript expression e, of shape t). Shapes: 'str' (with hj, j: expressions of its HTML+JSON-escaped and
// JSON-escaped text, where the record holds them), 'num', 'bool', { arr: shape }, { obj: fields }, { opt: fields }
// (an object or null), { loop } (a for's loop record). fields(name, e) is the field's value or undefined.

const str = (f) => (e) => ({ c: false, t: 'str', e: `${e}.${f}`, hj: `${e}.${f}H`, j: `${e}.${f}J` });
const strJ = (f) => (e) => ({ c: false, t: 'str', e: `UNJ(${e}.${f}J)`, j: `${e}.${f}J` });
const num = (f) => (e) => ({ c: false, t: 'num', e: `${e}.${f}` });
const UNDEF = { c: true, v: undefined };
const shape = (table) => (name, e) => (Object.hasOwn(table, name) ? table[name](e) : UNDEF);

const TAGREF = shape({ name: str('name'), slug: str('slug'), url: str('url') });
const LINK = shape({ title: str('title'), url: str('url'), date: str('date'), date_long: str('dateLong') });
const TOC = shape({ level: num('level'), id: str('id'), html: strJ('html') });
const cardFields = {
  title: str('title'),
  url: str('url'),
  date: str('date'),
  date_long: str('dateLong'),
  summary: str('summary'),
  excerpt: strJ('excerpt'),
  reading_minutes: num('reading'),
  tags: (e) => ({ c: false, t: { arr: { obj: TAGREF } }, e: `${e}.tagRefs` }),
};
const CARD = shape(cardFields);
const PAGE = shape({
  ...cardFields,
  slug: str('slug'),
  content: strJ('html'),
  toc: (e) => ({ c: false, t: { arr: { obj: TOC } }, e: `${e}.toc` }),
  newer: (e) => ({ c: false, t: { opt: LINK }, e: `${e}.newer` }),
  older: (e) => ({ c: false, t: { opt: LINK }, e: `${e}.older` }),
  is_post: (e) => ({ c: false, t: 'bool', e: `${e}.post` }),
});
const PAGER = shape({ number: num('number'), total: num('total'), url: str('url'), newer_url: str('newerUrl'), older_url: str('olderUrl') });
const TAGPAGE = shape({ name: str('name'), slug: str('slug'), url: str('url'), count: num('count'), posts: (e) => ({ c: false, t: { arr: { obj: CARD } }, e: `${e}.posts` }) });

/** The top-level names each kind of render sees beside site, tags and recent. */
const KINDS = {
  page: { page: { c: false, t: { obj: PAGE }, e: 'P' } },
  index: { posts: { c: false, t: { arr: { obj: CARD } }, e: 'Q' }, pager: { c: false, t: { obj: PAGER }, e: 'R' } },
  tag: { tag: { c: false, t: { obj: TAGPAGE }, e: 'T' } },
  tags: {},
};

const CODE_LIMIT = 4_000_000;

class Compiler {
  constructor(templates, globals) {
    this.templates = templates;
    this.globals = globals;
    this.K = [];
    this.vars = 0;
    this.size = 0;
  }

  /** The function rendering `name` for `kind`, or null when there is no such template. */
  compile(name, kind) {
    const nodes = this.templates.get(name);
    if (nodes === undefined) return null;
    const top = new Map(Object.entries(this.globals).map(([k, v]) => [k, { c: true, v }]));
    for (const [k, v] of Object.entries(KINDS[kind])) top.set(k, v);
    this.code = '';
    this.buf = '';
    this.nodes(nodes, [top], 0);
    this.flush();
    const body = `return function(P,Q,R,T){let o="";${this.code}return o}`;
    return new Function('K', 'J', 'JH', 'SEP', 'UNJ', body)(this.K, J, JH, SEP, UNJ);
  }

  /** Text already JSON-escaped. */
  lit(s) {
    this.buf += s;
  }

  flush() {
    if (this.buf === '') return;
    if (this.buf.length <= 48) this.emit(`o+=${JSON.stringify(this.buf)};`);
    else {
      this.K.push(this.buf);
      this.emit(`o+=K[${this.K.length - 1}];`);
    }
    this.buf = '';
  }

  emit(code) {
    this.code += code;
    this.size += code.length;
    if (this.size > CODE_LIMIT) throw new Unsupported('templates compile too large');
  }

  add(expr) {
    this.flush();
    this.emit(`o+=${expr};`);
  }

  /** The value of `path` in `frames` (innermost last). */
  lookup(path, frames) {
    let f = frames.length - 1;
    while (f >= 0 && !frames[f].has(path[0])) f--;
    if (f < 0) return UNDEF;
    let v = frames[f].get(path[0]);
    for (let k = 1; k < path.length; k++) {
      if (v.c) return { c: true, v: lookupConst(v.v, path, k) };
      const t = v.t;
      if (typeof t === 'string') return UNDEF;
      if (t.arr !== undefined) {
        if (path[k] !== 'length') return UNDEF;
        v = { c: false, t: 'num', e: `${v.e}.length` };
      } else if (t.obj !== undefined) v = t.obj(path[k], v.e);
      else if (t.loop !== undefined) v = Object.hasOwn(t.loop, path[k]) ? t.loop[path[k]] : UNDEF;
      else if (t.opt !== undefined) {
        const inner = t.opt(path[k], v.e);
        if (inner.c) v = inner;
        else if (inner.t === 'str') {
          const g = (x) => `(${v.e}===null?"":${x})`;
          v = { c: false, t: 'str', e: g(inner.e), hj: inner.hj && g(inner.hj), j: inner.j && g(inner.j) };
        } else throw new Unsupported('a path through a nullable record');
      }
    }
    return v;
  }

  /** A run-time value's truth: true, false or a JavaScript condition. */
  truth(v) {
    const t = v.t;
    if (t === 'str') return `(${v.e}!=="")`;
    if (t === 'num') return `(${v.e}!==0)`;
    if (t === 'bool') return `(${v.e})`;
    if (t.arr !== undefined) return `(${v.e}.length!==0)`;
    if (t.opt !== undefined) return `(${v.e}!==null)`;
    return true;
  }

  /** A run-time value's text, HTML-escaped (raw false) or not, JSON-escaped: '' or an expression. */
  text(v, raw) {
    const t = v.t;
    if (t === 'str') return raw ? v.j ?? `J(${v.e})` : v.hj ?? `JH(${v.e})`;
    if (t === 'num') return `(""+${v.e})`;
    if (t === 'bool') return `(${v.e}?"true":"false")`;
    if (t.arr !== undefined) return `SEP(${v.e}.length)`;
    return '';
  }

  nodes(nodes, frames, depth) {
    for (const n of nodes) {
      switch (n.kind) {
        case 'text':
          this.lit(J(n.text));
          break;
        case 'var':
        case 'raw': {
          const v = this.lookup(n.path, frames);
          if (v.c) {
            const s = textOf(v.v);
            this.lit(J(n.kind === 'raw' ? s : esc(s)));
          } else {
            const x = this.text(v, n.kind === 'raw');
            if (x !== '') this.add(x);
          }
          break;
        }
        case 'if': {
          const v = this.lookup(n.path, frames);
          const cond = v.c ? truthy(v.v) : this.truth(v);
          if (cond === true || cond === false) {
            const branch = cond !== n.not ? n.then : n.else;
            if (branch !== null) this.nodes(branch, frames, depth);
            break;
          }
          this.flush();
          this.emit(`if(${n.not ? '!' : ''}${cond}){`);
          this.nodes(n.then, frames, depth);
          this.flush();
          if (n.else !== null) {
            this.emit('}else{');
            this.nodes(n.else, frames, depth);
            this.flush();
          }
          this.emit('}');
          break;
        }
        case 'for': {
          const v = this.lookup(n.path, frames);
          if (v.c) {
            const xs = v.v;
            if (!Array.isArray(xs)) break;
            xs.forEach((x, k) => {
              const frame = new Map([[n.name, { c: true, v: x }]]);
              frame.set('loop', { c: true, v: { index: k + 1, first: k === 0, last: k === xs.length - 1, length: xs.length } });
              this.nodes(n.body, [...frames, frame], depth);
            });
            break;
          }
          if (typeof v.t === 'string' || v.t.arr === undefined) break;
          const id = ++this.vars;
          const a = `a${id}`;
          const len = `n${id}`;
          const k = `k${id}`;
          const x = `x${id}`;
          this.flush();
          this.emit(`{const ${a}=${v.e},${len}=${a}.length;for(let ${k}=0;${k}<${len};${k}++){const ${x}=${a}[${k}];`);
          const el = typeof v.t.arr === 'string' ? { c: false, t: v.t.arr, e: x } : { c: false, t: v.t.arr, e: x };
          const loop = {
            c: false,
            t: {
              loop: {
                index: { c: false, t: 'num', e: `(${k}+1)` },
                first: { c: false, t: 'bool', e: `${k}===0` },
                last: { c: false, t: 'bool', e: `${k}===${len}-1` },
                length: { c: false, t: 'num', e: len },
              },
            },
            e: 'null',
          };
          const frame = new Map([[n.name, el]]);
          frame.set('loop', loop);
          this.nodes(n.body, [...frames, frame], depth);
          this.flush();
          this.emit('}}');
          break;
        }
        case 'include': {
          const t = this.templates.get(n.name);
          if (t !== undefined && depth < INCLUDE_DEPTH) this.nodes(t, frames, depth + 1);
          break;
        }
      }
    }
  }
}

// ---------------------------------------------------------------- the build

const RESERVED = ['posts', 'tags', 'page'];

function resolvePath(dir, rel) {
  const parts = dir === '' ? [] : dir.split('/');
  for (const seg of rel.split('/')) {
    if (seg === '' || seg === '.') continue;
    if (seg === '..') {
      if (parts.length === 0) return null;
      parts.pop();
    } else parts.push(seg);
  }
  return parts.join('/');
}

const dirOf = (path) => (path.includes('/') ? path.slice(0, path.lastIndexOf('/')) : '');
const SCHEME = /^[A-Za-z][A-Za-z0-9+.-]*:/;
const ODD_LINE_ENDS = /[\u2028\u2029]/;

/** A record's string field with its escaped twins. */
function setStr(r, f, s) {
  r[f] = s;
  r[`${f}H`] = JH(s);
  r[`${f}J`] = J(s);
}

/** One site's answer as JSON text. */
export function buildJson(input) {
  const site = input.site;
  if (typeof site.base_url !== 'string' || !Array.isArray(input.files)) throw new Unsupported('input shape');
  const base = site.base_url.replace(/\/+$/, '');
  const report = [];

  // Templates.
  const templates = new Map();
  for (const name of Object.keys(input.templates).sort(byCodeUnits)) {
    try {
      templates.set(name, parseTemplate(name, input.templates[name]));
    } catch (err) {
      if (!(err instanceof TemplateError)) throw err;
      report.push(err.message);
    }
  }

  // Sources.
  const files = [...input.files].sort((x, y) => byCodeUnits(x.path, y.path));
  const sources = [];
  const statics = [];
  for (const f of files) {
    if (typeof f.path !== 'string' || typeof f.content !== 'string') throw new Unsupported('a file');
    if (f.path.endsWith('.md')) {
      let text = f.content;
      if (text.includes('\r')) text = text.replace(/\r\n?/g, '\n');
      sources.push({ path: f.path, text });
    } else statics.push(f);
  }

  // Front matter.
  const kept = [];
  for (const s of sources) {
    const fm = parseFrontMatter(s.text);
    if ('error' in fm) {
      report.push(`refused ${s.path}: ${fm.error}`);
      continue;
    }
    s.data = fm.data;
    s.body = fm.body;
    s.post = s.path.startsWith('posts/');
    const own = s.path.startsWith('tags/') ? 'tags' : s.path.startsWith('page/') ? 'page' : undefined;
    if (own !== undefined) {
      report.push(`refused ${s.path}: ${own}/ is the generator's own`);
      continue;
    }
    if (s.post && !Object.hasOwn(s.data, 'date')) {
      report.push(`refused ${s.path}: a post needs a date`);
      continue;
    }
    const layout = s.data.layout ?? (s.post ? 'post' : 'page');
    if (!templates.has(layout)) {
      report.push(`refused ${s.path}: layout "${layout}" is no template that parses`);
      continue;
    }
    s.layout = layout;
    if (s.data.draft === true) {
      report.push(`draft ${s.path}`);
      continue;
    }
    if (ODD_LINE_ENDS.test(s.body)) throw new Unsupported('a body with U+2028 or U+2029');
    kept.push(s);
  }

  // Permalinks.
  const taken = new Map();
  const bySource = new Map();
  for (const s of kept) {
    const dir = s.post ? 'posts' : dirOf(s.path);
    let set = taken.get(dir);
    if (set === undefined) taken.set(dir, (set = new Set(dir === '' ? RESERVED : [])));
    const slug = uniqueSlug(s.data.slug ?? slugify(s.data.title), set);
    s.outDir = dir === '' ? slug : `${dir}/${slug}`;
    setStr(s, 'slug', slug);
    setStr(s, 'url', `${base}/${s.outDir}/`);
    bySource.set(s.path, s);
  }

  // Markdown, by md.wasm: every kept body, then its links resolved against the site table.
  let table = `B${base}\n`;
  for (const s of kept) table += `P${s.path}\t${s.url}\n`;
  for (const f of statics) table += `S${f.path}\n`;
  let inBytes = 0;
  let maxBody = 0;
  for (const s of kept) {
    if (!s.body.isWellFormed()) throw new Unsupported('a body with a lone surrogate');
    const n = s.body.length * 3 + s.path.length * 3 + 8;
    inBytes += n;
    if (n > maxBody) maxBody = n;
  }
  for (const f of statics) if (/[\t\n]/.test(f.path)) throw new Unsupported('a static path with a tab or line break');
  for (const s of kept) if (/[\t\n]/.test(s.path)) throw new Unsupported('a path with a tab or line break');
  if (/[\t\n]/.test(base)) throw new Unsupported('a base url with a tab or line break');
  const tableBytes = table.length * 3;
  md.reserve(inBytes, maxBody, tableBytes);
  let u8 = new Uint8Array(md.memory.buffer);
  const siteAt = md.site_ptr();
  md.site_load(ENC.encodeInto(table, u8.subarray(siteAt, siteAt + tableBytes)).written);
  const inAt = md.in_ptr();
  let at = 0;
  const meta = new Uint32Array(md.memory.buffer, md.meta_ptr(), 8 + 5 * 512);
  const ids = md.ids_ptr();
  const spans = new Uint32Array(kept.length * 2 + 2);
  for (let k = 0; k < kept.length; k++) {
    const s = kept[k];
    const body = at;
    const bl = ENC.encodeInto(s.body, u8.subarray(inAt + at)).written;
    at += bl;
    const dir = at;
    const dl = ENC.encodeInto(dirOf(s.path), u8.subarray(inAt + at)).written;
    at += dl;
    md.render(body, bl, dir, dl, k);
    spans[2 * k] = meta[3];
    spans[2 * k + 1] = meta[4];
    s.reading = Math.max(1, Math.ceil(meta[2] / 200));
    s.u0 = meta[0];
    s.u1 = meta[1];
    const toc = [];
    for (let t = 0; t < meta[5]; t++) {
      const m = 8 + 5 * t;
      let id = '';
      for (let q = 0; q < meta[m + 2]; q++) id += String.fromCharCode(u8[ids + meta[m + 1] + q]);
      toc.push({ level: meta[m], id, idH: id, idJ: id, htmlJ: meta[m + 3], h1: meta[m + 4] });
    }
    s.toc = toc;
  }
  u8 = new Uint8Array(md.memory.buffer);
  const outAt = md.out_ptr();
  const big = DEC.decode(u8.subarray(outAt, outAt + md.out_len()));
  const broken = new Set();
  {
    const brkAt = md.brk_ptr();
    const end = brkAt + md.brk_len();
    const w = new Uint32Array(md.memory.buffer);
    for (let q = brkAt; q < end; ) {
      const src = w[q >> 2];
      const n = w[(q >> 2) + 1];
      broken.add(`broken-link ${kept[src].path} -> ${DEC.decode(u8.subarray(q + 8, q + 8 + n))}`);
      q += (8 + n + 3) & ~3;
    }
  }
  for (let k = 0; k < kept.length; k++) {
    const s = kept[k];
    s.htmlJ = big.slice(s.u0, s.u1);
    s.excerptJ = spans[2 * k] === 0xffffffff ? '' : big.slice(spans[2 * k], spans[2 * k + 1]);
    for (const h of s.toc) h.htmlJ = big.slice(h.htmlJ, h.h1);
    setStr(s, 'title', s.data.title);
    const date = s.data.date;
    setStr(s, 'date', date ?? '');
    setStr(s, 'dateLong', date === undefined ? '' : longDate(date));
    setStr(s, 'summary', s.data.summary ?? '');
    s.newer = null;
    s.older = null;
  }

  // Collections.
  const posts = kept.filter((s) => s.post);
  posts.sort((x, y) => byCodeUnits(y.data.date, x.data.date) || byCodeUnits(x.slug, y.slug));
  for (let k = 0; k < posts.length; k++) {
    posts[k].newer = k === 0 ? null : posts[k - 1];
    posts[k].older = k === posts.length - 1 ? null : posts[k + 1];
  }

  // Tags.
  const bySlug = new Map();
  const slugCache = new Map();
  const tagSlug = (name) => {
    let s = slugCache.get(name);
    if (s === undefined) slugCache.set(name, (s = slugify(name)));
    return s;
  };
  const ownTags = (s) => {
    const seen = new Set();
    return (s.data.tags ?? []).filter((name) => !seen.has(tagSlug(name)) && seen.add(tagSlug(name)));
  };
  for (const p of posts) {
    p.tagRefs = [];
    for (const name of ownTags(p)) {
      const slug = tagSlug(name);
      let tag = bySlug.get(slug);
      if (tag === undefined) {
        tag = { posts: [] };
        setStr(tag, 'name', name);
        setStr(tag, 'slug', slug);
        setStr(tag, 'url', `${base}/tags/${slug}/`);
        bySlug.set(slug, tag);
      }
      tag.posts.push(p);
      p.tagRefs.push(tag);
    }
  }
  for (const s of kept) {
    if (s.post) continue;
    s.tagRefs = ownTags(s).map((name) => {
      const t = bySlug.get(tagSlug(name));
      if (t !== undefined) return t;
      const r = {};
      setStr(r, 'name', name);
      setStr(r, 'slug', tagSlug(name));
      setStr(r, 'url', '');
      return r;
    });
  }
  const tags = [...bySlug.values()].sort((x, y) => y.posts.length - x.posts.length || byCodeUnits(x.slug, y.slug));
  for (const t of tags) t.count = t.posts.length;

  // Layouts.
  const refCard = (s) => ({
    title: s.title,
    url: s.url,
    date: s.date,
    date_long: s.dateLong,
    summary: s.summary,
    excerpt: UNJ(s.excerptJ),
    reading_minutes: s.reading,
    tags: s.tagRefs.map((t) => ({ name: t.name, slug: t.slug, url: t.url })),
  });
  const globals = {
    site: { title: site.title, base_url: base },
    tags: tags.map((t) => ({ name: t.name, slug: t.slug, url: t.url, count: t.count })),
    recent: posts.slice(0, site.recent).map(refCard),
  };
  const cc = new Compiler(templates, globals);
  const out = new Map();
  const fns = new Map();
  for (const s of kept) {
    let fn = fns.get(s.layout);
    if (fn === undefined) fns.set(s.layout, (fn = cc.compile(s.layout, 'page')));
    out.set(`${s.outDir}/index.html`, fn(s));
  }
  const per = site.per_page;
  const pages = Math.max(1, Math.ceil(posts.length / per));
  const indexFn = cc.compile('index', 'index');
  if (indexFn !== null) {
    const indexUrl = (k) => (k === 1 ? `${base}/` : `${base}/page/${k}/`);
    for (let k = 1; k <= pages; k++) {
      const pager = { number: k, total: pages };
      setStr(pager, 'url', indexUrl(k));
      setStr(pager, 'newerUrl', k === 1 ? '' : indexUrl(k - 1));
      setStr(pager, 'olderUrl', k === pages ? '' : indexUrl(k + 1));
      out.set(k === 1 ? 'index.html' : `page/${k}/index.html`, indexFn(null, posts.slice((k - 1) * per, k * per), pager));
    }
  }
  const tagFn = cc.compile('tag', 'tag');
  if (tagFn !== null) for (const t of tags) out.set(`tags/${t.slug}/index.html`, tagFn(null, null, null, t));
  const tagsFn = cc.compile('tags', 'tags');
  if (tagsFn !== null) out.set('tags/index.html', tagsFn());
  for (const f of statics) {
    if (out.has(f.path)) report.push(`shadowed ${f.path}: a built page has its path`);
    else out.set(f.path, J(f.content));
  }

  // The report.
  const lines = [...report, ...broken].sort(byCodeUnits);
  lines.push(`built ${kept.length - posts.length} pages, ${posts.length} posts, ${tags.length} tags and ${statics.length} static files`);
  out.set('build-report.txt', J(`${lines.join('\n')}\n`));

  let json = '{';
  let first = true;
  for (const [k, v] of out) {
    json += `${first ? '' : ','}"${J(k)}":"${v}"`;
    first = false;
  }
  return `${json}}`;
}
