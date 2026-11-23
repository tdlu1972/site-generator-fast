// md.c, built to md.wasm by build.sh: makefun-site's Markdown (vendor/makefun-site/lib/markdown.js), read from a
// source body's UTF-8 bytes and written as the HTML's JSON spelling (the inside of a JSON string), with each link
// resolved, the table of contents, the excerpt and the body's word count. fast.js hands it every kept source of a
// site in turn and slices the answer's texts out of what it writes.
//
// A case: reserve() sizes the regions, fast.js writes the site table (the base url, each kept source's path and url,
// each static path) and site_load() reads it, then for each source fast.js writes its body and calls render(). Every
// render appends to one output region; meta (below) gives the source's spans in UTF-16 code units of that region, so
// fast.js decodes the region once and slices it. A link to nothing is recorded in the broken region.
//
// Anything outside what this reads the reference's way traps, and fast.js answers the site with the reference.

typedef unsigned char u8;
typedef unsigned int u32;
#define NONE 0xffffffffu

extern u8 __heap_base;
static u32 heap_top;

static u8 *bump(u32 size) {
  u32 p = heap_top;
  u32 need = p + size + 64;
  u32 have = (u32)__builtin_wasm_memory_size(0) * 65536u;
  if (need > have && __builtin_wasm_memory_grow(0, (need - have + 65535) / 65536) == (__SIZE_TYPE__)-1) __builtin_trap();
  heap_top = (p + size + 15) & ~15u;
  return (u8 *)p;
}

// The slug of a heading's text that is not all ASCII (NFKD needs Unicode's tables): fast.js writes it at `out`.
__attribute__((import_module("env"), import_name("uslug"))) u32 uslug(const u8 *p, u32 n, u8 *out);

static u8 *IN, *SITE, *O, *T, *BRK, *IDS, *PATHB, *SCR;
static u32 *LINES;
static u32 o, ocap, tcap, brk, idlen;
static u32 meta[8 + 5 * 512];

#define ROOM(k) \
  if (o + (k) > ocap) __builtin_trap()

// ---------------------------------------------------------------- output

static void put(const u8 *p, u32 n) {
  ROOM(n);
  for (u32 k = 0; k < n; k++) O[o + k] = p[k];
  o += n;
}

#define LIT(s) put((const u8 *)(s), sizeof(s) - 1)

static const u8 HEX[] = "0123456789abcdef";

// p[0, n) HTML-escaped (& < > " ') and then JSON-escaped.
static void put_text(const u8 *p, u32 n) {
  ROOM(n * 6 + 8);
  u8 *q = O + o;
  for (u32 k = 0; k < n; k++) {
    u8 c = p[k];
    if (c >= 0x40 || (c >= 0x28 && c != 0x3c && c != 0x3e) || c == 0x20 || c == 0x21 || c == 0x23 || c == 0x24 || c == 0x25) {
      if (c == '\\') {
        *q++ = '\\';
        *q++ = '\\';
      } else
        *q++ = c;
      continue;
    }
    switch (c) {
      case '&': *q++ = '&'; *q++ = 'a'; *q++ = 'm'; *q++ = 'p'; *q++ = ';'; break;
      case '<': *q++ = '&'; *q++ = 'l'; *q++ = 't'; *q++ = ';'; break;
      case '>': *q++ = '&'; *q++ = 'g'; *q++ = 't'; *q++ = ';'; break;
      case '"': *q++ = '&'; *q++ = 'q'; *q++ = 'u'; *q++ = 'o'; *q++ = 't'; *q++ = ';'; break;
      case '\'': *q++ = '&'; *q++ = '#'; *q++ = '3'; *q++ = '9'; *q++ = ';'; break;
      case '\n': *q++ = '\\'; *q++ = 'n'; break;
      case '\t': *q++ = '\\'; *q++ = 't'; break;
      default:
        *q++ = '\\'; *q++ = 'u'; *q++ = '0'; *q++ = '0'; *q++ = HEX[c >> 4]; *q++ = HEX[c & 15];
    }
  }
  o = (u32)(q - O);
}

static void put_num(u32 v) {
  u8 b[12];
  u32 n = 0;
  do {
    b[n++] = (u8)('0' + v % 10);
    v /= 10;
  } while (v);
  ROOM(n);
  while (n) O[o++] = b[--n];
}

