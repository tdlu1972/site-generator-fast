// site.c, built to site.wasm by build.sh: makefun-site 1.0.0's build (vendor/makefun-site, whose README.md states every
// rule) for every case of /input/cases.json, read from the file's bytes and written as answers.json's bytes, with no
// JavaScript string in between. main.ts reads the file into this module's memory, calls run(), and writes what flush()
// hands it.
//
// The pipeline is the reference's, in its order: the templates parsed, the sources sorted, their front matter read,
// permalinks given, Markdown rendered (links resolved, ids made unique), posts ordered, tags gathered, every page
// rendered through its layout, then the index, tag and tags pages, the static files and the report. Each output text
// is written as its JSON spelling straight into the answer. Templates are rendered by walking their parsed trees with
// values of known shapes (a page, a card, a tag ...), each looked up by an interned field name.
//
// Only a slug of text outside ASCII is handed back to JavaScript (uslug: NFKD needs Unicode's tables). Anything this
// does not read the reference's way (an input of another shape, a lone surrogate, a line separator in a body ...)
// traps, and main.ts answers the file with the reference instead.

#include <wasm_simd128.h>

typedef unsigned char u8;
typedef unsigned int u32;
#define NONE 0xffffffffu
#define SPLAT(c) wasm_i8x16_splat((char)(c))
#define EQ(v, c) wasm_i8x16_eq((v), SPLAT(c))
#define BITS(m) ((u32)wasm_i8x16_bitmask(m))
#define TRAP() __builtin_trap()

extern u8 __heap_base;
static u32 heap_top;

static void *bump(u32 size) {
  if (!heap_top) heap_top = ((u32)&__heap_base + 15) & ~15u;
  u32 p = heap_top;
  u32 need = p + size + 64;
  u32 have = (u32)__builtin_wasm_memory_size(0) * 65536u;
  if (need > have && __builtin_wasm_memory_grow(0, (need - have + 65535) / 65536) == (__SIZE_TYPE__)-1) TRAP();
  heap_top = (p + size + 15) & ~15u;
  return (void *)p;
}

static void *zalloc(u32 size) {
  u8 *p = bump(size);
  __builtin_memset(p, 0, size);
  return p;
}

__attribute__((import_module("env"), import_name("flush"))) void flush(const u8 *p, u32 n);
__attribute__((import_module("env"), import_name("uslug"))) u32 uslug(const u8 *p, u32 n, u8 *out);

typedef struct {
  const u8 *p;
  u32 n;
} S;

static u32 slen(const char *c) {
  u32 n = 0;
  while (c[n]) n++;
  return n;
}

static S mk(const u8 *p, u32 n) {
  S s = {p, n};
  return s;
}
#define SL(lit) mk((const u8 *)(lit), sizeof(lit) - 1)

static int seq(S a, S b) {
  if (a.n != b.n) return 0;
  for (u32 k = 0; k < a.n; k++)
    if (a.p[k] != b.p[k]) return 0;
  return 1;
}

static int starts(S a, S pre) {
  if (a.n < pre.n) return 0;
  for (u32 k = 0; k < pre.n; k++)
    if (a.p[k] != pre.p[k]) return 0;
  return 1;
}

static int ends(S a, S suf) {
  if (a.n < suf.n) return 0;
  for (u32 k = 0; k < suf.n; k++)
    if (a.p[a.n - suf.n + k] != suf.p[k]) return 0;
  return 1;
}

// Code-unit order of two UTF-8 strings, as JavaScript compares the strings they spell.
static u32 next_unit_key(const u8 *p, u32 n, u32 *k) {
  u8 c = p[*k];
  if (c < 0x80) {
    (*k)++;
    return c;
  }
  if (c < 0xe0) {
    u32 cp = ((u32)(c & 0x1f) << 6) | (p[*k + 1] & 0x3f);
    *k += 2;
    return cp;
  }
  if (c < 0xf0) {
    u32 cp = ((u32)(c & 0x0f) << 12) | ((u32)(p[*k + 1] & 0x3f) << 6) | (p[*k + 2] & 0x3f);
    *k += 3;
    return cp;
  }
  // A supplementary character: its high surrogate decides against any BMP unit; the low one breaks ties.
  u32 cp = ((u32)(c & 0x07) << 18) | ((u32)(p[*k + 1] & 0x3f) << 12) | ((u32)(p[*k + 2] & 0x3f) << 6) | (p[*k + 3] & 0x3f);
  *k += 4;
  cp -= 0x10000;
  return ((0xd800 + (cp >> 10)) << 10) | (cp & 0x3ff) | 0x80000000u;
}

static int cmp16(S a, S b) {
  u32 i = 0, j = 0;
  while (i < a.n && j < b.n) {
    u32 x = next_unit_key(a.p, a.n, &i), y = next_unit_key(b.p, b.n, &j);
    if (x == y) continue;
    u32 xh = x & 0x80000000u ? (x & 0x7fffffffu) >> 10 : x;
    u32 yh = y & 0x80000000u ? (y & 0x7fffffffu) >> 10 : y;
    if (xh != yh) return xh < yh ? -1 : 1;
    // same high surrogate: both supplementary
    return (x & 0x3ff) < (y & 0x3ff) ? -1 : 1;
  }
  if (i < a.n) return 1;
  if (j < b.n) return -1;
  return 0;
}

// ---------------------------------------------------------------- output buffers

typedef struct {
  u8 *p;
  u32 len, cap;
} Buf;

static Buf OUT, MDB, TMP;
static Buf *W;  // where put* write

static void grow(Buf *b, u32 extra) {
  u32 cap = b->cap * 2;
  if (cap < b->len + extra + 4096) cap = b->len + extra + 4096;
  u8 *p = bump(cap + 64);
  __builtin_memcpy(p, b->p, b->len);
  b->p = p;
  b->cap = cap;
}

#define ROOM(k) \
  if (W->len + (k) > W->cap) grow(W, (k))

// n bytes from p to q, 16 at a time: it may write up to 15 bytes past q + n and read up to 15 past p + n, so every
// buffer keeps that much room. (memory.copy is a call out of Liftoff's code, dear for a short run.)
#define INLINE static inline __attribute__((always_inline))

INLINE void copy16(u8 *q, const u8 *p, u32 n) {
  for (u32 k = 0; k < n; k += 16) wasm_v128_store(q + k, wasm_v128_load(p + k));
}

INLINE void put(const u8 *p, u32 n) {
  ROOM(n + 32);
  u8 *q = W->p + W->len;
  if (n <= 32) {
    wasm_v128_store(q, wasm_v128_load(p));
    wasm_v128_store(q + 16, wasm_v128_load(p + 16));
  } else if (n <= 512)
    copy16(q, p, n);
  else
    __builtin_memcpy(q, p, n);
  W->len += n;
}
static void puts_(S s) { put(s.p, s.n); }
#define LIT(s) put((const u8 *)(s), sizeof(s) - 1)

static const u8 HEX[] = "0123456789abcdef";

// Byte classes by nibble lookup (two swizzles in place of a compare per character): a byte is in a set when the entry
// of its low nibble and the entry of its high nibble share a bit. Bit 1: below 0x20 (high nibble 0 or 1). Bit 2:
// " & ' (high 2). Bit 4: < > (high 3). Bit 8: \ (high 5). Bit 16: * (high 2). Bit 32: [ (high 5). Bit 64: ` (high 6).
// A byte at or past 0x80 has a high nibble whose entry is 0.
#define LO_TBL wasm_i8x16_make(1 | 64, 1, 1 | 2, 1, 1, 1, 1 | 2, 1 | 2, 1, 1, 1 | 16, 1 | 32, 1 | 4 | 8, 1, 1 | 4, 1)
#define HI_TBL wasm_i8x16_make(1, 1, 2 | 16, 4, 0, 8 | 32, 64, 0, 0, 0, 0, 0, 0, 0, 0, 0)
// The bits of the 16 bytes of v whose classes meet `set`.
INLINE u32 classes(v128_t v, u8 set) {
  v128_t lo = wasm_i8x16_swizzle(LO_TBL, wasm_v128_and(v, SPLAT(0x0f)));
  v128_t hi = wasm_i8x16_swizzle(HI_TBL, wasm_u8x16_shr(v, 4));
  v128_t m = wasm_v128_and(wasm_v128_and(lo, hi), SPLAT(set));
  return BITS(wasm_i8x16_ne(m, wasm_i8x16_splat(0)));
}
#define C_TEXT (1 | 2 | 4 | 8)        // what put_text escapes
#define C_INLINE (C_TEXT | 16 | 32 | 64)  // and what opens something inline: \ ` * [

// One character that HTML or JSON escapes, written at q; past it.
__attribute__((noinline)) static u8 *esc_text(u8 *q, u8 c) {
  switch (c) {
    case '&': *q++ = '&'; *q++ = 'a'; *q++ = 'm'; *q++ = 'p'; *q++ = ';'; break;
    case '<': *q++ = '&'; *q++ = 'l'; *q++ = 't'; *q++ = ';'; break;
    case '>': *q++ = '&'; *q++ = 'g'; *q++ = 't'; *q++ = ';'; break;
    case '"': *q++ = '&'; *q++ = 'q'; *q++ = 'u'; *q++ = 'o'; *q++ = 't'; *q++ = ';'; break;
    case '\'': *q++ = '&'; *q++ = '#'; *q++ = '3'; *q++ = '9'; *q++ = ';'; break;
    case '\\': *q++ = '\\'; *q++ = '\\'; break;
    case '\n': *q++ = '\\'; *q++ = 'n'; break;
    case '\t': *q++ = '\\'; *q++ = 't'; break;
    default: *q++ = '\\'; *q++ = 'u'; *q++ = '0'; *q++ = '0'; *q++ = HEX[c >> 4]; *q++ = HEX[c & 15];
  }
  return q;
}

// One character, escaped if HTML or JSON escapes it.
INLINE u8 *esc_or_copy(u8 *q, u8 c) {
  if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'' || c == '\\' || c < 0x20) return esc_text(q, c);
  *q = c;
  return q + 1;
}

// p[0, n) HTML-escaped (& < > " ') and then JSON-escaped, a block of 16 at a time (the last one masked to what is
// left: reading past p + n is safe, see copy16); each block is stored whole, then the pointer moves past what was clean.
INLINE void put_text(const u8 *p, u32 n) {
  ROOM(n * 6 + 32);
  u8 *q = W->p + W->len;
  u32 k = 0;
  while (k < n) {
    v128_t v = wasm_v128_load(p + k);
    wasm_v128_store(q, v);
    u32 left = n - k;
    u32 b = classes(v, C_TEXT);
    if (left < 16) b &= (1u << left) - 1;
    if (!b) {
      u32 adv = left < 16 ? left : 16;
      q += adv;
      k += adv;
      continue;
    }
    u32 z = __builtin_ctz(b);
    q = esc_text(q + z, p[k + z]);
    k += z + 1;
  }
  W->len = (u32)(q - W->p);
}
static void put_textS(S s) { put_text(s.p, s.n); }

// p[0, n) JSON-escaped only.
static void put_json(const u8 *p, u32 n) {
  ROOM(n * 6 + 32);
  u8 *q = W->p + W->len;
  u32 k = 0;
  for (;;) {
    while (k < n) {
      v128_t v = wasm_v128_load(p + k);
      v128_t m = wasm_v128_or(wasm_u8x16_lt(v, SPLAT(0x20)), wasm_v128_or(EQ(v, '"'), EQ(v, '\\')));
      wasm_v128_store(q, v);
      u32 left = n - k;
      u32 b = BITS(m);
      if (left < 16) b &= (1u << left) - 1;
      if (!b) {
        u32 adv = left < 16 ? left : 16;
        q += adv;
        k += adv;
        continue;
      }
      u32 z = __builtin_ctz(b);
      q += z;
      k += z;
      break;
    }
    if (k >= n) break;
    u8 c = p[k++];
    if (c >= 0x20 && c != '"' && c != '\\') {
      *q++ = c;
      continue;
    }
    *q++ = '\\';
    if (c == '"' || c == '\\') *q++ = c;
    else if (c == '\n') *q++ = 'n';
    else if (c == '\t') *q++ = 't';
    else {
      *q++ = 'u'; *q++ = '0'; *q++ = '0'; *q++ = HEX[c >> 4]; *q++ = HEX[c & 15];
    }
  }
  W->len = (u32)(q - W->p);
}
static void put_jsonS(S s) { put_json(s.p, s.n); }

static void put_num(u32 v) {
  u8 b[12];
  u32 n = 0;
  do {
    b[n++] = (u8)('0' + v % 10);
    v /= 10;
  } while (v);
  ROOM(n);
  while (n) W->p[W->len++] = b[--n];
}

// A UTF-8 encoding of code point cp at q; its length.
static u32 utf8(u8 *q, u32 cp) {
  if (cp < 0x80) {
    q[0] = (u8)cp;
    return 1;
  }
  if (cp < 0x800) {
    q[0] = (u8)(0xc0 | (cp >> 6));
    q[1] = (u8)(0x80 | (cp & 0x3f));
    return 2;
  }
  if (cp < 0x10000) {
    q[0] = (u8)(0xe0 | (cp >> 12));
    q[1] = (u8)(0x80 | ((cp >> 6) & 0x3f));
    q[2] = (u8)(0x80 | (cp & 0x3f));
    return 3;
  }
  q[0] = (u8)(0xf0 | (cp >> 18));
  q[1] = (u8)(0x80 | ((cp >> 12) & 0x3f));
  q[2] = (u8)(0x80 | ((cp >> 6) & 0x3f));
  q[3] = (u8)(0x80 | (cp & 0x3f));
  return 4;
}