// UTF-16 code units of O[0, pos), counted on from the last position asked (positions are asked in order).
static u32 upos, uval;
static u32 units_to(u32 pos) {
  if (pos < upos) __builtin_trap();
  u32 v = uval;
  for (u32 k = upos; k < pos; k++) {
    u8 b = O[k];
    v += (b & 0xc0) != 0x80;
    v += b >= 0xf0;
  }
  upos = pos;
  uval = v;
  return v;
}

// ---------------------------------------------------------------- text

// Whether p[0..] starts a character a JavaScript \s matches; its byte length in *len.
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
  if (n >= 3) {
    *len = 3;
    if (c == 0xe1) return p[1] == 0x9a && p[2] == 0x80;
    if (c == 0xe2) {
      if (p[1] == 0x80) return (p[2] >= 0x80 && p[2] <= 0x8a) || p[2] == 0xa8 || p[2] == 0xa9 || p[2] == 0xaf;
      return p[1] == 0x81 && p[2] == 0x9f;
    }
    if (c == 0xe3) return p[1] == 0x80 && p[2] == 0x80;
    if (c == 0xef) return p[1] == 0xbb && p[2] == 0xbf;
  }
  *len = 1;
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

static u32 words(const u8 *p, u32 n) {
  u32 w = 0;
  int in = 0;
  for (u32 k = 0; k < n;) {
    u8 c = p[k];
    if (c < 0x80) {
      int s = c == 32 || (c >= 9 && c <= 13);
      if (s)
        in = 0;
      else if (!in) {
        in = 1;
        w++;
      }
      k++;
      continue;
    }
    u32 l;
    if (ws_at(p + k, n - k, &l))
      in = 0;
    else if (!in) {
      in = 1;
      w++;
    }
    k += l;
  }
  return w;
}

// The slug of ASCII p[0, n) into out (at most 64 bytes); its length.
static u32 slug_ascii(const u8 *p, u32 n, u8 *out) {
  u32 len = 0;
  int gap = 0;
  for (u32 k = 0; k < n; k++) {
    u8 c = p[k];
    if (c >= 'A' && c <= 'Z') c = (u8)(c + 32);
    else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
      gap = len > 0;
      continue;
    }
    if (gap) {
      if (len < 64) out[len] = '-';
      len++;
    }
    gap = 0;
    if (len < 64) out[len] = c;
    len++;
  }
  if (len > 64) len = 64;
  while (len > 0 && out[len - 1] == '-') len--;
  if (len == 0) {
    const char *u = "untitled";
    for (u32 k = 0; k < 8; k++) out[k] = (u8)u[k];
    len = 8;
  }
  return len;
}

// ---------------------------------------------------------------- the site table: paths to urls

typedef struct {
  u32 key, klen, val, vlen, kind;  // kind 1 a page (val its url), 2 a static file
} Entry;

#define SLOTS 8192
static Entry table[SLOTS];
static u32 base_off, base_len, entries;

static u32 hash(const u8 *p, u32 n) {
  u32 h = 2166136261u;
  for (u32 k = 0; k < n; k++) h = (h ^ p[k]) * 16777619u;
  return h;
}

static Entry *find(const u8 *p, u32 n) {
  for (u32 s = hash(p, n) & (SLOTS - 1);; s = (s + 1) & (SLOTS - 1)) {
    Entry *e = &table[s];
    if (!e->kind) return e;
    if (e->klen == n) {
      u32 k = 0;
      while (k < n && SITE[e->key + k] == p[k]) k++;
      if (k == n) return e;
    }
  }
}