static u32 hexval(u8 c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  TRAP();
}

// The JSON spelling p[0, n) (a string's inside, as this file writes it) decoded, then HTML- and JSON-escaped.
static void put_unjson_text(const u8 *p, u32 n) {
  u8 *tmp = bump(n + 8);
  u32 t = 0;
  for (u32 k = 0; k < n; k++) {
    u8 c = p[k];
    if (c != '\\') {
      tmp[t++] = c;
      continue;
    }
    c = p[++k];
    if (c == 'n') tmp[t++] = '\n';
    else if (c == 't') tmp[t++] = '\t';
    else if (c == 'r') tmp[t++] = '\r';
    else if (c == 'b') tmp[t++] = 8;
    else if (c == 'f') tmp[t++] = 12;
    else if (c == 'u') {
      u32 cp = (hexval(p[k + 1]) << 12) | (hexval(p[k + 2]) << 8) | (hexval(p[k + 3]) << 4) | hexval(p[k + 4]);
      k += 4;
      if (cp >= 0xd800 && cp < 0xe000) TRAP();
      t += utf8(tmp + t, cp);
    } else
      tmp[t++] = c;
  }
  put_text(tmp, t);
}

// ---------------------------------------------------------------- text

// Whether p[0..] starts a character a JavaScript \s matches (and String.prototype.trim drops); its length in *len.
static int ws_at(const u8 *p, u32 n, u32 *len) {
  u8 c = p[0];
  if (c < 0x80) {
    *len = 1;
    return c == 32 || (c >= 9 && c <= 13);
  }
  if (c == 0xc2 && n >= 2) {
    *len = 2;
    return p[1] == 0xa0;
  }
  if (n >= 3 && c >= 0xe0 && c < 0xf0) {
    *len = 3;
    if (c == 0xe1) return p[1] == 0x9a && p[2] == 0x80;
    if (c == 0xe2) {
      if (p[1] == 0x80) return (p[2] >= 0x80 && p[2] <= 0x8a) || p[2] == 0xa8 || p[2] == 0xa9 || p[2] == 0xaf;
      return p[1] == 0x81 && p[2] == 0x9f;
    }
    if (c == 0xe3) return p[1] == 0x80 && p[2] == 0x80;
    if (c == 0xef) return p[1] == 0xbb && p[2] == 0xbf;
    return 0;
  }
  *len = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
  if (*len > n) *len = n;
  return 0;
}

static int has_ws(const u8 *p, u32 n) {
  for (u32 k = 0; k < n;) {
    u32 l;
    if (ws_at(p + k, n - k, &l)) return 1;
    k += l;
  }
  return 0;
}

// String.prototype.trim.
static S trimS(S s) {
  u32 a = 0, e = s.n, l;
  while (a < e && ws_at(s.p + a, e - a, &l)) a += l;
  // from the end: find the last non-space character
  u32 last = a;
  for (u32 k = a; k < e;) {
    if (!ws_at(s.p + k, e - k, &l)) last = k + l;
    k += l;
  }
  return mk(s.p + a, last - a);
}

// Whether p[0, n) holds U+2028 or U+2029 (E2 80 A8, E2 80 A9): a block at a time, looking at each E2 only.
static int line_sep(const u8 *p, u32 n) {
  u32 k = 0;
  for (; k + 16 <= n; k += 16) {
    u32 b = BITS(EQ(wasm_v128_load(p + k), 0xe2));
    while (b) {
      u32 j = k + __builtin_ctz(b);
      if (j + 2 < n && p[j + 1] == 0x80 && (p[j + 2] == 0xa8 || p[j + 2] == 0xa9)) return 1;
      b &= b - 1;
    }
  }
  for (; k + 2 < n; k++)
    if (p[k] == 0xe2 && p[k + 1] == 0x80 && (p[k + 2] == 0xa8 || p[k + 2] == 0xa9)) return 1;
  return 0;
}

static u32 words(const u8 *p, u32 n) {
  u32 w = 0;
  u32 in = 0;
  u32 k = 0;
  while (k < n) {
    if (k + 16 <= n) {
      v128_t v = wasm_v128_load(p + k);
      if (!BITS(v)) {
        // all ASCII: a word starts at each non-space after a space
        v128_t ws = wasm_v128_or(EQ(v, ' '), wasm_u8x16_le(wasm_i8x16_sub(v, SPLAT(9)), SPLAT(4)));
        u32 non = ~BITS(ws) & 0xffffu;
        u32 starts = non & ~((non << 1) | in);
        w += __builtin_popcount(starts);
        in = non >> 15;
        k += 16;
        continue;
      }
    }
    u8 c = p[k];
    u32 l = 1;
    int sp;
    if (c < 0x80) sp = c == 32 || (c >= 9 && c <= 13);
    else sp = ws_at(p + k, n - k, &l);
    if (sp) in = 0;
    else if (!in) {
      in = 1;
      w++;
    }
    k += l;
  }
  return w;
}

#include "nfkd.h"

// What NFKD leaves of code point cp, spelled for a slug (nfkd.h): 1 with *sp, *n set, or 0 when the table does not
// know it. CJK ideographs and Hangul syllables leave nothing ASCII: one "-".
static int nfkd_spell(u32 cp, const char **sp, u32 *n) {
  for (u32 r = 0; r < sizeof NFKD_RANGES / sizeof NFKD_RANGES[0]; r++)
    if (cp >= NFKD_RANGES[r][0] && cp < NFKD_RANGES[r][1]) {
      u32 e = NFKD_TABLES[r][cp - NFKD_RANGES[r][0]];
      *sp = NFKD_POOL + (e >> 4);
      *n = e & 15;
      return 1;
    }
  if ((cp >= 0x3400 && cp < 0x4dc0) || (cp >= 0x4e00 && cp < 0xa000) || (cp >= 0xac00 && cp < 0xd7a4)) {
    *sp = "-";
    *n = 1;
    return 1;
  }
  return 0;
}

// makefun-site's slug of s, in a new string: NFKD with marks dropped, A-Z lowered, each run of other characters one
// "-" (none at either end), cut to 64 (a "-" left at the cut dropped), "untitled" when empty.
static S slugify(S s) {
  u8 *out = bump(s.n * 3 + 72 > 128 ? s.n * 3 + 72 : 128);
  u32 len = 0;
  int gap = 0;
  for (u32 k = 0; k < s.n;) {
    u8 c = s.p[k];
    const char *sp;
    u32 sn;
    u8 one;
    if (c < 0x80) {
      one = c;
      sp = (const char *)&one;
      sn = 1;
      k++;
    } else {
      u32 cp, l;
      if (c < 0xe0) {
        cp = ((u32)(c & 0x1f) << 6) | (s.p[k + 1] & 0x3f);
        l = 2;
      } else if (c < 0xf0) {
        cp = ((u32)(c & 0x0f) << 12) | ((u32)(s.p[k + 1] & 0x3f) << 6) | (s.p[k + 2] & 0x3f);
        l = 3;
      } else {
        cp = 0x110000;
        l = 4;
      }
      if (!nfkd_spell(cp, &sp, &sn)) {
        u32 n = uslug(s.p, s.n, out);
        return mk(out, n);
      }
      k += l;
    }
    for (u32 q = 0; q < sn; q++) {
      u8 ch = (u8)sp[q];
      if (ch >= 'A' && ch <= 'Z') ch = (u8)(ch + 32);
      else if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))) {
        gap = len > 0;
        continue;
      }
      if (gap) {
        if (len < 64) out[len] = '-';
        len++;
      }
      gap = 0;
      if (len < 64) out[len] = ch;
      len++;
    }
  }
  if (len > 64) len = 64;
  while (len > 0 && out[len - 1] == '-') len--;
  if (len == 0) {
    __builtin_memcpy(out, "untitled", 8);
    len = 8;
  }
  return mk(out, len);
}

static S cat(S a, S b) {
  u8 *p = bump(a.n + b.n + 1);
  __builtin_memcpy(p, a.p, a.n);
  __builtin_memcpy(p + a.n, b.p, b.n);
  return mk(p, a.n + b.n);
}

static S numS(u32 v) {
  u8 b[12];
  u32 n = 0;
  do {
    b[n++] = (u8)('0' + v % 10);
    v /= 10;
  } while (v);
  u8 *p = bump(12);
  for (u32 k = 0; k < n; k++) p[k] = b[n - 1 - k];
  return mk(p, n);
}

// ---------------------------------------------------------------- string maps

typedef struct {
  S *keys;
  u32 *vals;
  u32 cap, n;
} Map;

static void map_init(Map *m, u32 want) {
  u32 cap = 64;
  while (cap < want * 2 + 8) cap *= 2;
  m->keys = zalloc(cap * sizeof(S));
  m->vals = bump(cap * 4);
  m->cap = cap;
  m->n = 0;
}

static u32 hashS(S s) {
  u32 h = 2166136261u;
  for (u32 k = 0; k < s.n; k++) h = (h ^ s.p[k]) * 16777619u;
  return h;
}

// The slot of key k (an empty one, keys[slot].p == 0, when absent).
static u32 map_slot(Map *m, S k) {
  for (u32 s = hashS(k) & (m->cap - 1);; s = (s + 1) & (m->cap - 1)) {
    if (!m->keys[s].p) return s;
    if (seq(m->keys[s], k)) return s;
  }
}

static u32 map_get(Map *m, S k) {
  u32 s = map_slot(m, k);
  return m->keys[s].p ? m->vals[s] : NONE;
}

// Adds k -> v unless k is there; 1 when added.
static int map_add(Map *m, S k, u32 v) {
  if ((m->n + 1) * 2 > m->cap) {
    Map bigger;
    map_init(&bigger, m->cap);
    for (u32 s = 0; s < m->cap; s++)
      if (m->keys[s].p) {
        u32 t = map_slot(&bigger, m->keys[s]);
        bigger.keys[t] = m->keys[s];
        bigger.vals[t] = m->vals[s];
        bigger.n++;
      }
    *m = bigger;
  }
  u32 s = map_slot(m, k);
  if (m->keys[s].p) return 0;
  m->keys[s] = k;
  m->vals[s] = v;
  m->n++;
  return 1;
}

// ---------------------------------------------------------------- the input's JSON

static const u8 *J;
static u32 jn, ji;

static void jws(void) {
  while (ji < jn && (J[ji] == ' ' || J[ji] == '\t' || J[ji] == '\n' || J[ji] == '\r')) ji++;
}

static void expect(u8 c) {
  jws();
  if (ji >= jn || J[ji] != c) TRAP();
  ji++;
}

static int peek(u8 c) {
  jws();
  return ji < jn && J[ji] == c;
}

static u32 RAW_A, RAW_B;  // the last string's spelling: J[RAW_A, RAW_B), its inside

// The first index from i of a quote or backslash in J.
static u32 scan_qb(u32 i) {
  const u8 *j = J;
  for (;;) {
    v128_t v = wasm_v128_load(j + i);
    u32 b = BITS(wasm_v128_or(EQ(v, '"'), EQ(v, '\\')));
    if (b) {
      i += __builtin_ctz(b);
      if (i >= jn) TRAP();
      return i;
    }
    i += 16;
    if (i >= jn) TRAP();
  }
}

// A JSON string, decoded to UTF-8 (pointing into J when it holds no escape).
static S jstr(void) {
  expect('"');
  u32 a = ji;
  u32 i = scan_qb(a);
  if (i >= jn) TRAP();
  if (J[i] == '"') {
    RAW_A = a;
    RAW_B = i;
    ji = i + 1;
    return mk(J + a, i - a);
  }
  // escapes: decode into a new string as long as the spelling (never longer than it)
  u32 cap = jn - a + 64 < 1u << 22 ? jn - a + 64 : 1u << 22;
  u8 *out = bump(cap);
  u32 o = 0;
  u32 k = a;
  for (;;) {
    // copy the plain run up to i
    u32 run = i - k;
    if (o + run + 16 > cap) TRAP();
    copy16(out + o, J + k, run);
    o += run;
    k = i;
    if (J[k] == '"') break;
    u8 c = J[++k];
    switch (c) {
      case 'n': out[o++] = '\n'; break;
      case 't': out[o++] = '\t'; break;
      case 'r': out[o++] = '\r'; break;
      case 'b': out[o++] = 8; break;
      case 'f': out[o++] = 12; break;
      case 'u': {
        u32 cp = (hexval(J[k + 1]) << 12) | (hexval(J[k + 2]) << 8) | (hexval(J[k + 3]) << 4) | hexval(J[k + 4]);
        k += 4;
        if (cp >= 0xdc00 && cp < 0xe000) TRAP();
        if (cp >= 0xd800 && cp < 0xdc00) {
          if (J[k + 1] != '\\' || J[k + 2] != 'u') TRAP();
          u32 lo = (hexval(J[k + 3]) << 12) | (hexval(J[k + 4]) << 8) | (hexval(J[k + 5]) << 4) | hexval(J[k + 6]);
          if (lo < 0xdc00 || lo >= 0xe000) TRAP();
          k += 6;
          cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
        }
        o += utf8(out + o, cp);
        break;
      }
      default: out[o++] = c;
    }
    k++;
    i = scan_qb(k);
  }
  RAW_A = a;
  RAW_B = k;
  ji = k + 1;
  heap_top = ((u32)out + o + 64 + 15) & ~15u;  // give back what the decoded string did not use
  return mk(out, o);
}

static void jskip(void);

static void jskip_container(u8 open, u8 close) {
  expect(open);
  if (peek(close)) {
    ji++;
    return;
  }
  for (;;) {
    if (open == '{') {
      jstr();
      expect(':');
    }
    jskip();
    if (peek(',')) {
      ji++;
      continue;
    }
    expect(close);
    return;
  }
}

static void jskip(void) {
  jws();
  if (ji >= jn) TRAP();
  u8 c = J[ji];
  if (c == '"') jstr();
  else if (c == '{') jskip_container('{', '}');
  else if (c == '[') jskip_container('[', ']');
  else
    while (ji < jn && J[ji] != ',' && J[ji] != '}' && J[ji] != ']' && J[ji] != ' ' && J[ji] != '\n' && J[ji] != '\t' && J[ji] != '\r') ji++;
}

// A whole number; anything else traps.
static u32 jint(void) {
  jws();
  u32 v = 0, d = 0;
  while (ji < jn && J[ji] >= '0' && J[ji] <= '9') {
    v = v * 10 + (J[ji++] - '0');
    if (++d > 9) TRAP();
  }
  if (!d) TRAP();
  if (ji < jn && (J[ji] == '.' || J[ji] == 'e' || J[ji] == 'E')) TRAP();
  return v;
}

// ---------------------------------------------------------------- a case

typedef struct {
  S path, content;
  u32 raw_a, raw_b;  // the content's spelling in J
} File;

typedef struct {
  u32 level;
  S id;
  u32 html_off, html_len;  // in MDB
} Toc;

typedef struct {
  S path, dir, text, body;
  // front matter
  S title, date, summary, layout, slug_key;
  int has_date, has_summary, has_layout, has_slug, has_tags, draft;
  S *tags;
  u32 ntags;
  // the build
  int post;
  u32 tpl;
  S slug, outdir, url, date_long;
  u32 html_off, html_len, ex_off, ex_len, toc0, ntoc, reading;
  u32 newer, older;
  u32 *refs, nrefs;  // tag indices (pseudo tags for a page's tag no post has)
} Src;

typedef struct {
  S name, slug, url;
  u32 *posts, nposts, cap;
} Tag;

typedef struct {
  u32 kind, next, a, b, c, d, e;
} Node;
enum { N_TEXT, N_VAR, N_RAW, N_IF, N_FOR, N_INCLUDE };

typedef struct {
  u32 n;
  S *names;
  u32 *ids;
} Path;

typedef struct {
  S name;
  int ok;
  u32 head;
} Tpl;

static S site_title, base;
static u32 per_page, recent_n;
static Tpl *tpls;
static u32 ntpl;
static Node *nodes;
static u32 nnodes, nodecap;
static Path *paths;
static u32 npaths, pathcap;
static File *files;
static u32 nfiles;
static Src *srcs;
static u32 nsrcs;
static u32 *kept, nkept;
static u32 *statics, nstatics;
static Toc *tocs;
static u32 ntocs, toccap;
static Tag *tags;
static u32 ntags_all, ntags;  // ntags: the posts' tags (sorted first ntags of `sorted_tags`)
static u32 *sorted_tags;
static u32 *posts, nposts;
static S *report;
static u32 nreport, reportcap;
static Map bysource, staticset, brokenset;

static void add_report(S line) {
  if (nreport == reportcap) {
    u32 cap = reportcap ? reportcap * 2 : 256;
    S *r = bump(cap * sizeof(S));
    __builtin_memcpy(r, report, nreport * sizeof(S));
    report = r;
    reportcap = cap;
  }
  report[nreport++] = line;
}

// A line built in TMP.
static void tmp_begin(void) {
  TMP.len = 0;
  W = &TMP;
}
static S tmp_end(Buf *back) {
  S s = mk(bump(TMP.len + 1), TMP.len);
  __builtin_memcpy((u8 *)s.p, TMP.p, TMP.len);
  W = back;
  return s;
}

// ---------------------------------------------------------------- field names

enum {
  F_UNKNOWN,
  F_title, F_base_url, F_name, F_slug, F_url, F_count, F_date, F_date_long, F_summary, F_excerpt, F_reading_minutes,
  F_tags, F_content, F_toc, F_newer, F_older, F_is_post, F_level, F_id, F_html, F_number, F_total, F_newer_url,
  F_older_url, F_posts, F_index, F_first, F_last, F_length, F_site, F_recent, F_page, F_pager, F_tag, F_loop,
  F_COUNT
};
static const char *FIELD_NAMES[F_COUNT] = {
  "", "title", "base_url", "name", "slug", "url", "count", "date", "date_long", "summary", "excerpt", "reading_minutes",
  "tags", "content", "toc", "newer", "older", "is_post", "level", "id", "html", "number", "total", "newer_url",
  "older_url", "posts", "index", "first", "last", "length", "site", "recent", "page", "pager", "tag", "loop",
};

static u32 field_id(S name) {
  for (u32 f = 1; f < F_COUNT; f++) {
    const char *c = FIELD_NAMES[f];
    u32 k = 0;
    while (c[k] && k < name.n && (u8)c[k] == name.p[k]) k++;
    if (!c[k] && k == name.n) return f;
  }
  return F_UNKNOWN;
}

// ---------------------------------------------------------------- templates: the reference's parser

static S tpl_error;
static S tpl_name;

static void tpl_fail(S msg) {
  // template "<name>": <msg>
  tmp_begin();
  LIT("template \"");
  puts_(tpl_name);
  LIT("\": ");
  puts_(msg);
  tpl_error = tmp_end(&OUT);
}

static int is_id_start(u8 c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
static int is_id_char(u8 c) { return is_id_start(c) || (c >= '0' && c <= '9'); }

static int is_path(S t) {
  u32 k = 0;
  for (;;) {
    if (k >= t.n || !is_id_start(t.p[k])) return 0;
    k++;
    while (k < t.n && is_id_char(t.p[k])) k++;
    if (k == t.n) return 1;
    if (t.p[k] != '.') return 0;
    k++;
  }
}

// JSON.stringify(t), for the "is not a path" message.
static S json_quote(S t) {
  Buf b = {bump(t.n * 6 + 8), 0, t.n * 6 + 8};
  Buf *back = W;
  W = &b;
  LIT("\"");
  for (u32 k = 0; k < t.n; k++) {
    u8 c = t.p[k];
    if (c == '"' || c == '\\') {
      u8 e[2] = {'\\', c};
      put(e, 2);
    } else if (c < 0x20) {
      if (c == '\n') LIT("\\n");
      else if (c == '\t') LIT("\\t");
      else if (c == '\r') LIT("\\r");
      else if (c == 8) LIT("\\b");
      else if (c == 12) LIT("\\f");
      else {
        u8 e[6] = {'\\', 'u', '0', '0', HEX[c >> 4], HEX[c & 15]};
        put(e, 6);
      }
    } else
      put(&c, 1);
  }
  LIT("\"");
  W = back;
  return mk(b.p, b.len);
}

// The path `text` names (its index in paths), or NONE with the error set.
static u32 path_of(S text) {
  if (!is_path(text)) {
    S q = json_quote(text);
    tmp_begin();
    puts_(q);
    LIT(" is not a path");
    tpl_fail(tmp_end(&OUT));
    return NONE;
  }
  u32 n = 1;
  for (u32 k = 0; k < text.n; k++) n += text.p[k] == '.';
  if (npaths == pathcap) {
    u32 cap = pathcap ? pathcap * 2 : 256;
    Path *p = bump(cap * sizeof(Path));
    __builtin_memcpy(p, paths, npaths * sizeof(Path));
    paths = p;
    pathcap = cap;
  }
  Path *p = &paths[npaths];
  p->n = n;
  p->names = bump(n * sizeof(S));
  p->ids = bump(n * 4);
  u32 a = 0, i = 0;
  for (u32 k = 0; k <= text.n; k++)
    if (k == text.n || text.p[k] == '.') {
      p->names[i] = mk(text.p + a, k - a);
      p->ids[i] = field_id(p->names[i]);
      i++;
      a = k + 1;
    }
  return npaths++;
}

static u32 new_node(u32 kind) {
  if (nnodes == nodecap) {
    u32 cap = nodecap ? nodecap * 2 : 1024;
    Node *n = bump(cap * sizeof(Node));
    __builtin_memcpy(n, nodes, nnodes * sizeof(Node));
    nodes = n;
    nodecap = cap;
  }
  Node *n = &nodes[nnodes];
  n->kind = kind;
  n->next = n->a = n->b = n->c = n->d = n->e = NONE;
  return nnodes++;
}

// A body under construction: where its next node links from.
typedef struct {
  u32 *head;  // index of the u32 holding the head (into nodes, as node index * 7 + field), or the root's
} Dummy;

static u32 root_head;
// Each open body is named by (node, which): which 0 = root, 1 = if's then, 2 = if's else, 3 = for's body.
typedef struct {
  u32 node, which, tail;
} Body;

static u32 *body_head(Body *b) {
  if (b->which == 0) return &root_head;
  Node *n = &nodes[b->node];
  return b->which == 1 ? &n->c : b->which == 2 ? &n->d : &n->d;
}

static void append(Body *b, u32 node) {
  if (b->tail == NONE) *body_head(b) = node;
  else nodes[b->tail].next = node;
  b->tail = node;
}

static void add_text(Body *b, const u8 *p, u32 n) {
  if (!n) return;
  u32 t = new_node(N_TEXT);
  // the text's JSON spelling
  Buf *back = W;
  Buf tb = {bump(n * 6 + 8), 0, n * 6 + 8};
  W = &tb;
  put_json(p, n);
  W = back;
  nodes[t].a = (u32)tb.p;
  nodes[t].b = tb.len;
  append(b, t);
}

static int blank_tab(const u8 *p, u32 a, u32 e) {
  for (u32 k = a; k < e; k++)
    if (p[k] != ' ' && p[k] != '\t') return 0;
  return 1;
}

static u32 find_from(S s, u32 from, const char *what, u32 wn) {
  for (u32 k = from; k + wn <= s.n; k++) {
    u32 q = 0;
    while (q < wn && s.p[k + q] == (u8)what[q]) q++;
    if (q == wn) return k;
  }
  return NONE;
}

static int all_nonspace(S s) {
  if (!s.n) return 0;
  return !has_ws(s.p, s.n);
}

// The tokens of a template (the reference's tokenize): text spans, {{ }} and {{{ }}} paths, {% %} tags.
enum { K_TEXT, K_VARTOK, K_RAWTOK, K_TAGTOK };
typedef struct {
  u32 k;
  S s;      // the text, or the tag (trimmed)
  u32 path; // a {{ }}'s
} Tok;
static Tok *toks;
static u32 ntoks, tokcap;

static void add_tok(u32 k, S s, u32 path) {
  if (k == K_TEXT && !s.n) return;
  if (ntoks == tokcap) {
    u32 cap = tokcap ? tokcap * 2 : 256;
    Tok *t = bump(cap * sizeof(Tok));
    __builtin_memcpy(t, toks, ntoks * sizeof(Tok));
    toks = t;
    tokcap = cap;
  }
  toks[ntoks].k = k;
  toks[ntoks].s = s;
  toks[ntoks].path = path;
  ntoks++;
}

static int tokenize(S src) {
  ntoks = 0;
  u32 ts = 0, te = 0;  // the pending text, always one span of src
  u32 i = 0;
  while (i < src.n) {
    u32 open = NONE;
    for (u32 k = i; k < src.n; k++)
      if (src.p[k] == '{') {
        open = k;
        break;
      }
    if (open == NONE || open == src.n - 1) {
      if (te == ts) ts = i;
      te = src.n;
      break;
    }
    u8 next = src.p[open + 1];
    if (next != '{' && next != '%') {
      if (te == ts) ts = i;
      te = open + 1;
      i = open + 1;
      continue;
    }
    if (te == ts) ts = i;
    te = open;
    if (next == '{') {
      int raw = open + 2 < src.n && src.p[open + 2] == '{';
      u32 close = raw ? find_from(src, open + 3, "}}}", 3) : find_from(src, open + 2, "}}", 2);
      if (close == NONE) {
        tpl_fail(raw ? SL("a {{{ is not closed") : SL("a {{ is not closed"));
        return 0;
      }
      add_tok(K_TEXT, mk(src.p + ts, te - ts), 0);
      u32 a = open + (raw ? 3 : 2);
      u32 pth = path_of(trimS(mk(src.p + a, close - a)));
      if (pth == NONE) return 0;
      add_tok(raw ? K_RAWTOK : K_VARTOK, mk(src.p, 0), pth);
      i = close + (raw ? 3 : 2);
      ts = te = i;
      continue;
    }
    u32 close = find_from(src, open + 2, "%}", 2);
    if (close == NONE) {
      tpl_fail(SL("a {% is not closed"));
      return 0;
    }
    u32 end = close + 2;
    u32 line_start = open;
    while (line_start > 0 && src.p[line_start - 1] != '\n') line_start--;
    u32 nl = NONE;
    for (u32 k = end; k < src.n; k++)
      if (src.p[k] == '\n') {
        nl = k;
        break;
      }
    u32 line_end = nl == NONE ? src.n : nl;
    int standalone = blank_tab(src.p, line_start, open) && blank_tab(src.p, end, line_end);
    if (standalone) te -= open - line_start;
    add_tok(K_TEXT, mk(src.p + ts, te - ts), 0);
    add_tok(K_TAGTOK, trimS(mk(src.p + open + 2, close - open - 2)), 0);
    i = standalone ? (nl == NONE ? src.n : nl + 1) : end;
    ts = te = i;
  }
  if (te > ts) add_tok(K_TEXT, mk(src.p + ts, te - ts), 0);
  return 1;
}

// Parses a template (the reference's parseTemplate): its head node in *head, or 0 with tpl_error set.
static int parse_template(S name, S src, u32 *head) {
  tpl_name = name;
  if (!tokenize(src)) return 0;
  root_head = NONE;
  Body stack[256];
  u32 sp = 0;
  stack[0].node = NONE;
  stack[0].which = 0;
  stack[0].tail = NONE;
  for (u32 q = 0; q < ntoks; q++) {
    Tok *tk = &toks[q];
    if (tk->k == K_TEXT) {
      add_text(&stack[sp], tk->s.p, tk->s.n);
      continue;
    }
    if (tk->k != K_TAGTOK) {
      u32 v = new_node(tk->k == K_RAWTOK ? N_RAW : N_VAR);
      nodes[v].a = tk->path;
      append(&stack[sp], v);
      continue;
    }
    S tag = tk->s;
    Body *top = &stack[sp];
    u32 topkind = top->which == 0 ? NONE : nodes[top->node].kind;
    if (starts(tag, SL("for "))) {
      u32 k = 4;
      if (k < tag.n && is_id_start(tag.p[k])) {
        u32 a = k++;
        while (k < tag.n && is_id_char(tag.p[k])) k++;
        S id = mk(tag.p + a, k - a);
        S rest = mk(tag.p + k, tag.n - k);
        if (starts(rest, SL(" in ")) && all_nonspace(mk(rest.p + 4, rest.n - 4))) {
          u32 pth = path_of(mk(rest.p + 4, rest.n - 4));
          if (pth == NONE) return 0;
          u32 f = new_node(N_FOR);
          nodes[f].a = pth;
          nodes[f].b = (u32)id.p;
          nodes[f].c = id.n;
          append(top, f);
          if (++sp == 256) TRAP();
          stack[sp].node = f;
          stack[sp].which = 3;
          stack[sp].tail = NONE;
          continue;
        }
      }
    }
    if (starts(tag, SL("if "))) {
      S rest = mk(tag.p + 3, tag.n - 3);
      int not = 0, match = 0;
      S pt = rest;
      if (starts(rest, SL("not ")) && all_nonspace(mk(rest.p + 4, rest.n - 4))) {
        not = 1;
        match = 1;
        pt = mk(rest.p + 4, rest.n - 4);
      } else if (all_nonspace(rest)) {
        match = 1;
        pt = rest;
      }
      if (match) {
        u32 pth = path_of(pt);
        if (pth == NONE) return 0;
        u32 f = new_node(N_IF);
        nodes[f].a = pth;
        nodes[f].b = not;
        nodes[f].e = 0;
        append(top, f);
        if (++sp == 256) TRAP();
        stack[sp].node = f;
        stack[sp].which = 1;
        stack[sp].tail = NONE;
        continue;
      }
    }
    if (seq(tag, SL("else"))) {
      if (topkind != N_IF || top->which != 1) {
        tpl_fail(SL("an else outside an if"));
        return 0;
      }
      nodes[top->node].e = 1;
      top->which = 2;
      top->tail = NONE;
      continue;
    }
    if (seq(tag, SL("endfor")) || seq(tag, SL("endif"))) {
      int isfor = tag.n == 6;
      if (topkind != (isfor ? (u32)N_FOR : (u32)N_IF)) {
        tpl_fail(isfor ? SL("an endfor with no open for") : SL("an endif with no open if"));
        return 0;
      }
      sp--;
      continue;
    }
    if (starts(tag, SL("include \"")) && tag.n > 10 && tag.p[tag.n - 1] == '"') {
      S nm = mk(tag.p + 9, tag.n - 10);
      int ok = 1;
      for (u32 k = 0; k < nm.n; k++) {
        u8 c = nm.p[k];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) ok = 0;
      }
      if (ok) {
        u32 f = new_node(N_INCLUDE);
        nodes[f].b = (u32)nm.p;
        nodes[f].c = nm.n;
        append(top, f);
        continue;
      }
    }
    tmp_begin();
    LIT("{% ");
    puts_(tag);
    LIT(" %} is not a tag");
    tpl_fail(tmp_end(&OUT));
    return 0;
  }
  if (sp > 0) {
    tpl_fail(nodes[stack[sp].node].kind == N_FOR ? SL("a for is not closed") : SL("a if is not closed"));
    return 0;
  }
  *head = root_head;
  return 1;
}