// SITE[0, len): "B<base>\n", then "P<path>\t<url>\n" for each kept source and "S<path>\n" for each static file.
__attribute__((export_name("site_load"))) void site_load(u32 len) {
  for (u32 s = 0; s < SLOTS; s++) table[s].kind = 0;
  entries = 0;
  u32 i = 0;
  while (i < len) {
    u8 kind = SITE[i++];
    u32 a = i;
    while (SITE[i] != '\n' && SITE[i] != '\t') i++;
    u32 b = i;
    if (kind == 'B') {
      base_off = a;
      base_len = b - a;
      i++;
      continue;
    }
    u32 v = 0, vl = 0;
    if (kind == 'P') {
      if (SITE[i] != '\t') __builtin_trap();
      v = ++i;
      while (SITE[i] != '\n') i++;
      vl = i - v;
    }
    i++;
    if (++entries > SLOTS / 2) __builtin_trap();
    Entry *e = find(SITE + a, b - a);
    if (e->kind) continue;  // a page's path is never a static's (one ends in .md)
    e->key = a;
    e->klen = b - a;
    e->val = v;
    e->vlen = vl;
    e->kind = kind == 'P' ? 1 : 2;
  }
}

// ---------------------------------------------------------------- links

static const u8 *DIR;
static u32 DIRLEN, SRC;

static int is_alpha(u8 c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

static void broken(const u8 *t, u32 n) {
  u32 *h = (u32 *)(BRK + brk);
  h[0] = SRC;
  h[1] = n;
  for (u32 k = 0; k < n; k++) BRK[brk + 8 + k] = t[k];
  brk += (8 + n + 3) & ~3u;
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
    // The path from DIR: its segments, then the target's, "." and "" skipped, ".." one up (never above the root).
    u32 seg_off[256], seg_len[256], ns = 0;
    int ok = 1;
    for (u32 a = 0; a < DIRLEN && DIRLEN;) {
      u32 b = a;
      while (b < DIRLEN && DIR[b] != '/') b++;
      if (ns == 256) __builtin_trap();
      seg_off[ns] = a | 0x80000000u;
      seg_len[ns++] = b - a;
      a = b + 1;
    }
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
        if (ns == 256) __builtin_trap();
        seg_off[ns] = a;
        seg_len[ns++] = l;
      }
      a = b + 1;
    }
    if (ok) {
      u32 pl = 0;
      for (u32 s = 0; s < ns; s++) {
        if (s) PATHB[pl++] = '/';
        const u8 *src = (seg_off[s] & 0x80000000u) ? DIR + (seg_off[s] & 0x7fffffffu) : t + seg_off[s];
        for (u32 k = 0; k < seg_len[s]; k++) PATHB[pl++] = src[k];
      }
      Entry *e = find(PATHB, pl);
      if (e->kind == 1) {
        put_text(SITE + e->val, e->vlen);
        put_text(t + hash_at, n - hash_at);
        return;
      }
      if (e->kind == 2) {
        put_text(SITE + base_off, base_len);
        LIT("/");
        put_text(PATHB, pl);
        put_text(t + hash_at, n - hash_at);
        return;
      }
    }
  }
  broken(t, n);
  put_text(t, n);
}

// ---------------------------------------------------------------- inline

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

static void inl(const u8 *s, u32 i, u32 to) {
  u32 run = i;
  while (i < to) {
    u8 c = s[i];
    if (c >= 128 || !SPECIAL[c]) {
      i++;
      continue;
    }
    if (c == '\\') {
      if (escapable(s, i + 1, to)) {
        put_text(s + run, i - run);
        put_text(s + i + 1, 1);
        i += 2;
        run = i;
      } else
        i++;
      continue;
    }
    if (c == '`') {
      u32 j = i + 1;
      while (j < to && s[j] != '`') j++;
      if (j < to && j > i + 1) {
        put_text(s + run, i - run);
        LIT("<code>");
        put_text(s + i + 1, j - i - 1);
        LIT("</code>");
        i = j + 1;
        run = i;
      } else
        i++;
      continue;
    }
    if (c == '*') {
      if (i + 1 < to && s[i + 1] == '*') {
        u32 j = dstar(s, i + 2, to);
        if (j != NONE && j > i + 2) {
          put_text(s + run, i - run);
          LIT("<strong>");
          inl(s, i + 2, j);
          LIT("</strong>");
          i = j + 2;
          run = i;
        } else
          i += 2;
        continue;
      }
      u32 j = sstar(s, i + 1, to);
      if (j != NONE && j > i + 1) {
        put_text(s + run, i - run);
        LIT("<em>");
        inl(s, i + 1, j);
        LIT("</em>");
        i = j + 1;
        run = i;
      } else
        i++;
      continue;
    }
    // [
    u32 kk = find1(s, ']', i + 1, to);
    u32 m = NONE;
    if (kk != NONE && kk + 1 < to && s[kk + 1] == '(') {
      m = kk + 2;
      while (m < to && s[m] != ')') m++;
      if (m == to) m = NONE;
    }
    if (m != NONE && m > kk + 2 && !has_ws(s + kk + 2, m - kk - 2)) {
      put_text(s + run, i - run);
      LIT("<a href=\\\"");
      link(s + kk + 2, m - kk - 2);
      LIT("\\\">");
      inl(s, i + 1, kk);
      LIT("</a>");
      i = m + 1;
      run = i;
      continue;
    }
    i++;
  }
  put_text(s + run, to - run);
}

// ---------------------------------------------------------------- blocks

static const u8 *B;  // the body
static u32 nlines;   // LINES[k] is line k's start; LINES[k + 1] - 1 its end

#define LS(k) (LINES[k])
#define LE(k) (LINES[(k) + 1] - 1)

static int blank(u32 a, u32 e) {
  for (u32 k = a; k < e; k++)
    if (B[k] != ' ' && B[k] != '\t') return 0;
  return 1;
}

// A fence's language span (*la, *le), or 0.
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

// A heading's depth, its text span in (*ta, *te), or 0.
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

// The digit count of an ordered item ("12. "), or 0.
static u32 ordered_at(u32 a, u32 e) {
  u32 k = a;
  while (k < e && B[k] >= '0' && B[k] <= '9') k++;
  u32 n = k - a;
  if (n == 0 || n > 9 || k + 1 >= e + 0 || B[k] != '.' || k + 1 >= e || B[k + 1] != ' ') return 0;
  return n;
}

static int continued(u32 a, u32 e) {
  u32 k = a;
  while (k < e && B[k] == ' ') k++;
  if (k - a < 2 || k >= e) return 0;
  u32 l;
  return !ws_at(B + k, e - k, &l);
}

static u32 tl;  // T's length
static void tput(u32 a, u32 e) {
  if (tl + (e - a) + 1 > tcap) __builtin_trap();
  for (u32 k = a; k < e; k++) T[tl++] = B[k];
}