static u32 find_tpl(S name) {
  for (u32 t = 0; t < ntpl; t++)
    if (tpls[t].ok && seq(tpls[t].name, name)) return t;
  return NONE;
}

// ---------------------------------------------------------------- front matter

static const char *KEYS[7] = {"title", "date", "tags", "draft", "layout", "slug", "summary"};

static S fm_error;

static void fm_fail_key(const char *pre, S key, const char *post) {
  tmp_begin();
  put((const u8 *)pre, slen(pre));
  puts_(key);
  put((const u8 *)post, slen(post));
  fm_error = tmp_end(&OUT);
}

static int days_in(u32 y, u32 m) {
  if (m == 2) return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0 ? 29 : 28;
  return m == 4 || m == 6 || m == 9 || m == 11 ? 30 : 31;
}

// A string value: "..." with \" and \\ its only escapes, or bare. 1 with *out, or 0 with fm_error.
static int string_of(S key, S raw, S *out) {
  if (raw.n && raw.p[0] == '"') {
    if (raw.n < 2 || raw.p[raw.n - 1] != '"') {
      fm_fail_key("key \"", key, "\": a quoted string is not closed");
      return 0;
    }
    u8 *o = bump(raw.n);
    u32 n = 0;
    for (u32 i = 1; i < raw.n - 1; i++) {
      u8 c = raw.p[i];
      if (c == '\\') {
        u8 x = raw.p[i + 1];
        if ((x != '"' && x != '\\') || i + 1 == raw.n - 1) {
          fm_fail_key("key \"", key, "\": a quoted string holds an escape other than \\\" or \\\\");
          return 0;
        }
        o[n++] = x;
        i++;
      } else if (c == '"') {
        fm_fail_key("key \"", key, "\": a quoted string holds a stray quote");
        return 0;
      } else
        o[n++] = c;
    }
    *out = mk(o, n);
    return 1;
  }
  if (raw.n && raw.p[0] == '[') {
    fm_fail_key("key \"", key, "\": a bare string opens on [");
    return 0;
  }
  *out = raw;
  return 1;
}

static int parse_front_matter(Src *s) {
  S t = s->text;
  u32 nl = NONE;
  for (u32 k = 0; k < t.n; k++)
    if (t.p[k] == '\n') {
      nl = k;
      break;
    }
  S first = mk(t.p, nl == NONE ? t.n : nl);
  if (!seq(first, SL("---"))) {
    fm_error = SL("no front matter");
    return 0;
  }
  // the closing line
  u32 at = nl + 1, close_at = NONE, body_at = 0;
  u32 lines_end = 0;
  while (nl != NONE) {
    u32 e = at;
    while (e < t.n && t.p[e] != '\n') e++;
    if (e - at == 3 && t.p[at] == '-' && t.p[at + 1] == '-' && t.p[at + 2] == '-') {
      close_at = at;
      body_at = e < t.n ? e + 1 : t.n;
      break;
    }
    if (e >= t.n) break;
    at = e + 1;
  }
  if (nl == NONE || close_at == NONE) {
    fm_error = SL("the front matter is not closed");
    return 0;
  }
  lines_end = close_at;
  int seen[7] = {0, 0, 0, 0, 0, 0, 0};
  u32 lineno = 1;
  for (u32 a = nl + 1; a < lines_end;) {
    u32 e = a;
    while (t.p[e] != '\n') e++;
    lineno++;
    S line = mk(t.p + a, e - a);
    a = e + 1;
    if (blank_tab(line.p, 0, line.n) || (line.n && line.p[0] == '#')) continue;
    // ^([a-z_]+):(?: (.*))?$
    u32 k = 0;
    while (k < line.n && ((line.p[k] >= 'a' && line.p[k] <= 'z') || line.p[k] == '_')) k++;
    int ok = k > 0 && k < line.n && line.p[k] == ':';
    S raw = mk(line.p, 0);
    if (ok) {
      if (k + 1 == line.n) raw = mk(line.p + k + 1, 0);
      else if (line.p[k + 1] == ' ') {
        raw = mk(line.p + k + 2, line.n - k - 2);
        if (line_sep(raw.p, raw.n)) ok = 0;
      } else
        ok = 0;
    }
    if (!ok) {
      tmp_begin();
      LIT("line ");
      put_num(lineno);
      LIT(" is not \"key: value\"");
      fm_error = tmp_end(&OUT);
      return 0;
    }
    S key = mk(line.p, k);
    while (raw.n && (raw.p[raw.n - 1] == ' ' || raw.p[raw.n - 1] == '\t')) raw.n--;
    u32 ki = 7;
    for (u32 q = 0; q < 7; q++)
      if (seq(key, mk((const u8 *)KEYS[q], slen(KEYS[q])))) ki = q;
    if (ki == 7) {
      fm_fail_key("unknown key \"", key, "\"");
      return 0;
    }
    if (seen[ki]) {
      fm_fail_key("key \"", key, "\" is given twice");
      return 0;
    }
    if (!raw.n) {
      fm_fail_key("key \"", key, "\" has no value");
      return 0;
    }
    seen[ki] = 1;
    switch (ki) {
      case 0:
        if (!string_of(key, raw, &s->title)) return 0;
        break;
      case 6:
        if (!string_of(key, raw, &s->summary)) return 0;
        s->has_summary = 1;
        break;
      case 1: {
        int good = raw.n == 10 && raw.p[4] == '-' && raw.p[7] == '-';
        for (u32 q = 0; good && q < 10; q++)
          if (q != 4 && q != 7 && (raw.p[q] < '0' || raw.p[q] > '9')) good = 0;
        if (good) {
          u32 y = (raw.p[0] - '0') * 1000 + (raw.p[1] - '0') * 100 + (raw.p[2] - '0') * 10 + (raw.p[3] - '0');
          u32 mo = (raw.p[5] - '0') * 10 + (raw.p[6] - '0');
          u32 d = (raw.p[8] - '0') * 10 + (raw.p[9] - '0');
          if (y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > (u32)days_in(y, mo)) good = 0;
        }
        if (!good) {
          fm_error = SL("key \"date\": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)");
          return 0;
        }
        s->date = raw;
        s->has_date = 1;
        break;
      }
      case 2: {
        if (raw.p[0] != '[' || raw.p[raw.n - 1] != ']') {
          fm_error = SL("key \"tags\": not a list [a, b]");
          return 0;
        }
        S inner = raw.n >= 2 ? mk(raw.p + 1, raw.n - 2) : mk(raw.p, 0);
        s->has_tags = 1;
        if (trimS(inner).n == 0) {
          s->ntags = 0;
          break;
        }
        u32 n = 1;
        for (u32 q = 0; q < inner.n; q++) n += inner.p[q] == ',';
        s->tags = bump(n * sizeof(S));
        s->ntags = 0;
        u32 a2 = 0;
        for (u32 q = 0; q <= inner.n; q++)
          if (q == inner.n || inner.p[q] == ',') {
            S tg = trimS(mk(inner.p + a2, q - a2));
            int good = tg.n > 0;
            for (u32 z = 0; z < tg.n; z++)
              if (tg.p[z] == '"' || tg.p[z] == '[' || tg.p[z] == ']') good = 0;
            if (!good) {
              fm_error = SL("key \"tags\": an empty tag, or one holding \" [ or ]");
              return 0;
            }
            s->tags[s->ntags++] = tg;
            a2 = q + 1;
          }
        break;
      }
      case 3:
        if (seq(raw, SL("true"))) s->draft = 1;
        else if (seq(raw, SL("false"))) s->draft = 0;
        else {
          fm_error = SL("key \"draft\": true or false");
          return 0;
        }
        break;
      case 4: {
        int good = 1;
        for (u32 q = 0; q < raw.n; q++) {
          u8 c = raw.p[q];
          if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) good = 0;
        }
        if (!good) {
          fm_error = SL("key \"layout\": not a template name");
          return 0;
        }
        s->layout = raw;
        s->has_layout = 1;
        break;
      }
      case 5: {
        // ^[a-z0-9]+(?:-[a-z0-9]+)*$, at most 64
        int good = raw.n <= 64 && raw.p[0] != '-' && raw.p[raw.n - 1] != '-';
        for (u32 q = 0; good && q < raw.n; q++) {
          u8 c = raw.p[q];
          if (c == '-') {
            if (raw.p[q + 1] == '-') good = 0;
          } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            good = 0;
        }
        if (!good) {
          fm_error = SL("key \"slug\": not a slug");
          return 0;
        }
        s->slug_key = raw;
        s->has_slug = 1;
        break;
      }
    }
  }
  if (!seen[0]) {
    fm_error = SL("no title");
    return 0;
  }
  s->body = mk(t.p + body_at, t.n - body_at);
  return 1;
}