// meta: [0] the output's start, [1] its end (code units), [2] words, [3] excerpt start, [4] its end (NONE when none),
// [5] toc entries; then per entry: level, id offset and length in IDS, html start and end (code units).
__attribute__((export_name("render"))) void render(u32 body, u32 len, u32 dir, u32 dirlen, u32 src) {
  B = IN + body;
  DIR = IN + dir;
  DIRLEN = dirlen;
  SRC = src;
  meta[0] = units_to(o);
  meta[2] = words(B, len);
  meta[3] = meta[4] = NONE;
  u32 ntoc = 0;
  idlen = 0;
  u32 ids_off[512], ids_len[512], nids = 0;

  nlines = 0;
  LINES[nlines++] = 0;
  for (u32 k = 0; k < len; k++)
    if (B[k] == '\n') LINES[nlines++] = k + 1;
  LINES[nlines] = len + 1;  // the last line ends at len

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
      // The id: the slug of the text as written, made unique in the page.
      u8 *slug = SCR;
      u32 sl;
      int ascii = 1;
      for (u32 k = x; k < y; k++)
        if (B[k] >= 0x80) ascii = 0;
      sl = ascii ? slug_ascii(B + x, y - x, slug) : uslug(B + x, y - x, slug);
      u32 id = idlen, idl = sl;
      for (u32 n = 1;; n++) {
        // candidate: slug, or slug-n
        idl = sl;
        if (idlen + sl + 12 > 65536) __builtin_trap();
        for (u32 k = 0; k < sl; k++) IDS[id + k] = slug[k];
        if (n > 1) {
          IDS[id + idl++] = '-';
          u8 b[12];
          u32 bn = 0, v = n;
          do {
            b[bn++] = (u8)('0' + v % 10);
            v /= 10;
          } while (v);
          while (bn) IDS[id + idl++] = b[--bn];
        }
        int taken = 0;
        for (u32 q = 0; q < nids && !taken; q++) {
          if (ids_len[q] != idl) continue;
          u32 k = 0;
          while (k < idl && IDS[ids_off[q] + k] == IDS[id + k]) k++;
          taken = k == idl;
        }
        if (!taken) break;
      }
      if (nids == 512) __builtin_trap();
      ids_off[nids] = id;
      ids_len[nids++] = idl;
      idlen += idl;
      LIT("<h");
      put_num(depth);
      LIT(" id=\\\"");
      put(IDS + id, idl);
      LIT("\\\">");
      u32 h0 = o;
      inl(B, x, y);
      u32 h1 = o;
      LIT("</h");
      put_num(depth);
      LIT(">\\n");
      if (depth == 2 || depth == 3) {
        if (ntoc == 512) __builtin_trap();
        u32 *m = meta + 8 + 5 * ntoc++;
        m[0] = depth;
        m[1] = id;
        m[2] = idl;
        m[3] = units_to(h0);
        m[4] = units_to(h1);
      }
      i++;
      continue;
    }
    if (is_hr(a, e)) {
      LIT("<hr>\\n");
      i++;
      continue;
    }
    if (B[a] == '>' && e > a) {
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
      // Items are gathered whole (continuation lines joined on), then each is written.
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
    // A paragraph: this line and the next ones up to a blank line, a fence, a heading or a thematic break.
    tl = 0;
    {
      u32 la = a, le = e;
      trim(&la, &le);
      tput(la, le);
    }
    i++;
    while (i < nlines && !ends_paragraph(LS(i), LE(i))) {
      u32 la = LS(i), le = LE(i);
      trim(&la, &le);
      T[tl++] = '\n';
      tput(la, le);
      i++;
    }
    LIT("<p>");
    u32 p0 = o;
    inl(T, 0, tl);
    u32 p1 = o;
    LIT("</p>\\n");
    if (meta[3] == NONE) {
      meta[3] = units_to(p0);
      meta[4] = units_to(p1);
    }
  }
  meta[5] = ntoc;
  meta[1] = units_to(o);
}

// ---------------------------------------------------------------- regions

static u32 in_cap;

// Sizes the regions for a site whose bodies take `in` bytes all told (the longest `maxbody`) and whose table `site`.
__attribute__((export_name("reserve"))) void reserve(u32 in, u32 maxbody, u32 site) {
  heap_top = ((u32)&__heap_base + 15) & ~15u;
  in_cap = in + 64;
  IN = bump(in_cap);
  SITE = bump(site + 64);
  ocap = in * 7 + 65536;
  O = bump(ocap + 64);
  tcap = maxbody * 2 + 64;
  T = bump(tcap);
  LINES = (u32 *)bump((maxbody + 4) * 4);
  BRK = bump(in + 65536);
  IDS = bump(65536 + 64);
  PATHB = bump(maxbody + 4096);
  SCR = bump(4096);
  o = 0;
  brk = 0;
  upos = uval = 0;
  for (u32 k = 0; k < 128; k++) SPECIAL[k] = ESCAPABLE[k] = 0;
  SPECIAL['\\'] = SPECIAL['`'] = SPECIAL['*'] = SPECIAL['['] = 1;
  const char *e = "\\`*_[]()#+-.!>";
  for (u32 k = 0; e[k]; k++) ESCAPABLE[(u8)e[k]] = 1;
}

__attribute__((export_name("in_ptr"))) u32 in_ptr(void) { return (u32)IN; }
__attribute__((export_name("site_ptr"))) u32 site_ptr(void) { return (u32)SITE; }
__attribute__((export_name("out_ptr"))) u32 out_ptr(void) { return (u32)O; }
__attribute__((export_name("out_len"))) u32 out_len(void) { return o; }
__attribute__((export_name("meta_ptr"))) u32 meta_ptr(void) { return (u32)meta; }
__attribute__((export_name("brk_ptr"))) u32 brk_ptr(void) { return (u32)BRK; }
__attribute__((export_name("brk_len"))) u32 brk_len(void) { return brk; }
__attribute__((export_name("ids_ptr"))) u32 ids_ptr(void) { return (u32)IDS; }
__attribute__((export_name("scr_ptr"))) u32 scr_ptr(void) { return (u32)SCR; }