static const char *MONTHS[12] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

static S long_date(S d) {
  tmp_begin();
  u32 day = (d.p[8] - '0') * 10 + (d.p[9] - '0');
  u32 mo = (d.p[5] - '0') * 10 + (d.p[6] - '0');
  put_num(day);
  LIT(" ");
  put((const u8 *)MONTHS[mo - 1], slen(MONTHS[mo - 1]));
  LIT(" ");
  put(d.p, 4);
  return tmp_end(&OUT);
}

// ---------------------------------------------------------------- Markdown (lib/markdown.js), into MDB

static Src *CUR;  // the source being rendered

static int is_alpha(u8 c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

static void broken(const u8 *t, u32 n) {
  tmp_begin();
  LIT("broken-link ");
  puts_(CUR->path);
  LIT(" -> ");
  put(t, n);
  S line = mk(TMP.p, TMP.len);
  W = &MDB;
  if (map_get(&brokenset, line) == NONE) {
    W = &TMP;
    line = tmp_end(&MDB);
    map_add(&brokenset, line, 0);
    add_report(line);
  }
}

// A link's href, written HTML- and JSON-escaped.
static void link(const u8 *t, u32 n) {
  if (is_alpha(t[0])) {
    u32 k = 1;
    while (k < n && (is_alpha(t[k]) || (t[k] >= '0' && t[k] <= '9') || t[k] == '+' || t[k] == '.' || t[k] == '-')) k++;
    if (k < n && t[k] == ':') {
      put_text(t, n);
      return;
    }
  }
  if (t[0] == '/' || t[0] == '#') {
    put_text(t, n);
    return;
  }
  u32 hash_at = 0;
  while (hash_at < n && t[hash_at] != '#') hash_at++;
  if (hash_at > 0) {
    static S seg[1024];
    u32 ns = 0;
    int ok = 1;
    S dir = CUR->dir;
    for (u32 a = 0; a < dir.n;) {
      u32 b = a;
      while (b < dir.n && dir.p[b] != '/') b++;
      if (ns == 1024) TRAP();
      seg[ns++] = mk(dir.p + a, b - a);
      a = b + 1;
      if (b == dir.n) break;
    }
    if (dir.n && dir.p[dir.n - 1] == '/') seg[ns++] = mk(dir.p + dir.n, 0);
    for (u32 a = 0; a <= hash_at;) {
      u32 b = a;
      while (b < hash_at && t[b] != '/') b++;
      u32 l = b - a;
      if (l == 0 || (l == 1 && t[a] == '.')) {
      } else if (l == 2 && t[a] == '.' && t[a + 1] == '.') {
        if (ns == 0) {
          ok = 0;
          break;
        }
        ns--;
      } else {
        if (ns == 1024) TRAP();
        seg[ns++] = mk(t + a, l);
      }
      a = b + 1;
    }
    if (ok) {
      static u8 pb[8192];
      u32 pl = 0;
      for (u32 s = 0; s < ns; s++) {
        if (pl + seg[s].n + 1 > sizeof pb) TRAP();
        if (s) pb[pl++] = '/';
        __builtin_memcpy(pb + pl, seg[s].p, seg[s].n);
        pl += seg[s].n;
      }
      S path = mk(pb, pl);
      u32 src = map_get(&bysource, path);
      if (src != NONE) {
        put_textS(srcs[src].url);
        put_text(t + hash_at, n - hash_at);
        return;
      }
      if (map_get(&staticset, path) != NONE) {
        put_textS(base);
        LIT("/");
        put_textS(path);
        put_text(t + hash_at, n - hash_at);
        return;
      }
    }
  }
  broken(t, n);
  W = &MDB;
  put_text(t, n);
}

static u8 SPECIAL[128];
static u8 ESCAPABLE[128];

static int escapable(const u8 *s, u32 j, u32 to) { return j < to && s[j] < 128 && ESCAPABLE[s[j]]; }

static u32 find1(const u8 *s, u8 ch, u32 from, u32 to) {
  for (u32 j = from; j < to; j++) {
    if (s[j] == '\\' && escapable(s, j + 1, to)) {
      j++;
      continue;
    }
    if (s[j] == ch) return j;
  }
  return NONE;
}

static u32 dstar(const u8 *s, u32 from, u32 to) {
  for (u32 j = from; j + 2 <= to; j++) {
    if (s[j] == '\\' && escapable(s, j + 1, to)) {
      j++;
      continue;
    }
    if (s[j] == '*' && s[j + 1] == '*') return j;
  }
  return NONE;
}

static u32 sstar(const u8 *s, u32 from, u32 to) {
  for (u32 j = from; j < to; j++) {
    u8 c = s[j];
    if (c == '\\' && escapable(s, j + 1, to)) {
      j++;
      continue;
    }
    if (c != '*') continue;
    if (j + 1 < to && s[j + 1] == '*') {
      j++;
      continue;
    }
    return j;
  }
  return NONE;
}

// s[i, to) rendered inline, written HTML- and JSON-escaped. One pass: each block of 16 bytes is stored as it is, and the
// write pointer moves past what holds nothing to escape and nothing that opens a code span, emphasis or a link; such a
// character is then handled on its own.
static void inl(const u8 *s, u32 i, u32 to) {
  ROOM((to - i) * 6 + 64);
  u8 *q = W->p + W->len;
  while (i < to) {
    v128_t v = wasm_v128_load(s + i);
    wasm_v128_store(q, v);
    u32 left = to - i;
    u32 b = classes(v, C_INLINE);
    if (left < 16) b &= (1u << left) - 1;
    if (!b) {
      u32 adv = left < 16 ? left : 16;
      q += adv;
      i += adv;
      continue;
    }
    u32 z = __builtin_ctz(b);
    q += z;
    i += z;
    u8 c = s[i];
    if (c == '\\') {
      if (escapable(s, i + 1, to)) {
        q = esc_or_copy(q, s[i + 1]);
        i += 2;
      } else {
        q = esc_text(q, c);
        i++;
      }
      continue;
    }
    if (c != '`' && c != '*' && c != '[') {
      q = esc_text(q, c);
      i++;
      continue;
    }
    // a code span, emphasis or a link: through put and the rest, which keep W->len
    W->len = (u32)(q - W->p);
    if (c == '`') {
      u32 j = i + 1;
      while (j < to && s[j] != '`') j++;
      if (j < to && j > i + 1) {
        LIT("<code>");
        put_text(s + i + 1, j - i - 1);
        LIT("</code>");
        i = j + 1;
      } else {
        LIT("`");
        i++;
      }
    } else if (c == '*') {
      if (i + 1 < to && s[i + 1] == '*') {
        u32 j = dstar(s, i + 2, to);
        if (j != NONE && j > i + 2) {
          LIT("<strong>");
          inl(s, i + 2, j);
          LIT("</strong>");
          i = j + 2;
        } else {
          LIT("**");
          i += 2;
        }
      } else {
        u32 j = sstar(s, i + 1, to);
        if (j != NONE && j > i + 1) {
          LIT("<em>");
          inl(s, i + 1, j);
          LIT("</em>");
          i = j + 1;
        } else {
          LIT("*");
          i++;
        }
      }
    } else {
      u32 kk = find1(s, ']', i + 1, to);
      u32 m = NONE;
      if (kk != NONE && kk + 1 < to && s[kk + 1] == '(') {
        m = kk + 2;
        while (m < to && s[m] != ')') m++;
        if (m == to) m = NONE;
      }
      if (m != NONE && m > kk + 2 && !has_ws(s + kk + 2, m - kk - 2)) {
        LIT("<a href=\\\"");
        link(s + kk + 2, m - kk - 2);
        LIT("\\\">");
        inl(s, i + 1, kk);
        LIT("</a>");
        i = m + 1;
      } else {
        LIT("[");
        i++;
      }
    }
    ROOM((to - i) * 6 + 64);
    q = W->p + W->len;
  }
  W->len = (u32)(q - W->p);
}

static const u8 *B;
static u32 *LINES, linecap;
static u32 nlines;
#define LS(k) (LINES[k])
#define LE(k) (LINES[(k) + 1] - 1)

static int blank(u32 a, u32 e) { return blank_tab(B, a, e); }

static int fence_open(u32 a, u32 e, u32 *la, u32 *le) {
  if (e - a < 3 || B[a] != '`' || B[a + 1] != '`' || B[a + 2] != '`') return 0;
  u32 k = a + 3;
  while (k < e) {
    u8 c = B[k];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-')) break;
    k++;
  }
  if (!blank(k, e)) return 0;
  *la = a + 3;
  *le = k;
  return 1;
}

static int fence_close(u32 a, u32 e) { return e - a >= 3 && B[a] == '`' && B[a + 1] == '`' && B[a + 2] == '`' && blank(a + 3, e); }

static int is_hr(u32 a, u32 e) {
  if (e - a < 3) return 0;
  u8 c = B[a];
  return (c == '-' || c == '*') && B[a + 1] == c && B[a + 2] == c && blank(a + 3, e);
}

static u32 heading(u32 a, u32 e, u32 *ta, u32 *te) {
  u32 n = 0;
  while (a + n < e && B[a + n] == '#') n++;
  if (n == 0 || n > 6) return 0;
  u32 i = a + n;
  while (i < e && (B[i] == ' ' || B[i] == '\t')) i++;
  if (i == a + n) return 0;
  u32 z = e;
  while (z > i && (B[z - 1] == ' ' || B[z - 1] == '\t')) z--;
  if (z == i) return 0;
  *ta = i;
  *te = z;
  return n;
}

static int ends_paragraph(u32 a, u32 e) {
  u32 x, y;
  return blank(a, e) || fence_open(a, e, &x, &y) || heading(a, e, &x, &y) || is_hr(a, e);
}

static void trim(u32 *a, u32 *e) {
  while (*a < *e && (B[*a] == ' ' || B[*a] == '\t')) (*a)++;
  while (*e > *a && (B[*e - 1] == ' ' || B[*e - 1] == '\t')) (*e)--;
}

static int is_bullet(u32 a, u32 e) { return e - a >= 2 && (B[a] == '-' || B[a] == '*') && B[a + 1] == ' '; }

static u32 ordered_at(u32 a, u32 e) {
  u32 k = a;
  while (k < e && B[k] >= '0' && B[k] <= '9') k++;
  u32 n = k - a;
  if (n == 0 || n > 9 || k + 1 >= e || B[k] != '.' || B[k + 1] != ' ') return 0;
  return n;
}

static int continued(u32 a, u32 e) {
  u32 k = a;
  while (k < e && B[k] == ' ') k++;
  if (k - a < 2 || k >= e) return 0;
  u32 l;
  return !ws_at(B + k, e - k, &l);
}

static u8 *T;
static u32 tl, tcap;
static void tput(u32 a, u32 e) {
  if (tl + (e - a) + 17 > tcap) TRAP();
  copy16(T + tl, B + a, e - a);
  tl += e - a;
}

static void add_toc(u32 level, S id, u32 off, u32 len) {
  if (ntocs == toccap) {
    u32 cap = toccap ? toccap * 2 : 1024;
    Toc *t = bump(cap * sizeof(Toc));
    __builtin_memcpy(t, tocs, ntocs * sizeof(Toc));
    tocs = t;
    toccap = cap;
  }
  tocs[ntocs].level = level;
  tocs[ntocs].id = id;
  tocs[ntocs].html_off = off;
  tocs[ntocs].html_len = len;
  ntocs++;
}

static void render_markdown(Src *s) {
  CUR = s;
  W = &MDB;
  B = s->body.p;
  u32 len = s->body.n;
  s->reading = (words(B, len) + 199) / 200;
  if (s->reading < 1) s->reading = 1;
  s->ex_off = NONE;
  s->toc0 = ntocs;
  u32 html0 = MDB.len;
  S ids[512];
  u32 nids = 0;

  if (len + 2 > linecap) {
    linecap = len + 64;
    LINES = bump(linecap * 4);
  }
  if (len * 2 + 64 > tcap) {
    tcap = len * 2 + 64;
    T = bump(tcap);
  }
  nlines = 0;
  LINES[nlines++] = 0;
  {
    u32 k = 0, nl = nlines;
    u32 *L = LINES;
    for (; k + 16 <= len; k += 16) {
      u32 b = BITS(EQ(wasm_v128_load(B + k), '\n'));
      while (b) {
        L[nl++] = k + __builtin_ctz(b) + 1;
        b &= b - 1;
      }
    }
    for (; k < len; k++)
      if (B[k] == '\n') L[nl++] = k + 1;
    nlines = nl;
  }
  LINES[nlines] = len + 1;

  u32 i = 0;
  while (i < nlines) {
    u32 a = LS(i), e = LE(i);
    if (blank(a, e)) {
      i++;
      continue;
    }
    u32 x, y;
    if (fence_open(a, e, &x, &y)) {
      i++;
      if (x == y)
        LIT("<pre><code>");
      else {
        LIT("<pre><code class=\\\"language-");
        put_text(B + x, y - x);
        LIT("\\\">");
      }
      while (i < nlines && !fence_close(LS(i), LE(i))) {
        put_text(B + LS(i), LE(i) - LS(i));
        LIT("\\n");
        i++;
      }
      i++;
      LIT("</code></pre>\\n");
      continue;
    }
    u32 depth = heading(a, e, &x, &y);
    if (depth) {
      Buf *back = W;
      S sl = slugify(mk(B + x, y - x));
      S id = sl;
      for (u32 n = 2;; n++) {
        int taken = 0;
        for (u32 q = 0; q < nids && !taken; q++) taken = seq(ids[q], id);
        if (!taken) break;
        S suf = numS(n);
        tmp_begin();
        puts_(sl);
        LIT("-");
        puts_(suf);
        id = tmp_end(back);
      }
      W = back;
      if (nids == 512) TRAP();
      ids[nids++] = id;
      LIT("<h");
      put_num(depth);
      LIT(" id=\\\"");
      puts_(id);
      LIT("\\\">");
      u32 h0 = MDB.len;
      inl(B, x, y);
      u32 h1 = MDB.len;
      LIT("</h");
      put_num(depth);
      LIT(">\\n");
      if (depth == 2 || depth == 3) add_toc(depth, id, h0, h1 - h0);
      i++;
      continue;
    }
    if (is_hr(a, e)) {
      LIT("<hr>\\n");
      i++;
      continue;
    }
    if (e > a && B[a] == '>') {
      tl = 0;
      int any = 0;
      while (i < nlines && LE(i) > LS(i) && B[LS(i)] == '>') {
        u32 la = LS(i) + 1, le = LE(i);
        if (la < le && B[la] == ' ') la++;
        trim(&la, &le);
        if (la < le) {
          if (any) T[tl++] = '\n';
          tput(la, le);
          any = 1;
        }
        i++;
      }
      if (any) {
        LIT("<blockquote>\\n<p>");
        inl(T, 0, tl);
        LIT("</p>\\n</blockquote>\\n");
      } else
        LIT("<blockquote>\\n</blockquote>\\n");
      continue;
    }
    int bullet = is_bullet(a, e);
    u32 od = bullet ? 0 : ordered_at(a, e);
    if (bullet || od) {
      if (bullet)
        LIT("<ul>\\n");
      else {
        u32 start = 0;
        for (u32 k = a; k < a + od; k++) start = start * 10 + (B[k] - '0');
        if (start != 1) {
          LIT("<ol start=\\\"");
          put_num(start);
          LIT("\\\">\\n");
        } else
          LIT("<ol>\\n");
      }
      int open = 0;
      while (i < nlines) {
        u32 la = LS(i), le = LE(i);
        u32 d = bullet ? (is_bullet(la, le) ? 2 : 0) : ordered_at(la, le);
        if (d) {
          if (open) {
            LIT("<li>");
            inl(T, 0, tl);
            LIT("</li>\\n");
          }
          tl = 0;
          la += bullet ? 2 : d + 2;
          trim(&la, &le);
          tput(la, le);
          open = 1;
        } else if (continued(la, le)) {
          trim(&la, &le);
          T[tl++] = '\n';
          tput(la, le);
        } else
          break;
        i++;
      }
      LIT("<li>");
      inl(T, 0, tl);
      LIT("</li>\\n");
      if (bullet)
        LIT("</ul>\\n");
      else
        LIT("</ol>\\n");
      continue;
    }
    // A paragraph: this line and the next ones up to a blank line, a fence, a heading or a thematic break, each
    // trimmed of spaces and tabs and joined by "\n". When no line has any to trim, that is the body's own span.
    u32 first = i;
    int clean = 1;
    i++;
    while (i < nlines && !ends_paragraph(LS(i), LE(i))) i++;
    for (u32 k = first; k < i && clean; k++) {
      u8 x0 = B[LS(k)], x1 = B[LE(k) - 1];
      clean = x0 != ' ' && x0 != '\t' && x1 != ' ' && x1 != '\t';
    }
    const u8 *ps = B;
    u32 pa = LS(first), pe = LE(i - 1);
    if (!clean) {
      tl = 0;
      for (u32 k = first; k < i; k++) {
        u32 la = LS(k), le = LE(k);
        trim(&la, &le);
        if (k > first) T[tl++] = '\n';
        tput(la, le);
      }
      ps = T;
      pa = 0;
      pe = tl;
    }
    LIT("<p>");
    u32 p0 = MDB.len;
    inl(ps, pa, pe);
    u32 p1 = MDB.len;
    LIT("</p>\\n");
    if (s->ex_off == NONE) {
      s->ex_off = p0;
      s->ex_len = p1 - p0;
    }
  }
  s->html_off = html0;
  s->html_len = MDB.len - html0;
  s->ntoc = ntocs - s->toc0;
}

// ---------------------------------------------------------------- template values

enum { T_UNDEF, T_NULL, T_BOOL, T_NUM, T_STR, T_STRJ, T_OBJ, T_ARR };
enum { K_SITE, K_TAGITEM, K_CARD, K_PAGE, K_LINK, K_TAGREF, K_TOC, K_PAGER, K_TAGPAGE, K_LOOP };
enum { A_TAGS, A_CARDS, A_TAGREFS, A_TOC };

typedef struct {
  u32 t, k, a, b;
} V;

static V vmk(u32 t, u32 k, u32 a, u32 b) {
  V v = {t, k, a, b};
  return v;
}
static const V UNDEF = {T_UNDEF, 0, 0, 0};
static V vstr(S s) { return vmk(T_STR, 0, (u32)s.p, s.n); }
static V vnum(u32 n) { return vmk(T_NUM, 0, n, 0); }
static V vbool(int b) { return vmk(T_BOOL, 0, b != 0, 0); }
static V vmd(u32 off, u32 len) { return vmk(T_STRJ, 0, off, len); }  // a span of MDB

// The pager of index page k (1-based) of `pages`.
static u32 pages_total;
static S index_url(u32 k) {
  if (k == 1) return cat(base, SL("/"));
  tmp_begin();
  puts_(base);
  LIT("/page/");
  put_num(k);
  LIT("/");
  return tmp_end(&OUT);
}

static S EMPTY = {(const u8 *)"", 0};

static V card_field(Src *s, u32 si, u32 f, int page) {
  switch (f) {
    case F_title: return vstr(s->title);
    case F_url: return vstr(s->url);
    case F_date: return vstr(s->has_date ? s->date : EMPTY);
    case F_date_long: return vstr(s->has_date ? s->date_long : EMPTY);
    case F_summary: return vstr(s->has_summary ? s->summary : EMPTY);
    case F_excerpt: return s->ex_off == NONE ? vstr(EMPTY) : vmd(s->ex_off, s->ex_len);
    case F_reading_minutes: return vnum(s->reading);
    case F_tags: return vmk(T_ARR, A_TAGREFS, (u32)s->refs, s->nrefs);
  }
  if (!page) return UNDEF;
  switch (f) {
    case F_slug: return vstr(s->slug);
    case F_content: return vmd(s->html_off, s->html_len);
    case F_toc: return vmk(T_ARR, A_TOC, s->toc0, s->ntoc);
    case F_newer: return s->newer == NONE ? vmk(T_NULL, 0, 0, 0) : vmk(T_OBJ, K_LINK, s->newer, 0);
    case F_older: return s->older == NONE ? vmk(T_NULL, 0, 0, 0) : vmk(T_OBJ, K_LINK, s->older, 0);
    case F_is_post: return vbool(s->post);
  }
  (void)si;
  return UNDEF;
}

static V get(V v, u32 f) {
  if (v.t == T_ARR) return f == F_length ? vnum(v.b) : UNDEF;
  if (v.t != T_OBJ) return UNDEF;
  switch (v.k) {
    case K_SITE:
      if (f == F_title) return vstr(site_title);
      if (f == F_base_url) return vstr(base);
      return UNDEF;
    case K_TAGITEM:
    case K_TAGREF:
    case K_TAGPAGE: {
      Tag *t = &tags[v.a];
      if (f == F_name) return vstr(t->name);
      if (f == F_slug) return vstr(t->slug);
      if (f == F_url) return vstr(t->url);
      if (v.k == K_TAGREF) return UNDEF;
      if (f == F_count) return vnum(t->nposts);
      if (v.k == K_TAGPAGE && f == F_posts) return vmk(T_ARR, A_CARDS, (u32)t->posts, t->nposts);
      return UNDEF;
    }
    case K_CARD: return card_field(&srcs[v.a], v.a, f, 0);
    case K_PAGE: return card_field(&srcs[v.a], v.a, f, 1);
    case K_LINK: {
      Src *s = &srcs[v.a];
      if (f == F_title || f == F_url || f == F_date || f == F_date_long) return card_field(s, v.a, f, 0);
      return UNDEF;
    }
    case K_TOC: {
      Toc *t = &tocs[v.a];
      if (f == F_level) return vnum(t->level);
      if (f == F_id) return vstr(t->id);
      if (f == F_html) return vmd(t->html_off, t->html_len);
      return UNDEF;
    }
    case K_PAGER: {
      u32 k = v.a;
      if (f == F_number) return vnum(k);
      if (f == F_total) return vnum(pages_total);
      if (f == F_url) return vstr(index_url(k));
      if (f == F_newer_url) return vstr(k == 1 ? EMPTY : index_url(k - 1));
      if (f == F_older_url) return vstr(k == pages_total ? EMPTY : index_url(k + 1));
      return UNDEF;
    }
    case K_LOOP:
      if (f == F_index) return vnum(v.a + 1);
      if (f == F_first) return vbool(v.a == 0);
      if (f == F_last) return vbool(v.a + 1 == v.b);
      if (f == F_length) return vnum(v.b);
      return UNDEF;
  }
  return UNDEF;
}

static int truthy(V v) {
  switch (v.t) {
    case T_BOOL: return v.a;
    case T_NUM: return v.a != 0;
    case T_STR:
    case T_STRJ: return v.b != 0;
    case T_ARR: return v.b != 0;
    case T_OBJ: return 1;
  }
  return 0;
}

static void emit(V v, int raw) {
  switch (v.t) {
    case T_STR:
      if (raw) put_json((const u8 *)v.a, v.b);
      else put_text((const u8 *)v.a, v.b);
      return;
    case T_STRJ:
      if (raw) put(MDB.p + v.a, v.b);
      else put_unjson_text(MDB.p + v.a, v.b);
      return;
    case T_NUM: put_num(v.a); return;
    case T_BOOL:
      if (v.a) LIT("true");
      else LIT("false");
      return;
    case T_ARR:
      for (u32 k = 1; k < v.b; k++) LIT(", ");
      return;
  }
}

// The scope: the render's own names, then the loops' frames.
typedef struct {
  S name;
  V x, loop;
} Frame;
static Frame frames[64];
static u32 nframes;
static V top_site, top_tags, top_recent, top_page, top_posts, top_pager, top_tag;
static int has_page, has_posts, has_pager, has_tag;

static V lookup(u32 pi) {
  Path *p = &paths[pi];
  V v;
  S n0 = p->names[0];
  u32 id0 = p->ids[0];
  int found = 0;
  for (u32 f = nframes; f-- > 0;) {
    if (id0 == F_loop) {
      v = frames[f].loop;
      found = 1;
      break;
    }
    if (seq(frames[f].name, n0)) {
      v = frames[f].x;
      found = 1;
      break;
    }
  }
  if (!found) {
    switch (id0) {
      case F_site: v = top_site; break;
      case F_tags: v = top_tags; break;
      case F_recent: v = top_recent; break;
      case F_page: v = has_page ? top_page : UNDEF; break;
      case F_posts: v = has_posts ? top_posts : UNDEF; break;
      case F_pager: v = has_pager ? top_pager : UNDEF; break;
      case F_tag: v = has_tag ? top_tag : UNDEF; break;
      default: v = UNDEF;
    }
  }
  for (u32 k = 1; k < p->n && v.t != T_UNDEF; k++) v = get(v, p->ids[k]);
  return v;
}

static V element(V arr, u32 k) {
  switch (arr.k) {
    case A_TAGS: return vmk(T_OBJ, K_TAGITEM, sorted_tags[k], 0);
    case A_CARDS: return vmk(T_OBJ, K_CARD, ((u32 *)arr.a)[k], 0);
    case A_TAGREFS: return vmk(T_OBJ, K_TAGREF, ((u32 *)arr.a)[k], 0);
    case A_TOC: return vmk(T_OBJ, K_TOC, arr.a + k, 0);
  }
  return UNDEF;
}

// A node rendered at the top of a page's scope (no loop open) whose paths name only site, tags and recent, or names its
// own loops bind, or names no render has (undefined everywhere), writes the same bytes on every page of the site: its
// first rendering is kept and copied after that. Which nodes are so is worked out once per node and include depth.
static u8 *cflag;          // per node and depth: 0 not yet known, 1 the same everywhere, 2 not
static u32 *coff, *clen;   // its bytes in CB, once rendered (coff NONE before)
static Buf CB;

static int body_const(u32 n, u32 depth, const S *bound, u32 nb);
static int loop_ok;  // whether `loop` reads a loop of the subtree's own (or no loop at all), not the one around it

static int path_const(u32 pi, const S *bound, u32 nb) {
  Path *p = &paths[pi];
  if (p->ids[0] == F_loop) return loop_ok;
  for (u32 k = 0; k < nb; k++)
    if (seq(bound[k], p->names[0])) return 1;
  u32 id = p->ids[0];
  return !(id == F_page || id == F_posts || id == F_pager || id == F_tag);
}

static int node_const(u32 n, u32 depth, const S *bound, u32 nb) {
  Node *d = &nodes[n];
  switch (d->kind) {
    case N_TEXT: return 1;
    case N_VAR:
    case N_RAW: return path_const(d->a, bound, nb);
    case N_IF: return path_const(d->a, bound, nb) && body_const(d->c, depth, bound, nb) && (d->e != 1 || body_const(d->d, depth, bound, nb));
    case N_FOR: {
      if (!path_const(d->a, bound, nb) || nb == 32) return 0;
      S inner[33];
      for (u32 k = 0; k < nb; k++) inner[k] = bound[k];
      inner[nb] = mk((const u8 *)d->b, d->c);
      int was = loop_ok;
      loop_ok = 1;
      int r = body_const(d->d, depth, inner, nb + 1);
      loop_ok = was;
      return r;
    }
    case N_INCLUDE: return d->a == NONE || depth >= 8 || body_const(tpls[d->a].head, depth + 1, bound, nb);
  }
  return 0;
}

static int body_const(u32 n, u32 depth, const S *bound, u32 nb) {
  for (; n != NONE; n = nodes[n].next)
    if (!node_const(n, depth, bound, nb)) return 0;
  return 1;
}

static void render(u32 n, u32 depth);

static void render_node(u32 n, u32 depth) {
  Node *d = &nodes[n];
  switch (d->kind) {
    case N_TEXT: put((const u8 *)d->a, d->b); break;
    case N_VAR: emit(lookup(d->a), 0); break;
    case N_RAW: emit(lookup(d->a), 1); break;
    case N_IF: {
      int v = truthy(lookup(d->a));
      if (v != (int)d->b) render(d->c, depth);
      else if (d->e == 1) render(d->d, depth);
      break;
    }
    case N_FOR: {
      V xs = lookup(d->a);
      if (xs.t != T_ARR) break;
      if (nframes == 64) TRAP();
      Frame *fr = &frames[nframes++];
      fr->name = mk((const u8 *)d->b, d->c);
      for (u32 k = 0; k < xs.b; k++) {
        frames[nframes - 1].x = element(xs, k);
        frames[nframes - 1].loop = vmk(T_OBJ, K_LOOP, k, xs.b);
        render(nodes[n].d, depth);
      }
      nframes--;
      break;
    }
    case N_INCLUDE:
      if (d->a != NONE && depth < 8) render(tpls[d->a].head, depth + 1);
      break;
  }
}

// An include inside one loop whose template names only site, tags, recent and the loop's element (never `loop`, which
// moves with the index) writes the same bytes for the same element wherever it is: a card of a post, listed on the
// index and on each of its tags' pages. Kept by template, include depth, the element's name and the element.
typedef struct {
  u32 tpl, depth, k, a, off, len;
  S name;
} Card;
#define CARD_SLOTS 4096
static Card *cards;
static u32 ncards;
static S fname;    // the bound name iflag holds flags for
static u8 *iflag;  // per template and depth, for one bound name: 0 unknown, 1 cacheable, 2 not (reset per name)

static void render_node(u32 n, u32 depth);

// 1 when include node n was written from the cache or rendered and kept.
static int card_include(u32 n, u32 depth) {
  Node *d = &nodes[n];
  Frame *fr = &frames[0];
  if (d->a == NONE || depth >= 8 || fr->x.t != T_OBJ) return 0;
  u32 fi = (d->a * 9 + depth) * 2;
  // the flag is kept per template and depth, for the bound name it was worked out with
  if (!seq(fname, fr->name)) {
    __builtin_memset(iflag, 0, ntpl * 9 * 2);
    fname = fr->name;
  }
  if (!iflag[fi]) {
    loop_ok = 0;
    S bound[1] = {fr->name};
    iflag[fi] = body_const(tpls[d->a].head, depth + 1, bound, 1) ? 1 : 2;
  }
  if (iflag[fi] != 1) return 0;
  u32 h = (d->a * 31 + depth * 7 + fr->x.k * 131 + fr->x.a * 2654435761u) & (CARD_SLOTS - 1);
  for (;; h = (h + 1) & (CARD_SLOTS - 1)) {
    Card *c = &cards[h];
    if (c->len == NONE) break;
    if (c->tpl == d->a && c->depth == depth && c->k == fr->x.k && c->a == fr->x.a && seq(c->name, fr->name)) {
      put(CB.p + c->off, c->len);
      return 1;
    }
  }
  if (ncards * 2 > CARD_SLOTS) return 0;
  u32 start = W->len;
  render_node(n, depth);
  u32 len = W->len - start;
  if (CB.len + len + 64 > CB.cap) {
    Buf *back = W;
    W = &CB;
    grow(&CB, len + 64);
    W = back;
  }
  __builtin_memcpy(CB.p + CB.len, W->p + start, len);
  Card *c = &cards[h];
  c->tpl = d->a;
  c->depth = depth;
  c->k = fr->x.k;
  c->a = fr->x.a;
  c->name = fr->name;
  c->off = CB.len;
  c->len = len;
  ncards++;
  CB.len += len;
  return 1;
}

static void render(u32 n, u32 depth) {
  for (; n != NONE; n = nodes[n].next) {
    u32 kind = nodes[n].kind;
    if (nframes == 1 && kind == N_INCLUDE && card_include(n, depth)) continue;
    if (nframes || kind < N_IF) {
      render_node(n, depth);
      continue;
    }
    u32 ci = depth * nnodes + n;
    if (!cflag[ci]) {
      loop_ok = 1;
      cflag[ci] = node_const(n, depth, 0, 0) ? 1 : 2;
    }
    if (cflag[ci] == 2) {
      render_node(n, depth);
      continue;
    }
    if (coff[ci] != NONE) {
      put(CB.p + coff[ci], clen[ci]);
      continue;
    }
    u32 start = W->len;
    render_node(n, depth);
    u32 len = W->len - start;
    if (CB.len + len + 64 > CB.cap) {
      Buf *back = W;
      W = &CB;
      grow(&CB, len + 64);
      W = back;
    }
    __builtin_memcpy(CB.p + CB.len, W->p + start, len);
    coff[ci] = CB.len;
    clen[ci] = len;
    CB.len += len;
  }
}

// ---------------------------------------------------------------- the build of one case

static Map outkeys;

static void out_key(S path) {
  map_add(&outkeys, path, 0);
  LIT(",\"");
  put_jsonS(path);
  LIT("\":\"");
}

static void sort_idx(u32 *a, u32 n, int (*less)(u32, u32)) {
  // insertion sort for small runs, merge sort above
  if (n < 2) return;
  u32 *tmp = bump(n * 4);
  for (u32 w = 1; w < n; w *= 2) {
    for (u32 lo = 0; lo < n; lo += 2 * w) {
      u32 mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
      u32 i = lo, j = mid, k = lo;
      while (i < mid && j < hi) tmp[k++] = less(a[j], a[i]) ? a[j++] : a[i++];
      while (i < mid) tmp[k++] = a[i++];
      while (j < hi) tmp[k++] = a[j++];
    }
    __builtin_memcpy(a, tmp, n * 4);
  }
}

static int file_less(u32 x, u32 y) { return cmp16(files[x].path, files[y].path) < 0; }
static int post_less(u32 x, u32 y) {
  int c = cmp16(srcs[y].date, srcs[x].date);
  if (c) return c < 0;
  return cmp16(srcs[x].slug, srcs[y].slug) < 0;
}
static int tag_less(u32 x, u32 y) {
  if (tags[x].nposts != tags[y].nposts) return tags[x].nposts > tags[y].nposts;
  return cmp16(tags[x].slug, tags[y].slug) < 0;
}
static S *report_arr;
static int report_less(u32 x, u32 y) { return cmp16(report_arr[x], report_arr[y]) < 0; }

static Map slugcache;
static S slugs_cached[4096];
static u32 ncache;
static S tag_slug(S name) {
  u32 v = map_get(&slugcache, name);
  S *cache = slugs_cached;
  if (v != NONE) return cache[v];
  S s = slugify(name);
  if (ncache == 4096) TRAP();
  cache[ncache] = s;
  map_add(&slugcache, name, ncache++);
  return s;
}

static Map taken;
static S slug_in(S dir, S slug) {
  // uniqueSlug, keyed "dir\nslug"
  S s = slug;
  for (u32 n = 2;; n++) {
    tmp_begin();
    puts_(dir);
    LIT("\n");
    puts_(s);
    S key = tmp_end(&OUT);
    if (map_add(&taken, key, 0)) return s;
    tmp_begin();
    puts_(slug);
    LIT("-");
    put_num(n);
    s = tmp_end(&OUT);
  }
}

static void build_case(void) {


  // Sources.
  u32 *order = bump(nfiles * 4 + 4);
  for (u32 k = 0; k < nfiles; k++) order[k] = k;
  sort_idx(order, nfiles, file_less);
  srcs = zalloc(nfiles * sizeof(Src) + 16);
  nsrcs = 0;
  statics = bump(nfiles * 4 + 4);
  nstatics = 0;
  map_init(&staticset, nfiles);
  for (u32 k = 0; k < nfiles; k++) {
    File *f = &files[order[k]];
    if (ends(f->path, SL(".md"))) {
      Src *s = &srcs[nsrcs++];
      s->path = f->path;
      // \r\n and lone \r made \n
      S c = f->content;
      int cr = 0;
      {
        u32 q = 0;
        for (; q + 16 <= c.n && !cr; q += 16) cr = BITS(EQ(wasm_v128_load(c.p + q), '\r')) != 0;
        for (; q < c.n; q++)
          if (c.p[q] == '\r') cr = 1;
      }
      if (cr) {
        u8 *o = bump(c.n + 1);
        u32 n = 0;
        for (u32 q = 0; q < c.n; q++) {
          if (c.p[q] == '\r') {
            o[n++] = '\n';
            if (q + 1 < c.n && c.p[q + 1] == '\n') q++;
          } else
            o[n++] = c.p[q];
        }
        c = mk(o, n);
      }
      s->text = c;
    } else {
      statics[nstatics++] = order[k];
      map_add(&staticset, f->path, order[k]);
    }
  }

  // Front matter.
  kept = bump(nsrcs * 4 + 4);
  nkept = 0;
  for (u32 k = 0; k < nsrcs; k++) {
    Src *s = &srcs[k];
    if (!parse_front_matter(s)) {
      tmp_begin();
      LIT("refused ");
      puts_(s->path);
      LIT(": ");
      puts_(fm_error);
      add_report(tmp_end(&OUT));
      continue;
    }
    s->post = starts(s->path, SL("posts/"));
    const char *own = starts(s->path, SL("tags/")) ? "tags" : starts(s->path, SL("page/")) ? "page" : 0;
    if (own) {
      tmp_begin();
      LIT("refused ");
      puts_(s->path);
      LIT(": ");
      put((const u8 *)own, 4);
      LIT("/ is the generator's own");
      add_report(tmp_end(&OUT));
      continue;
    }
    if (s->post && !s->has_date) {
      tmp_begin();
      LIT("refused ");
      puts_(s->path);
      LIT(": a post needs a date");
      add_report(tmp_end(&OUT));
      continue;
    }
    S layout = s->has_layout ? s->layout : s->post ? SL("post") : SL("page");
    s->tpl = find_tpl(layout);
    if (s->tpl == NONE) {
      tmp_begin();
      LIT("refused ");
      puts_(s->path);
      LIT(": layout \"");
      puts_(layout);
      LIT("\" is no template that parses");
      add_report(tmp_end(&OUT));
      continue;
    }
    if (s->draft) {
      tmp_begin();
      LIT("draft ");
      puts_(s->path);
      add_report(tmp_end(&OUT));
      continue;
    }
    if (line_sep(s->body.p, s->body.n)) TRAP();
    kept[nkept++] = k;
  }

  // Permalinks.
  map_init(&taken, nkept + 8);
  map_init(&bysource, nkept);
  {
    const char *res[3] = {"posts", "tags", "page"};
    for (u32 r = 0; r < 3; r++) {
      tmp_begin();
      LIT("\n");
      put((const u8 *)res[r], slen(res[r]));
      map_add(&taken, tmp_end(&OUT), 0);
    }
  }
  for (u32 k = 0; k < nkept; k++) {
    Src *s = &srcs[kept[k]];
    u32 slash = NONE;
    for (u32 q = 0; q < s->path.n; q++)
      if (s->path.p[q] == '/') slash = q;
    s->dir = slash == NONE ? EMPTY : mk(s->path.p, slash);
    S dir = s->post ? SL("posts") : s->dir;
    s->slug = slug_in(dir, s->has_slug ? s->slug_key : slugify(s->title));
    s->outdir = dir.n ? cat(cat(dir, SL("/")), s->slug) : s->slug;
    s->url = cat(cat(cat(base, SL("/")), s->outdir), SL("/"));
    if (s->has_date) s->date_long = long_date(s->date);
    s->newer = s->older = NONE;
    map_add(&bysource, s->path, kept[k]);
  }

  // Markdown.
  MDB.len = 0;
  ntocs = 0;
  map_init(&brokenset, 64);
  for (u32 k = 0; k < nkept; k++) render_markdown(&srcs[kept[k]]);
  W = &OUT;

  // Collections.
  posts = bump(nkept * 4 + 4);
  nposts = 0;
  for (u32 k = 0; k < nkept; k++)
    if (srcs[kept[k]].post) posts[nposts++] = kept[k];
  sort_idx(posts, nposts, post_less);
  for (u32 k = 0; k < nposts; k++) {
    srcs[posts[k]].newer = k == 0 ? NONE : posts[k - 1];
    srcs[posts[k]].older = k + 1 == nposts ? NONE : posts[k + 1];
  }

  // Tags.
  Map byslug;
  map_init(&byslug, 64);
  map_init(&slugcache, 64);
  ncache = 0;
  u32 tagcap = 64;
  tags = bump(tagcap * sizeof(Tag));
  ntags_all = 0;
  // the posts' tags first, then a page's tags no post has (url "")
  for (int pass = 0; pass < 2; pass++) {
    u32 n = pass == 0 ? nposts : nkept;
    for (u32 k = 0; k < n; k++) {
      Src *s = pass == 0 ? &srcs[posts[k]] : &srcs[kept[k]];
      if ((pass == 0) != (s->post != 0)) continue;
      s->refs = bump(s->ntags * 4 + 4);
      s->nrefs = 0;
      for (u32 q = 0; q < s->ntags; q++) {
        S slug = tag_slug(s->tags[q]);
        int dup = 0;
        for (u32 z = 0; z < s->nrefs && !dup; z++) dup = seq(tags[s->refs[z]].slug, slug);
        // dedupe by slug within the source (the first spelling kept): compare against earlier tags' slugs
        if (!dup)
          for (u32 z = 0; z < q && !dup; z++) dup = seq(tag_slug(s->tags[z]), slug);
        if (dup) continue;
        u32 t = map_get(&byslug, slug);
        if (t == NONE) {
          if (ntags_all == tagcap) {
            Tag *nt = bump(tagcap * 2 * sizeof(Tag));
            __builtin_memcpy(nt, tags, ntags_all * sizeof(Tag));
            tags = nt;
            tagcap *= 2;
          }
          t = ntags_all++;
          Tag *tg = &tags[t];
          tg->name = s->tags[q];
          tg->slug = slug;
          tg->nposts = 0;
          tg->cap = 0;
          tg->posts = 0;
          if (pass == 0) {
            tg->url = cat(cat(cat(base, SL("/tags/")), slug), SL("/"));
            map_add(&byslug, slug, t);
            ntags = ntags_all;
          } else
            tg->url = EMPTY;  // a pseudo tag, never listed, never shared
        }
        if (pass == 0) {
          Tag *tg = &tags[t];
          if (tg->nposts == tg->cap) {
            u32 cap = tg->cap ? tg->cap * 2 : 8;
            u32 *np = bump(cap * 4);
            if (tg->nposts) __builtin_memcpy(np, tg->posts, tg->nposts * 4);
            tg->posts = np;
            tg->cap = cap;
          }
          tg->posts[tg->nposts++] = posts[k];
        }
        s->refs[s->nrefs++] = t;
      }
    }
  }
  ntags = byslug.n;
  sorted_tags = bump(ntags * 4 + 4);
  {
    u32 n = 0;
    for (u32 t = 0; t < ntags_all; t++)
      if (tags[t].url.n) sorted_tags[n++] = t;
    sort_idx(sorted_tags, n, tag_less);
  }

  // Layouts.
  top_site = vmk(T_OBJ, K_SITE, 0, 0);
  top_tags = vmk(T_ARR, A_TAGS, 0, ntags);
  top_recent = vmk(T_ARR, A_CARDS, (u32)posts, recent_n < nposts ? recent_n : nposts);
  nframes = 0;
  cflag = zalloc(nnodes * 9 + 16);
  coff = bump(nnodes * 9 * 4 + 16);
  __builtin_memset(coff, 0xff, nnodes * 9 * 4);
  clen = bump(nnodes * 9 * 4 + 16);
  CB.len = 0;
  cards = bump(CARD_SLOTS * sizeof(Card));
  for (u32 q = 0; q < CARD_SLOTS; q++) cards[q].len = NONE;
  ncards = 0;
  fname = EMPTY;
  iflag = zalloc(ntpl * 9 * 2 + 16);
  map_init(&outkeys, nkept + 64);
  W = &OUT;
  LIT("{\"build-report.txt\":\"");
  u32 report_at = OUT.len;  // the report is written last, here: a placeholder
  LIT("\"");
  // the report goes in after everything else; keep its key first and patch below
  for (u32 k = 0; k < nkept; k++) {
    Src *s = &srcs[kept[k]];
    out_key(cat(s->outdir, SL("/index.html")));
    has_page = 1;
    has_posts = has_pager = has_tag = 0;
    top_page = vmk(T_OBJ, K_PAGE, kept[k], 0);
    render(tpls[s->tpl].head, 0);
    LIT("\"");
  }
  has_page = 0;
  u32 per = per_page;
  pages_total = nposts == 0 ? 1 : (nposts + per - 1) / per;
  u32 ti = find_tpl(SL("index"));
  if (ti != NONE)
    for (u32 k = 1; k <= pages_total; k++) {
      if (k == 1) out_key(SL("index.html"));
      else {
        tmp_begin();
        LIT("page/");
        put_num(k);
        LIT("/index.html");
        out_key(tmp_end(&OUT));
      }
      has_posts = has_pager = 1;
      u32 a = (k - 1) * per, b = k * per < nposts ? k * per : nposts;
      top_posts = vmk(T_ARR, A_CARDS, (u32)(posts + a), b > a ? b - a : 0);
      top_pager = vmk(T_OBJ, K_PAGER, k, 0);
      render(tpls[ti].head, 0);
      LIT("\"");
    }
  has_posts = has_pager = 0;
  ti = find_tpl(SL("tag"));
  if (ti != NONE)
    for (u32 q = 0; q < ntags; q++) {
      Tag *t = &tags[sorted_tags[q]];
      out_key(cat(cat(SL("tags/"), t->slug), SL("/index.html")));
      has_tag = 1;
      top_tag = vmk(T_OBJ, K_TAGPAGE, sorted_tags[q], 0);
      render(tpls[ti].head, 0);
      LIT("\"");
    }
  has_tag = 0;
  ti = find_tpl(SL("tags"));
  if (ti != NONE) {
    out_key(SL("tags/index.html"));
    render(tpls[ti].head, 0);
    LIT("\"");
  }
  for (u32 q = 0; q < nstatics; q++) {
    File *f = &files[statics[q]];
    if (seq(f->path, SL("build-report.txt"))) continue;  // the report is written over it
    if (map_get(&outkeys, f->path) != NONE) {
      tmp_begin();
      LIT("shadowed ");
      puts_(f->path);
      LIT(": a built page has its path");
      add_report(tmp_end(&OUT));
      continue;
    }
    out_key(f->path);
    put(J + f->raw_a, f->raw_b - f->raw_a);
    LIT("\"");
  }

  // The report: every line sorted, then the counts. Written into the placeholder by moving the tail.
  u32 *ord = bump(nreport * 4 + 4);
  for (u32 k = 0; k < nreport; k++) ord[k] = k;
  report_arr = report;
  sort_idx(ord, nreport, report_less);
  Buf rb = {bump(4096), 0, 4096};
  Buf *back = W;
  W = &rb;
  for (u32 k = 0; k < nreport; k++) {
    put_jsonS(report[ord[k]]);
    LIT("\\n");
  }
  LIT("built ");
  put_num(nkept - nposts);
  LIT(" pages, ");
  put_num(nposts);
  LIT(" posts, ");
  put_num(ntags);
  LIT(" tags and ");
  put_num(nstatics);
  LIT(" static files\\n");
  W = back;
  // move OUT[report_at..] right by rb.len and drop the report in
  ROOM(rb.len);
  __builtin_memmove(OUT.p + report_at + rb.len, OUT.p + report_at, OUT.len - report_at);
  __builtin_memcpy(OUT.p + report_at, rb.p, rb.len);
  OUT.len += rb.len;
  LIT("}");
}

// ---------------------------------------------------------------- reading a case

static void parse_case_input(void) {
  int have_site = 0, have_tpl = 0, have_files = 0;
  expect('{');
  if (peek('}')) TRAP();
  for (;;) {
    S key = jstr();
    expect(':');
    if (seq(key, SL("site"))) {
      have_site = 1;
      int t = 0, b = 0, p = 0, r = 0;
      expect('{');
      if (!peek('}'))
        for (;;) {
          S k = jstr();
          expect(':');
          if (seq(k, SL("title"))) {
            site_title = jstr();
            t = 1;
          } else if (seq(k, SL("base_url"))) {
            S u = jstr();
            while (u.n && u.p[u.n - 1] == '/') u.n--;
            base = u;
            b = 1;
          } else if (seq(k, SL("per_page"))) {
            per_page = jint();
            p = 1;
          } else if (seq(k, SL("recent"))) {
            recent_n = jint();
            r = 1;
          } else
            jskip();
          if (peek(',')) {
            ji++;
            continue;
          }
          break;
        }
      expect('}');
      if (!t || !b || !p || !r || per_page == 0) TRAP();
    } else if (seq(key, SL("templates"))) {
      have_tpl = 1;
      expect('{');
      ntpl = 0;
      u32 cap = 64;
      tpls = bump(cap * sizeof(Tpl));
      if (!peek('}'))
        for (;;) {
          S name = jstr();
          expect(':');
          S src = jstr();
          for (u32 q = 0; q < ntpl; q++)
            if (seq(tpls[q].name, name)) TRAP();
          if (ntpl == cap) {
            Tpl *n = bump(cap * 2 * sizeof(Tpl));
            __builtin_memcpy(n, tpls, ntpl * sizeof(Tpl));
            tpls = n;
            cap *= 2;
          }
          tpls[ntpl].name = name;
          tpls[ntpl].ok = 0;
          // parse now
          u32 head;
          if (parse_template(name, src, &head)) {
            tpls[ntpl].ok = 1;
            tpls[ntpl].head = head;
          } else
            add_report(tpl_error);
          ntpl++;
          if (peek(',')) {
            ji++;
            continue;
          }
          break;
        }
      expect('}');
    } else if (seq(key, SL("files"))) {
      have_files = 1;
      expect('[');
      u32 cap = 1024;
      files = bump(cap * sizeof(File));
      nfiles = 0;
      if (!peek(']'))
        for (;;) {
          if (nfiles == cap) {
            File *n = bump(cap * 2 * sizeof(File));
            __builtin_memcpy(n, files, nfiles * sizeof(File));
            files = n;
            cap *= 2;
          }
          File *f = &files[nfiles++];
          int hp = 0, hc = 0;
          expect('{');
          if (!peek('}'))
            for (;;) {
              S k = jstr();
              expect(':');
              if (seq(k, SL("path"))) {
                f->path = jstr();
                hp = 1;
              } else if (seq(k, SL("content"))) {
                f->content = jstr();
                f->raw_a = RAW_A;
                f->raw_b = RAW_B;
                hc = 1;
              } else
                jskip();
              if (peek(',')) {
                ji++;
                continue;
              }
              break;
            }
          expect('}');
          if (!hp || !hc) TRAP();
          if (peek(',')) {
            ji++;
            continue;
          }
          break;
        }
      expect(']');
    } else
      jskip();
    if (peek(',')) {
      ji++;
      continue;
    }
    break;
  }
  expect('}');
  if (!have_site || !have_tpl || !have_files) TRAP();
  // includes resolved to the templates that parse
  for (u32 n = 0; n < nnodes; n++)
    if (nodes[n].kind == N_INCLUDE && nodes[n].a == NONE) nodes[n].a = find_tpl(mk((const u8 *)nodes[n].b, nodes[n].c));
}

// ---------------------------------------------------------------- entry points

static u8 *INPUT;
static u32 input_len;

__attribute__((export_name("input"))) u32 input(u32 n) {
  INPUT = bump(n + 64);
  input_len = n;
  return (u32)INPUT;
}

__attribute__((export_name("run"))) void run(void) {
  for (u32 k = 0; k < 128; k++) SPECIAL[k] = ESCAPABLE[k] = 0;
  SPECIAL['\\'] = SPECIAL['`'] = SPECIAL['*'] = SPECIAL['['] = 1;
  const char *e = "\\`*_[]()#+-.!>";
  for (u32 k = 0; e[k]; k++) ESCAPABLE[(u8)e[k]] = 1;
  J = INPUT;
  jn = input_len;
  ji = 0;
  OUT.cap = input_len * 4 + 65536;
  OUT.p = bump(OUT.cap + 64);
  MDB.cap = input_len * 2 + 65536;
  MDB.p = bump(MDB.cap + 64);
  TMP.cap = 65536;
  TMP.p = bump(TMP.cap + 64);
  CB.cap = 1 << 20;
  CB.p = bump(CB.cap + 64);
  W = &OUT;
  expect('[');
  OUT.len = 0;
  LIT("[");
  int first = 1;
  if (!peek(']'))
    for (;;) {
      nnodes = nodecap = 0;
      ntoks = tokcap = 0;
      npaths = pathcap = 0;
      ntocs = toccap = 0;
      nreport = reportcap = 0;
      linecap = tcap = 0;
      expect('{');
      int have_input = 0;
      if (!peek('}'))
        for (;;) {
          S k = jstr();
          expect(':');
          if (seq(k, SL("input"))) {
            parse_case_input();
            have_input = 1;
          } else
            jskip();
          if (peek(',')) {
            ji++;
            continue;
          }
          break;
        }
      expect('}');
      if (!have_input) TRAP();
      W = &OUT;
      if (!first) LIT(",");
      first = 0;
      build_case();
      flush(OUT.p, OUT.len);
      OUT.len = 0;
      if (peek(',')) {
        ji++;
        continue;
      }
      break;
    }
  expect(']');
  W = &OUT;
  LIT("]");
  flush(OUT.p, OUT.len);
  OUT.len = 0;
}
