/* ======================================================================
 * 白话运行时
 *
 * 白话编译器生成的每个 C 程序，开头都带着这一份代码。它负责：
 * 值的表示、垃圾回收、加减乘除和比较、文字、列表和字典、内置函数、中文报错。
 * 整数没有上限：64 位装不下时自动换成大整数。
 * 用不到的内存由垃圾回收器收回（标记清除）。
 * 出错时列出调用过程：是从哪里一层一层调用到出错的地方的。
 * ====================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/wait.h>
#endif

/* ---------- 让 C 编译器生成更快的代码 ---------- */

#if defined(__GNUC__) || defined(__clang__)
#define BH_LIKELY(x) __builtin_expect(!!(x), 1)
#define BH_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define BH_INLINE static inline __attribute__((always_inline))
#define BH_NOINLINE __attribute__((noinline))
#else
#define BH_LIKELY(x) (x)
#define BH_UNLIKELY(x) (x)
#define BH_INLINE static inline
#define BH_NOINLINE
#endif

#if defined(__has_builtin)
#if __has_builtin(__builtin_add_overflow) && __has_builtin(__builtin_mul_overflow)
#define BH_HAVE_OVERFLOW_BUILTINS 1
#endif
#elif defined(__GNUC__) && __GNUC__ >= 5
#define BH_HAVE_OVERFLOW_BUILTINS 1
#endif

/* ---------- 值的表示 ---------- */

enum { T_UNDEF, T_NIL, T_BOOL, T_INT, T_FLOAT, T_STR, T_LIST, T_FUNC, T_BUILTIN, T_BIG, T_FRAME, T_DICT };

typedef struct { int tag; union { int64_t i; double f; void *p; } u; } Value;

/* 放在堆上的东西，开头都有这么一段，垃圾回收器靠它把所有东西串起来 */
typedef struct Obj { struct Obj *next; size_t size; unsigned char kind, mark; } Obj;
enum { K_STR, K_BUF, K_LIST, K_FRAME, K_FUNC, K_BIG, K_DICT };

typedef struct { Obj h; int64_t cap, used; uint32_t cp[1]; } Buf;        /* 可以往后接的文字缓冲区 */
typedef struct { Obj h; int64_t len; uint32_t *cp; Buf *buf; } Str;      /* 文字：一串 Unicode 编码 */
typedef struct { Obj h; int64_t len, cap; Value *items; } List;
typedef struct Frame { Obj h; struct Frame *up; int64_t n; Value v[1]; } Frame;   /* 会被内层函数用到的局部变量 */
typedef struct { Obj h; Value (*code)(Frame *, Value *); int arity; const char *name; Frame *up; } Func;
typedef struct { Obj h; int neg; int64_t n; uint32_t d[1]; } Big;        /* 大整数：绝对值每 32 位一段，低位在前 */
/* 字典：条目按放进去的先后排在 e 里（删掉的条目键的 tag 是 T_UNDEF），
 * slots 是散列表，存条目的位置：-1 空着，-2 原来的条目删掉了 */
typedef struct { uint64_t hash; Value key, val; } DictEntry;
typedef struct { Obj h; int64_t len, used, cap, nslots; DictEntry *e; int64_t *slots; } Dict;
typedef struct { const char *name; Value (*fn)(int, Value *); int min, max; } Builtin;  /* max < 0：个数不限 */
typedef struct BhRoots { struct BhRoots *prev; int64_t n; } BhRoots;     /* 后面紧跟着 n 个 Value */

static const Value BH_NIL = {T_NIL, {0}}, BH_TRUE = {T_BOOL, {1}}, BH_FALSE = {T_BOOL, {0}};

static int bh_line = 0;                          /* 正在执行的白话代码是第几行 */
static const char **bh_src = NULL;               /* 源代码的每一行，报错时显示 */
static int bh_src_n = 0;
static const char **bh_rt_lines = NULL;          /* 运行时自己的源码（只有编译器带着） */
static int bh_rt_lines_n = 0;
static int bh_argc = 0;
static char **bh_argv = NULL;
static uintptr_t bh_stack_base = 0;
static size_t bh_stack_limit = 768 * 1024;

#define BH_TOO_DEEP "嵌套得太深了。如果是函数自己调用自己，检查一下是不是缺少出口"
#define BH_TOO_BIG "数字太大了，算不了"

static Value bh_int(int64_t i) { Value v; v.tag = T_INT; v.u.i = i; return v; }
static Value bh_float(double f) { Value v; v.tag = T_FLOAT; v.u.f = f; return v; }
static Value bh_bool(int b) { return b ? BH_TRUE : BH_FALSE; }
static Value bh_obj(int tag, void *p) { Value v; v.tag = tag; v.u.p = p; return v; }

/* ---------- 报错 ---------- */

/* 正在进行的函数调用，一层套一层地串起来。出错时用它显示「调用过程」。
 * line 是调用发生的那一行（在调用者里），name 是被调用的函数 */
typedef struct BhCall { struct BhCall *prev; const char *name; int line; } BhCall;
static BhCall *bh_calls = NULL;

/* 调用过程里的一层：在哪个函数里（NULL 是最外层）、执行到第几行 */
typedef struct { const char *name; int line; } BhFrameInfo;
typedef struct { int64_t first, size, times; } BhStackUnit;    /* 一段：从第 first 层开始，size 层一组，重复 times 遍 */

static int bh_frame_same(const BhFrameInfo *a, const BhFrameInfo *b) {
    if (a->line != b->line) return 0;
    if (!a->name || !b->name) return a->name == b->name;
    return strcmp(a->name, b->name) == 0;
}

static void bh_stack_unit(const BhFrameInfo *f, const BhStackUnit *u) {
    int64_t k;
    for (k = u->first; k < u->first + u->size; k++) {
        if (f[k].name) fprintf(stderr, "  函数「%s」，第 %d 行", f[k].name, f[k].line);
        else fprintf(stderr, "  最外层，第 %d 行", f[k].line);
        if (f[k].line >= 1 && f[k].line <= bh_src_n && bh_src[f[k].line - 1][0]) fprintf(stderr, "：%s", bh_src[f[k].line - 1]);
        fputc('\n', stderr);
    }
    if (u->times > 1 && u->size == 1) fprintf(stderr, "  （上面这一行又重复了 %lld 次）\n", (long long)(u->times - 1));
    else if (u->times > 1) fprintf(stderr, "  （上面这 %lld 行又重复了 %lld 次）\n", (long long)u->size, (long long)(u->times - 1));
}

/* 从最外层往里，一层一层列出来。连着重复的几层（递归）并在一起；太多的话只列开头和结尾。
 * 解释器 baihua.py 的 print_stack 做同样的事，两边的输出一字不差 */
static void bh_print_stack(void) {
    BhCall *r;
    BhFrameInfo *f;
    BhStackUnit *units;
    int64_t depth = 0, n, nu = 0, i, k, p, reps, omitted;
    if (!bh_calls) return;
    for (r = bh_calls; r; r = r->prev) depth++;
    n = depth + 1;
    f = (BhFrameInfo *)malloc((size_t)n * sizeof(BhFrameInfo));
    units = (BhStackUnit *)malloc((size_t)n * sizeof(BhStackUnit));
    if (!f || !units) return;
    f[depth].name = bh_calls->name;
    f[depth].line = bh_line;
    for (r = bh_calls, k = depth; r; r = r->prev, k--) {
        f[k - 1].line = r->line;
        f[k - 1].name = r->prev ? r->prev->name : NULL;
    }
    i = 0;
    while (i < n) {
        units[nu].first = i;
        units[nu].size = 1;
        units[nu].times = 1;
        for (p = 1; p <= 3; p++) {
            reps = 1;
            while (i + (reps + 1) * p <= n) {
                for (k = 0; k < p; k++)
                    if (!bh_frame_same(&f[i + reps * p + k], &f[i + k])) break;
                if (k < p) break;
                reps++;
            }
            if (reps >= 2) { units[nu].size = p; units[nu].times = reps; break; }
        }
        i += units[nu].size * units[nu].times;
        nu++;
    }
    fputs("调用过程（从最外层到出错的地方）：\n", stderr);
    if (nu <= 24) {
        for (i = 0; i < nu; i++) bh_stack_unit(f, &units[i]);
    } else {
        for (i = 0; i < 8; i++) bh_stack_unit(f, &units[i]);
        omitted = 0;
        for (i = 8; i < nu - 16; i++) omitted += units[i].size * units[i].times;
        fprintf(stderr, "  ……（中间省略了 %lld 层）……\n", (long long)omitted);
        for (i = nu - 16; i < nu; i++) bh_stack_unit(f, &units[i]);
    }
    free(f);
    free(units);
}

static void bh_fail(const char *fmt, ...) {
    va_list ap;
    fflush(stdout);
    fputs("运行错误", stderr);
    if (bh_line > 0) fprintf(stderr, "（第 %d 行）", bh_line);
    fputs("：", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (bh_line >= 1 && bh_line <= bh_src_n && bh_src[bh_line - 1][0])
        fprintf(stderr, "  >  %s\n", bh_src[bh_line - 1]);
    bh_print_stack();
    exit(1);
}

static void *bh_xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) bh_fail("内存不够了");
    return p;
}

/* ======================================================================
 * 垃圾回收：标记清除
 *
 * 1. 所有放在堆上的东西都串在 bh_all 这条链上。
 * 2. 回收时从「根」出发：最外层的变量、文字常量、所有正在运行的函数的变量。
 *    顺着它们能找到的东西都做上标记。
 * 3. 把链走一遍，没有标记的就是垃圾，释放掉。
 *
 * 正在运行的函数的变量怎么找？编译器让每个函数把自己的变量放进一个数组，
 * 并挂到 bh_roots 这条链上（「影子栈」），回收器照着这条链找。
 *
 * 回收只在「安全点」发生：函数刚开始时、循环每转一圈时。运行时内部的 C 代码
 * 执行到一半时绝不会回收，所以这些代码可以放心地把值拿在手里。
 * ====================================================================== */

static Obj *bh_all = NULL;
static size_t bh_since_gc = 0;                           /* 上次回收之后又申请了多少内存 */
static size_t bh_gc_threshold = (size_t)8 << 20;         /* 申请超过这么多就该回收了 */
static int bh_gc_wanted = 0;
static long bh_gc_stress = 0, bh_gc_countdown = 0;       /* 测试用：环境变量 BH_GC_STRESS=N，每 N 个安全点强制回收一次 */
static BhRoots *bh_roots = NULL;
static Value *bh_root_arrays[2];
static int64_t bh_root_counts[2];
static Obj **bh_gray = NULL;                             /* 已经标记、但还没检查它里面装了什么的东西 */
static size_t bh_gray_n = 0, bh_gray_cap = 0;

/* ---------- 小块内存池 ----------
 * 回收器管的东西（短文字、短列表、函数……）大多很小，申请和释放又特别频繁。
 * 按 16 字节分档：释放掉的块串起来留着，下次要同样大小的直接拿来用，比每次都找 malloc 快得多。
 * 用 AddressSanitizer 检查内存时，空闲的块会被标成「不许碰」，用了已经释放的东西照样能查出来。 */
#define BH_POOL_MAX 256
#define BH_POOL_CHUNK 65536
static void *bh_pool[BH_POOL_MAX / 16 + 1];

#if defined(__SANITIZE_ADDRESS__)
#define BH_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define BH_ASAN 1
#endif
#endif
#ifdef BH_ASAN
void __asan_poison_memory_region(void const volatile *addr, size_t size);
void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
#define BH_POISON(p, n) __asan_poison_memory_region((p), (n))
#define BH_UNPOISON(p, n) __asan_unpoison_memory_region((p), (n))
#else
#define BH_POISON(p, n) ((void)0)
#define BH_UNPOISON(p, n) ((void)0)
#endif

static void *bh_alloc(size_t n) {
    size_t k, size, i;
    void *p;
    if (n > BH_POOL_MAX) return bh_xmalloc(n);
    k = n ? (n + 15) >> 4 : 1;
    if (!bh_pool[k]) {                       /* 这一档没有空闲的了：一次要一大块，切成小块 */
        char *chunk = (char *)bh_xmalloc(BH_POOL_CHUNK);
        size = k << 4;
        for (i = 0; i + size <= BH_POOL_CHUNK; i += size) {
            *(void **)(chunk + i) = bh_pool[k];
            bh_pool[k] = chunk + i;
            BH_POISON(chunk + i, size);
        }
    }
    p = bh_pool[k];
    BH_UNPOISON(p, k << 4);
    bh_pool[k] = *(void **)p;
    return p;
}

static void bh_release(void *p, size_t n) {
    size_t k;
    if (!p) return;
    if (n > BH_POOL_MAX) { free(p); return; }
    k = n ? (n + 15) >> 4 : 1;
    *(void **)p = bh_pool[k];
    bh_pool[k] = p;
    BH_POISON(p, k << 4);
}

static void *bh_new(int kind, size_t size) {
    Obj *o = (Obj *)bh_alloc(size);
    o->next = bh_all;
    bh_all = o;
    o->size = size;
    o->kind = (unsigned char)kind;
    o->mark = 0;
    bh_since_gc += size;
    if (bh_since_gc > bh_gc_threshold) bh_gc_wanted = 1;
    return o;
}

static void bh_mark_obj(void *p) {
    Obj *o = (Obj *)p;
    if (!o || o->mark) return;
    o->mark = 1;
    if (bh_gray_n == bh_gray_cap) {
        bh_gray_cap = bh_gray_cap ? bh_gray_cap * 2 : 1024;
        bh_gray = (Obj **)realloc(bh_gray, bh_gray_cap * sizeof(Obj *));
        if (!bh_gray) bh_fail("内存不够了");
    }
    bh_gray[bh_gray_n++] = o;
}

static void bh_mark_value(Value v) {
    if (v.tag == T_STR || v.tag == T_LIST || v.tag == T_FUNC || v.tag == T_BIG || v.tag == T_FRAME || v.tag == T_DICT) bh_mark_obj(v.u.p);
}

static void bh_collect(void) {
    Obj **link, *o;
    BhRoots *r;
    size_t live = 0;
    int64_t i;
    int k;
    if (bh_gc_stress > 1 && bh_since_gc <= bh_gc_threshold) {
        if (--bh_gc_countdown > 0) return;
        bh_gc_countdown = bh_gc_stress;
    }
    /* 标记：从根出发 */
    for (k = 0; k < 2; k++)
        for (i = 0; i < bh_root_counts[k]; i++) bh_mark_value(bh_root_arrays[k][i]);
    for (r = bh_roots; r; r = r->prev) {
        Value *v = (Value *)(r + 1);
        for (i = 0; i < r->n; i++) bh_mark_value(v[i]);
    }
    while (bh_gray_n) {
        o = bh_gray[--bh_gray_n];
        if (o->kind == K_STR) {
            bh_mark_obj(((Str *)o)->buf);
        } else if (o->kind == K_LIST) {
            List *l = (List *)o;
            for (i = 0; i < l->len; i++) bh_mark_value(l->items[i]);
        } else if (o->kind == K_FRAME) {
            Frame *f = (Frame *)o;
            bh_mark_obj(f->up);
            for (i = 0; i < f->n; i++) bh_mark_value(f->v[i]);
        } else if (o->kind == K_FUNC) {
            bh_mark_obj(((Func *)o)->up);
        } else if (o->kind == K_DICT) {
            Dict *d = (Dict *)o;
            for (i = 0; i < d->used; i++)
                if (d->e[i].key.tag != T_UNDEF) { bh_mark_value(d->e[i].key); bh_mark_value(d->e[i].val); }
        }
    }
    /* 清除：没被标记的都释放 */
    link = &bh_all;
    while ((o = *link) != NULL) {
        if (o->mark) {
            o->mark = 0;
            live += o->size;
            if (o->kind == K_LIST) live += (size_t)((List *)o)->cap * sizeof(Value);
            if (o->kind == K_DICT) live += (size_t)((Dict *)o)->cap * sizeof(DictEntry) + (size_t)((Dict *)o)->nslots * sizeof(int64_t);
            link = &o->next;
        } else {
            *link = o->next;
            if (o->kind == K_LIST) bh_release(((List *)o)->items, (size_t)((List *)o)->cap * sizeof(Value));
            if (o->kind == K_DICT) { free(((Dict *)o)->e); free(((Dict *)o)->slots); }
            bh_release(o, o->size);
        }
    }
    bh_since_gc = 0;
    bh_gc_threshold = live > ((size_t)8 << 20) ? live : (size_t)8 << 20;   /* 活着的东西越多，隔得越久再回收 */
    bh_gc_wanted = bh_gc_stress != 0;
}

/* 下面三个是给编译器生成的代码用的 */
#define BH_SAFEPOINT() do { if (bh_gc_wanted) bh_collect(); } while (0)
#define BH_ENTER(fr, count) do { memset(&(fr), 0, sizeof(fr)); (fr).h.n = (count); (fr).h.prev = bh_roots; bh_roots = &(fr).h; } while (0)
#define BH_RETURN(fr, x) do { Value bh_result = (x); bh_roots = (fr).h.prev; return bh_result; } while (0)

/* ---------- 文字 ---------- */

static Str *bh_str_alloc(int64_t len) {
    Str *s = (Str *)bh_new(K_STR, sizeof(Str) + (size_t)(len > 0 ? len : 1) * sizeof(uint32_t));
    s->len = len;
    s->cp = (uint32_t *)(s + 1);
    s->buf = NULL;
    return s;
}

/* UTF-8 字节 -> 文字。不合法的字节换成 U+FFFD */
static Str *bh_decode(const char *bytes, size_t n) {
    const unsigned char *s = (const unsigned char *)bytes;
    Str *out = bh_str_alloc((int64_t)n);
    size_t i = 0;
    int64_t k = 0;
    while (i < n) {
        uint32_t c = s[i];
        int extra = 0, j, ok;
        if (c < 0x80) extra = 0;
        else if (c >= 0xC2 && c <= 0xDF) { extra = 1; c &= 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { extra = 2; c &= 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { extra = 3; c &= 0x07; }
        else { out->cp[k++] = 0xFFFD; i++; continue; }
        ok = i + (size_t)extra < n;
        for (j = 1; ok && j <= extra; j++) {
            if ((s[i + j] & 0xC0) != 0x80) ok = 0;
            else c = (c << 6) | (s[i + j] & 0x3F);
        }
        if (!ok || (extra == 2 && c < 0x800) || (extra == 3 && (c < 0x10000 || c > 0x10FFFF))
                || (c >= 0xD800 && c <= 0xDFFF)) {
            out->cp[k++] = 0xFFFD;
            i++;
            continue;
        }
        out->cp[k++] = c;
        i += (size_t)extra + 1;
    }
    out->len = k;
    return out;
}

static size_t bh_encode1(uint32_t c, char *o) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) {
        o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        o[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

/* 文字 -> 以 0 结尾的 UTF-8，给 C 的库函数用。用完要 free */
static char *bh_cstr(const Str *s) {
    char *o = (char *)bh_xmalloc((size_t)s->len * 4 + 1);
    size_t k = 0;
    int64_t i;
    for (i = 0; i < s->len; i++) k += bh_encode1(s->cp[i], o + k);
    o[k] = 0;
    return o;
}

static void bh_fwrite(FILE *f, const uint32_t *cp, int64_t len) {
    char buf[4096];
    size_t k = 0;
    int64_t i;
    for (i = 0; i < len; i++) {
        if (k > sizeof buf - 8) { fwrite(buf, 1, k, f); k = 0; }
        k += bh_encode1(cp[i], buf + k);
    }
    fwrite(buf, 1, k, f);
}

static Value bh_str_lit(const char *z) { return bh_obj(T_STR, bh_decode(z, strlen(z))); }

/* 单个字符的文字。英文字符很常用，各存一份永远不回收的 */
static Value bh_char(uint32_t c) {
    static Str *cache[128];
    Str *s;
    if (c >= 128) {
        s = bh_str_alloc(1);
        s->cp[0] = c;
        return bh_obj(T_STR, s);
    }
    if (!cache[c]) {
        s = (Str *)bh_xmalloc(sizeof(Str) + sizeof(uint32_t));
        memset(s, 0, sizeof(Str));
        s->h.mark = 1;                           /* 不在 bh_all 链上，标记永远是 1，回收器不会碰它 */
        s->len = 1;
        s->cp = (uint32_t *)(s + 1);
        s->cp[0] = c;
        cache[c] = s;
    }
    return bh_obj(T_STR, cache[c]);
}

static int bh_strcmp(const Str *a, const Str *b) {
    int64_t i, n = a->len < b->len ? a->len : b->len;
    for (i = 0; i < n; i++)
        if (a->cp[i] != b->cp[i]) return a->cp[i] < b->cp[i] ? -1 : 1;
    return (a->len > b->len) - (a->len < b->len);
}

/* 一点一点拼出一段文字 */
typedef struct { uint32_t *cp; int64_t len, cap; } SB;

static void sb_reserve(SB *b, int64_t more) {
    if (b->len + more > b->cap) {
        int64_t cap = b->cap ? b->cap * 2 : 32;
        while (cap < b->len + more) cap *= 2;
        b->cp = (uint32_t *)realloc(b->cp, (size_t)cap * sizeof(uint32_t));
        if (!b->cp) bh_fail("内存不够了");
        b->cap = cap;
    }
}
static void sb_put(SB *b, uint32_t c) { sb_reserve(b, 1); b->cp[b->len++] = c; }
static void sb_cp(SB *b, const uint32_t *cp, int64_t len) {
    if (!len) return;
    sb_reserve(b, len);
    memcpy(b->cp + b->len, cp, (size_t)len * sizeof(uint32_t));
    b->len += len;
}
static void sb_str(SB *b, const Str *s) { sb_cp(b, s->cp, s->len); }
static void sb_ascii(SB *b, const char *z) { while (*z) sb_put(b, (unsigned char)*z++); }
static void sb_utf8(SB *b, const char *z) { sb_str(b, bh_decode(z, strlen(z))); }
static Value sb_done(SB *b) {
    Str *s = bh_str_alloc(b->len);
    if (b->len) memcpy(s->cp, b->cp, (size_t)b->len * sizeof(uint32_t));
    free(b->cp);
    return bh_obj(T_STR, s);
}

/* 把两段文字接起来。
 * 循环里常见的写法是 甲 = 甲 + 乙，如果每次都把整段重抄一遍，接一万次就要抄五千万个字。
 * 所以第一次接的时候多申请一倍的空间，之后只要前一段正好是这块空间里最长的那段文字，
 * 就直接把后一段写在它后面。前一段文字自己的长度不变，所以它的内容没有被改动。 */
static Value bh_str_concat(const Str *a, const Str *b) {
    int64_t total = a->len + b->len;
    Str *r = (Str *)bh_new(K_STR, sizeof(Str));
    r->len = total;
    if (a->buf && a->cp == a->buf->cp && a->buf->used == a->len && a->buf->cap - a->len >= b->len) {
        if (b->len) memcpy(a->buf->cp + a->len, b->cp, (size_t)b->len * sizeof(uint32_t));
        r->buf = a->buf;
    } else {
        int64_t cap = total < 16 ? 32 : total * 2;
        Buf *nb = (Buf *)bh_new(K_BUF, sizeof(Buf) + (size_t)cap * sizeof(uint32_t));
        nb->cap = cap;
        if (a->len) memcpy(nb->cp, a->cp, (size_t)a->len * sizeof(uint32_t));
        if (b->len) memcpy(nb->cp + a->len, b->cp, (size_t)b->len * sizeof(uint32_t));
        r->buf = nb;
    }
    r->buf->used = total;
    r->cp = r->buf->cp;
    return bh_obj(T_STR, r);
}

/* ---------- 列表 ---------- */

static List *bh_list_alloc(int64_t cap) {
    List *l = (List *)bh_new(K_LIST, sizeof(List));
    l->len = 0;
    l->cap = cap > 4 ? cap : 4;
    l->items = (Value *)bh_alloc((size_t)l->cap * sizeof(Value));
    bh_since_gc += (size_t)l->cap * sizeof(Value);
    return l;
}
static void bh_push(List *l, Value v) {
    if (l->len == l->cap) {
        Value *items = (Value *)bh_alloc((size_t)l->cap * 2 * sizeof(Value));
        bh_since_gc += (size_t)l->cap * sizeof(Value);
        memcpy(items, l->items, (size_t)l->len * sizeof(Value));
        bh_release(l->items, (size_t)l->cap * sizeof(Value));
        l->items = items;
        l->cap *= 2;
    }
    l->items[l->len++] = v;
}
static Value bh_list_make(int64_t n, const Value *items) {
    List *l = bh_list_alloc(n);
    if (n) memcpy(l->items, items, (size_t)n * sizeof(Value));
    l->len = n;
    return bh_obj(T_LIST, l);
}
static Value bh_list_at(Value list, int64_t i) { return ((List *)list.u.p)->items[i]; }
static int64_t bh_list_len(Value list) { return ((List *)list.u.p)->len; }

/* ======================================================================
 * 大整数
 *
 * 平时整数就是一个 64 位的数。只有装不下的时候才换成 Big：把绝对值切成
 * 每 32 位一段，放进数组，低位在前；正负号单独记。加减乘除就是小学竖式，
 * 只不过每一「位」是 0 到 42 亿多，而不是 0 到 9。
 * 算完之后如果又能装进 64 位，就换回普通整数，所以同一个数只有一种表示。
 * ====================================================================== */

/* 参加运算的整数，不管原来是普通整数还是大整数，都先看成「正负号 + 若干段」 */
typedef struct { int neg; int64_t n; const uint32_t *d; uint32_t small[2]; } BigRef;

static void big_ref(BigRef *r, Value v) {
    if (v.tag == T_BIG) {
        Big *b = (Big *)v.u.p;
        r->neg = b->neg; r->n = b->n; r->d = b->d;
    } else {
        uint64_t m = v.u.i < 0 ? (uint64_t)0 - (uint64_t)v.u.i : (uint64_t)v.u.i;
        r->neg = v.u.i < 0;
        r->small[0] = (uint32_t)m;
        r->small[1] = (uint32_t)(m >> 32);
        r->n = r->small[1] ? 2 : r->small[0] ? 1 : 0;
        r->d = r->small;
    }
}

/* 把算出来的结果变成值：去掉高位的 0，能装进 64 位就用普通整数 */
static Value big_make(int neg, const uint32_t *d, int64_t n) {
    Big *b;
    while (n > 0 && d[n - 1] == 0) n--;
    if (n <= 2) {
        uint64_t m = n == 0 ? 0 : n == 1 ? d[0] : ((uint64_t)d[1] << 32) | d[0];
        if (!neg && m <= (uint64_t)INT64_MAX) return bh_int((int64_t)m);
        if (neg && m <= (uint64_t)INT64_MAX + 1) return bh_int(m == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)m);
    }
    b = (Big *)bh_new(K_BIG, sizeof(Big) + (size_t)n * sizeof(uint32_t));
    b->neg = neg;
    b->n = n;
    memcpy(b->d, d, (size_t)n * sizeof(uint32_t));
    return bh_obj(T_BIG, b);
}

static int mag_cmp(const uint32_t *a, int64_t na, const uint32_t *b, int64_t nb) {
    int64_t i;
    if (na != nb) return na > nb ? 1 : -1;
    for (i = na - 1; i >= 0; i--)
        if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
    return 0;
}

static int64_t mag_add(uint32_t *r, const uint32_t *a, int64_t na, const uint32_t *b, int64_t nb) {
    uint64_t carry = 0;
    int64_t i, n = na > nb ? na : nb;
    for (i = 0; i < n; i++) {
        carry += (uint64_t)(i < na ? a[i] : 0) + (i < nb ? b[i] : 0);
        r[i] = (uint32_t)carry;
        carry >>= 32;
    }
    r[n] = (uint32_t)carry;
    return n + 1;
}

static int64_t mag_sub(uint32_t *r, const uint32_t *a, int64_t na, const uint32_t *b, int64_t nb) {   /* 要求 a >= b */
    int64_t borrow = 0, i;
    for (i = 0; i < na; i++) {
        int64_t t = (int64_t)a[i] - (i < nb ? (int64_t)b[i] : 0) - borrow;
        borrow = t < 0;
        r[i] = (uint32_t)t;
    }
    return na;
}

static uint32_t mag_divmod1(uint32_t *q, const uint32_t *a, int64_t na, uint32_t b) {   /* 除数只有一段 */
    uint64_t rem = 0;
    int64_t i;
    for (i = na - 1; i >= 0; i--) {
        uint64_t cur = (rem << 32) | a[i];
        q[i] = (uint32_t)(cur / b);
        rem = cur % b;
    }
    return (uint32_t)rem;
}

/* 多段除以多段（nb >= 2，na >= nb）。经典的「算法 D」：先估一位商，再乘回去减，估大了就加回来 */
static void mag_divmod(uint32_t *q, uint32_t *r, const uint32_t *u, int64_t na, const uint32_t *v, int64_t nb) {
    uint32_t *un = (uint32_t *)bh_xmalloc((size_t)(na + 1) * sizeof(uint32_t));
    uint32_t *vn = (uint32_t *)bh_xmalloc((size_t)nb * sizeof(uint32_t));
    int64_t i, j;
    int s = 0;
    while (!((v[nb - 1] << s) & 0x80000000u)) s++;          /* 先把除数往左挪，让最高位是 1 */
    for (i = nb - 1; i > 0; i--) vn[i] = (v[i] << s) | (s ? v[i - 1] >> (32 - s) : 0);
    vn[0] = v[0] << s;
    un[na] = s ? u[na - 1] >> (32 - s) : 0;
    for (i = na - 1; i > 0; i--) un[i] = (u[i] << s) | (s ? u[i - 1] >> (32 - s) : 0);
    un[0] = u[0] << s;
    for (j = na - nb; j >= 0; j--) {
        uint64_t num = ((uint64_t)un[j + nb] << 32) | un[j + nb - 1];
        uint64_t qhat = num / vn[nb - 1], rhat = num % vn[nb - 1], p;
        int64_t k = 0, t;
        while (qhat >= 0x100000000ULL || qhat * vn[nb - 2] > ((rhat << 32) | un[j + nb - 2])) {
            qhat--;
            rhat += vn[nb - 1];
            if (rhat >= 0x100000000ULL) break;
        }
        for (i = 0; i < nb; i++) {
            p = qhat * vn[i];
            t = (int64_t)un[i + j] - k - (int64_t)(p & 0xFFFFFFFFu);
            un[i + j] = (uint32_t)t;
            k = (int64_t)(p >> 32) - (t >> 32);
        }
        t = (int64_t)un[j + nb] - k;
        un[j + nb] = (uint32_t)t;
        q[j] = (uint32_t)qhat;
        if (t < 0) {                                          /* 估大了一：加回来 */
            uint64_t carry = 0;
            q[j]--;
            for (i = 0; i < nb; i++) {
                carry += (uint64_t)un[i + j] + vn[i];
                un[i + j] = (uint32_t)carry;
                carry >>= 32;
            }
            un[j + nb] += (uint32_t)carry;
        }
    }
    for (i = 0; i < nb; i++) r[i] = (un[i] >> s) | (s ? un[i + 1] << (32 - s) : 0);
    free(un);
    free(vn);
}

/* 取最高的三段，算出近似的小数值。lo 是被略去的低位段数 */
static double mag_top(const uint32_t *d, int64_t n, int64_t *lo) {
    double x = 0;
    int64_t i;
    *lo = n > 3 ? n - 3 : 0;
    for (i = n - 1; i >= *lo; i--) x = x * 4294967296.0 + (double)d[i];
    return x;
}

static double mag_to_double(const uint32_t *d, int64_t n) {       /* 太大就得到无穷大 */
    int64_t lo;
    double x = mag_top(d, n, &lo);
    return ldexp(x, lo > 40 ? 1280 : (int)(lo * 32));
}

static double mag_ratio(const uint32_t *a, int64_t na, const uint32_t *b, int64_t nb) {   /* a / b，要求 a < b */
    int64_t la, lb;
    double x = mag_top(a, na, &la), y = mag_top(b, nb, &lb);
    return lb - la > 40 ? 0.0 : ldexp(x / y, (int)((la - lb) * 32));
}

static int big_cmp(const BigRef *x, const BigRef *y) {
    int c;
    if (x->n == 0 && y->n == 0) return 0;
    if (x->neg != y->neg) return x->neg ? -1 : 1;
    c = mag_cmp(x->d, x->n, y->d, y->n);
    return x->neg ? -c : c;
}

static Value big_addsub(const BigRef *x, const BigRef *y, int subtract) {
    int yneg = y->neg ^ subtract, c;
    uint32_t *r = (uint32_t *)bh_xmalloc((size_t)((x->n > y->n ? x->n : y->n) + 1) * sizeof(uint32_t));
    Value v;
    if (x->neg == yneg) {
        v = big_make(x->neg, r, mag_add(r, x->d, x->n, y->d, y->n));
    } else {
        c = mag_cmp(x->d, x->n, y->d, y->n);
        if (c >= 0) v = big_make(x->neg, r, mag_sub(r, x->d, x->n, y->d, y->n));
        else v = big_make(yneg, r, mag_sub(r, y->d, y->n, x->d, x->n));
    }
    free(r);
    return v;
}

static Value big_mul(const BigRef *x, const BigRef *y) {
    uint32_t *r = (uint32_t *)bh_xmalloc((size_t)(x->n + y->n + 1) * sizeof(uint32_t));
    int64_t i, j;
    Value v;
    memset(r, 0, (size_t)(x->n + y->n + 1) * sizeof(uint32_t));
    for (i = 0; i < x->n; i++) {
        uint64_t carry = 0;
        for (j = 0; j < y->n; j++) {
            carry += (uint64_t)x->d[i] * y->d[j] + r[i + j];
            r[i + j] = (uint32_t)carry;
            carry >>= 32;
        }
        r[i + y->n] = (uint32_t)carry;
    }
    v = big_make(x->neg ^ y->neg, r, x->n + y->n);
    free(r);
    return v;
}

/* 除法和取余数。op 是 '/' 或 '%'。规则和普通整数一样：
 * 能整除就得到整数，否则得到小数；余数的正负跟着除数走 */
static Value big_divmod(int op, const BigRef *x, const BigRef *y) {
    uint32_t *q, *r;
    int64_t qn, rn;
    Value v;
    if (y->n == 0) bh_fail("除数不能是 0");
    q = (uint32_t *)bh_xmalloc((size_t)(x->n + 1) * sizeof(uint32_t));
    r = (uint32_t *)bh_xmalloc((size_t)(y->n + x->n + 1) * sizeof(uint32_t));
    if (mag_cmp(x->d, x->n, y->d, y->n) < 0) {
        qn = 0;
        rn = x->n;
        if (rn) memcpy(r, x->d, (size_t)rn * sizeof(uint32_t));
    } else if (y->n == 1) {
        r[0] = mag_divmod1(q, x->d, x->n, y->d[0]);
        qn = x->n;
        rn = 1;
    } else {
        mag_divmod(q, r, x->d, x->n, y->d, y->n);
        qn = x->n - y->n + 1;
        rn = y->n;
    }
    while (qn > 0 && q[qn - 1] == 0) qn--;
    while (rn > 0 && r[rn - 1] == 0) rn--;
    if (op == '/') {
        if (rn == 0) {
            v = big_make(x->neg ^ y->neg, q, qn);
        } else {
            double result = mag_to_double(q, qn) + mag_ratio(r, rn, y->d, y->n);
            if (isinf(result)) bh_fail(BH_TOO_BIG);
            v = bh_float(x->neg ^ y->neg ? -result : result);
        }
    } else if (rn == 0) {
        v = bh_int(0);
    } else if (x->neg == y->neg) {
        v = big_make(x->neg, r, rn);
    } else {
        uint32_t *t = (uint32_t *)bh_xmalloc((size_t)y->n * sizeof(uint32_t));
        v = big_make(y->neg, t, mag_sub(t, y->d, y->n, r, rn));
        free(t);
    }
    free(q);
    free(r);
    return v;
}

/* 把一个没有小数部分的小数，原原本本地变成「正负号 + 若干段」。buf 要有 36 段 */
static void big_from_whole(BigRef *r, uint32_t *buf, double whole) {
    int e, shift;
    double m = frexp(fabs(whole), &e);
    uint64_t mant;
    int64_t n = 36;
    memset(buf, 0, 36 * sizeof(uint32_t));
    r->neg = whole < 0;
    r->d = buf;
    r->n = 0;
    if (whole == 0) return;
    mant = (uint64_t)ldexp(m, 53);              /* 小数的 53 位有效数字 */
    shift = e - 53;                             /* whole = mant * 2^shift */
    if (shift <= 0) {
        mant >>= -shift;
        buf[0] = (uint32_t)mant;
        buf[1] = (uint32_t)(mant >> 32);
    } else {
        int limb = shift / 32, bit = shift % 32;
        buf[limb] = (uint32_t)(mant << bit);
        buf[limb + 1] = (uint32_t)(bit ? mant >> (32 - bit) : mant >> 32);
        buf[limb + 2] = bit ? (uint32_t)(mant >> (64 - bit)) : 0;
    }
    while (n > 0 && buf[n - 1] == 0) n--;
    r->n = n;
}

static Value bh_from_double(double d) {         /* 小数 -> 整数，去掉小数部分。d 不能是无穷大 */
    BigRef r;
    uint32_t buf[36];
    big_from_whole(&r, buf, trunc(d));
    return big_make(r.neg, buf, r.n);
}

/* 整数和小数比大小，不丢精度。返回 -1、0、1。d 不能是无穷大 */
static int bh_cmp_int_double(Value iv, double d) {
    BigRef x, y;
    uint32_t buf[36];
    double whole = floor(d);
    int c;
    big_ref(&x, iv);
    big_from_whole(&y, buf, whole);
    c = big_cmp(&x, &y);
    if (c) return c;
    return d > whole ? -1 : 0;
}

static void big_show(SB *b, const Big *x) {
    int64_t n = x->n, k = 0, i;
    uint32_t *tmp = (uint32_t *)bh_xmalloc((size_t)n * sizeof(uint32_t));
    uint32_t *chunks = (uint32_t *)bh_xmalloc((size_t)(n * 32 / 29 + 2) * sizeof(uint32_t));
    char buf[16];
    memcpy(tmp, x->d, (size_t)n * sizeof(uint32_t));
    while (n > 0) {                             /* 每次除以十亿，余数就是最低的九位十进制数字 */
        chunks[k++] = mag_divmod1(tmp, tmp, n, 1000000000u);
        while (n > 0 && tmp[n - 1] == 0) n--;
    }
    if (x->neg) sb_put(b, '-');
    for (i = k - 1; i >= 0; i--) {
        snprintf(buf, sizeof buf, i == k - 1 ? "%u" : "%09u", (unsigned)chunks[i]);
        sb_ascii(b, buf);
    }
    free(tmp);
    free(chunks);
}

/* 十进制数字（可以带正负号）-> 整数，多长都行 */
static Value bh_big_lit(const char *z) {
    int neg = 0;
    uint32_t *d;
    int64_t n = 0, i;
    Value v;
    if (*z == '+' || *z == '-') { neg = *z == '-'; z++; }
    d = (uint32_t *)bh_xmalloc((strlen(z) / 9 + 2) * sizeof(uint32_t));
    for (; *z; z++) {
        uint64_t carry = (uint64_t)(*z - '0');
        for (i = 0; i < n; i++) {
            carry += (uint64_t)d[i] * 10;
            d[i] = (uint32_t)carry;
            carry >>= 32;
        }
        if (carry) d[n++] = (uint32_t)carry;
    }
    v = big_make(neg, d, n);
    free(d);
    return v;
}

static Value bh_big_arith(int op, Value a, Value b) {
    BigRef x, y;
    big_ref(&x, a);
    big_ref(&y, b);
    if (op == '+') return big_addsub(&x, &y, 0);
    if (op == '-') return big_addsub(&x, &y, 1);
    if (op == '*') return big_mul(&x, &y);
    return big_divmod(op, &x, &y);
}

/* ---------- 把值变成给人看的文字 ---------- */

static const char *bh_tn(Value v) {
    switch (v.tag) {
    case T_NIL: return "空";
    case T_BOOL: return "真假";
    case T_INT: case T_BIG: return "整数";
    case T_FLOAT: return "小数";
    case T_STR: return "文字";
    case T_LIST: return "列表";
    case T_DICT: return "字典";
    default: return "函数";
    }
}

/* 整数写成十进制，返回写了几个字符。比 printf 快得多 */
static int bh_itoa(int64_t v, char *out) {
    char tmp[24];
    int n = 0, k = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do { tmp[n++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
    if (v < 0) out[k++] = '-';
    while (n) out[k++] = tmp[--n];
    out[k] = 0;
    return k;
}

/* 一串英文字符做成文字。只有一个字符时用现成的那份 */
static Value bh_ascii_str(const char *z, size_t n) {
    Str *s;
    size_t i;
    if (n == 1) return bh_char((unsigned char)z[0]);
    s = bh_str_alloc((int64_t)n);
    for (i = 0; i < n; i++) s->cp[i] = (unsigned char)z[i];
    return bh_obj(T_STR, s);
}

static void bh_show_into(SB *b, Value v, int quote, int depth) {
    char buf[64];
    switch (v.tag) {
    case T_NIL: sb_put(b, 0x7A7A); return;                       /* 空 */
    case T_BOOL: sb_put(b, v.u.i ? 0x771F : 0x5047); return;     /* 真 假 */
    case T_INT: bh_itoa(v.u.i, buf); sb_ascii(b, buf); return;
    case T_BIG: big_show(b, (Big *)v.u.p); return;
    case T_FLOAT: snprintf(buf, sizeof buf, "%.12g", v.u.f); sb_ascii(b, buf); return;
    case T_STR:
        if (quote) sb_put(b, 0x201C);
        sb_str(b, (Str *)v.u.p);
        if (quote) sb_put(b, 0x201D);
        return;
    case T_LIST: {
        List *l = (List *)v.u.p;
        int64_t i;
        if (depth > 500) bh_fail(BH_TOO_DEEP);
        sb_put(b, 0x3010);                                        /* 【 */
        for (i = 0; i < l->len; i++) {
            if (i) sb_put(b, 0xFF0C);                             /* ， */
            bh_show_into(b, l->items[i], 1, depth + 1);
        }
        sb_put(b, 0x3011);                                        /* 】 */
        return;
    }
    case T_DICT: {                                                /* 【“甲”：1，“乙”：2】，空字典是【：】 */
        Dict *d = (Dict *)v.u.p;
        int64_t i, k = 0;
        if (depth > 500) bh_fail(BH_TOO_DEEP);
        sb_put(b, 0x3010);
        if (d->len == 0) sb_put(b, 0xFF1A);
        for (i = 0; i < d->used; i++) {
            if (d->e[i].key.tag == T_UNDEF) continue;
            if (k++) sb_put(b, 0xFF0C);
            bh_show_into(b, d->e[i].key, 1, depth + 1);
            sb_put(b, 0xFF1A);
            bh_show_into(b, d->e[i].val, 1, depth + 1);
        }
        sb_put(b, 0x3011);
        return;
    }
    case T_FUNC: sb_utf8(b, "<函数 "); sb_utf8(b, ((Func *)v.u.p)->name); sb_put(b, '>'); return;
    case T_BUILTIN: sb_utf8(b, "<内置函数 "); sb_utf8(b, ((Builtin *)v.u.p)->name); sb_put(b, '>'); return;
    default: sb_put(b, '?');
    }
}

static Value bh_show(Value v, int quote) {
    SB b = {NULL, 0, 0};
    char buf[64];
    if (v.tag == T_STR && !quote) return v;                     /* 文字不会被改动，不用抄一份 */
    if (v.tag == T_INT) return bh_ascii_str(buf, (size_t)bh_itoa(v.u.i, buf));
    if (v.tag == T_FLOAT) {
        snprintf(buf, sizeof buf, "%.12g", v.u.f);
        return bh_ascii_str(buf, strlen(buf));
    }
    bh_show_into(&b, v, quote, 0);
    return sb_done(&b);
}

static const char *bh_shown(Value v) { return bh_cstr((Str *)bh_show(v, 1).u.p); }   /* 只在报错信息里用 */

/* ---------- 运算 ---------- */

static int bh_isint(Value v) { return v.tag == T_INT || v.tag == T_BIG; }
static int bh_isnum(Value v) { return v.tag == T_INT || v.tag == T_BIG || v.tag == T_FLOAT; }

static double bh_tof(Value v) {
    double d;
    if (v.tag == T_FLOAT) return v.u.f;
    if (v.tag != T_BIG) return (double)v.u.i;
    d = mag_to_double(((Big *)v.u.p)->d, ((Big *)v.u.p)->n);
    if (isinf(d)) bh_fail(BH_TOO_BIG);
    return ((Big *)v.u.p)->neg ? -d : d;
}

/* 大整数当成普通整数用（下标、次数等）：太大的按 64 位的最大、最小值算 */
static int64_t bh_small(Value v) {
    if (v.tag == T_BIG) return ((Big *)v.u.p)->neg ? INT64_MIN : INT64_MAX;
    return v.u.i;
}

static BH_NOINLINE int bh_truthy_slow(Value v) {
    switch (v.tag) {
    case T_NIL: return 0;
    case T_BOOL: case T_INT: return v.u.i != 0;
    case T_FLOAT: return v.u.f != 0.0;
    case T_STR: return ((Str *)v.u.p)->len != 0;
    case T_LIST: return ((List *)v.u.p)->len != 0;
    case T_DICT: return ((Dict *)v.u.p)->len != 0;
    default: return 1;
    }
}


BH_INLINE int bh_truthy(Value v) {
    if (BH_LIKELY(v.tag == T_BOOL)) return v.u.i != 0;
    return bh_truthy_slow(v);
}

static BH_NOINLINE Value bh_arith(int op, Value a, Value b) {
    if (!(bh_isnum(a) && bh_isnum(b))) {
        const char *name = op == '+' ? "加" : op == '-' ? "减" : op == '*' ? "乘" : op == '/' ? "除" : "取余数";
        bh_fail("%s和%s之间不能%s", bh_tn(a), bh_tn(b), name);
    }
    if (a.tag == T_INT && b.tag == T_INT) {       /* 最常见的情况：两个普通整数。装不下再换成大整数 */
        int64_t x = a.u.i, y = b.u.i, r;
        switch (op) {
        case '+':
            if (!((y > 0 && x > INT64_MAX - y) || (y < 0 && x < INT64_MIN - y))) return bh_int(x + y);
            break;
        case '-':
            if (!((y < 0 && x > INT64_MAX + y) || (y > 0 && x < INT64_MIN + y))) return bh_int(x - y);
            break;
        case '*':
            if (x > 0 ? (y > 0 ? x <= INT64_MAX / y : y >= INT64_MIN / x)
                      : x < 0 ? (y > 0 ? x >= INT64_MIN / y : y >= INT64_MAX / x) : 1) return bh_int(x * y);
            break;
        case '/':
            if (y == 0) bh_fail("除数不能是 0");
            if (x == INT64_MIN && y == -1) break;
            if (x % y == 0) return bh_int(x / y);         /* 能整除就保持整数 */
            return bh_float((double)x / (double)y);
        default:
            if (y == 0) bh_fail("除数不能是 0");
            if (y == -1) return bh_int(0);
            r = x % y;
            if (r != 0 && ((r < 0) != (y < 0))) r += y;   /* 余数的正负跟着除数走 */
            return bh_int(r);
        }
        return bh_big_arith(op, a, b);
    }
    if (a.tag != T_FLOAT && b.tag != T_FLOAT) return bh_big_arith(op, a, b);
    {
        double x = bh_tof(a), y = bh_tof(b), r;
        switch (op) {
        case '+': return bh_float(x + y);
        case '-': return bh_float(x - y);
        case '*': return bh_float(x * y);
        case '/':
            if (y == 0) bh_fail("除数不能是 0");
            return bh_float(x / y);
        default:
            if (y == 0) bh_fail("除数不能是 0");
            r = fmod(x, y);
            if (r != 0) { if ((r < 0) != (y < 0)) r += y; }
            else r = copysign(0.0, y);
            return bh_float(r);
        }
    }
}

static Value bh_repeat(Value seq, int64_t times) {
    int64_t i;
    if (times < 0) times = 0;
    if (seq.tag == T_STR) {
        SB b = {NULL, 0, 0};
        for (i = 0; i < times; i++) sb_str(&b, (Str *)seq.u.p);
        return sb_done(&b);
    } else {
        List *src = (List *)seq.u.p, *out = bh_list_alloc(src->len * times);
        int64_t j;
        for (i = 0; i < times; i++)
            for (j = 0; j < src->len; j++) bh_push(out, src->items[j]);
        return bh_obj(T_LIST, out);
    }
}

static BH_NOINLINE Value bh_add_slow(Value a, Value b) {
    if (a.tag == T_STR || b.tag == T_STR) {               /* 文字 + 任何东西 = 拼在一起 */
        if (a.tag != T_STR) a = bh_show(a, 0);
        if (b.tag != T_STR) b = bh_show(b, 0);
        return bh_str_concat((Str *)a.u.p, (Str *)b.u.p);
    }
    if (a.tag == T_LIST && b.tag == T_LIST) {
        List *x = (List *)a.u.p, *y = (List *)b.u.p, *out = bh_list_alloc(x->len + y->len);
        int64_t i;
        for (i = 0; i < x->len; i++) bh_push(out, x->items[i]);
        for (i = 0; i < y->len; i++) bh_push(out, y->items[i]);
        return bh_obj(T_LIST, out);
    }
    return bh_arith('+', a, b);
}
static BH_NOINLINE Value bh_mul_slow(Value a, Value b) {     /* 文字或列表 乘以 整数 = 重复几遍 */
    if ((a.tag == T_STR || a.tag == T_LIST) && b.tag == T_INT) return bh_repeat(a, b.u.i);
    if ((b.tag == T_STR || b.tag == T_LIST) && a.tag == T_INT) return bh_repeat(b, a.u.i);
    return bh_arith('*', a, b);
}


/* ---------- 快速路径 ----------
 * 下面这些是生成的代码最常调用的操作。最常见的情况（两个普通整数、列表配整数下标……）
 * 直接在调用的地方算完；别的情况交给上面那些慢一点、但什么都能处理的函数。 */

BH_INLINE int bh_add_ok(int64_t x, int64_t y, int64_t *r) {
#ifdef BH_HAVE_OVERFLOW_BUILTINS
    return !__builtin_add_overflow(x, y, r);
#else
    if ((y > 0 && x > INT64_MAX - y) || (y < 0 && x < INT64_MIN - y)) return 0;
    *r = x + y;
    return 1;
#endif
}
BH_INLINE int bh_sub_ok(int64_t x, int64_t y, int64_t *r) {
#ifdef BH_HAVE_OVERFLOW_BUILTINS
    return !__builtin_sub_overflow(x, y, r);
#else
    if ((y < 0 && x > INT64_MAX + y) || (y > 0 && x < INT64_MIN + y)) return 0;
    *r = x - y;
    return 1;
#endif
}
BH_INLINE int bh_mul_ok(int64_t x, int64_t y, int64_t *r) {
#ifdef BH_HAVE_OVERFLOW_BUILTINS
    return !__builtin_mul_overflow(x, y, r);
#else
    if (!(x > 0 ? (y > 0 ? x <= INT64_MAX / y : y >= INT64_MIN / x)
                : x < 0 ? (y > 0 ? x >= INT64_MIN / y : y >= INT64_MAX / x) : 1)) return 0;
    *r = x * y;
    return 1;
#endif
}

BH_INLINE Value bh_add(Value a, Value b) {
    int64_t r;
    if (BH_LIKELY(a.tag == T_INT && b.tag == T_INT) && BH_LIKELY(bh_add_ok(a.u.i, b.u.i, &r))) return bh_int(r);
    return bh_add_slow(a, b);
}
BH_INLINE Value bh_sub(Value a, Value b) {
    int64_t r;
    if (BH_LIKELY(a.tag == T_INT && b.tag == T_INT) && BH_LIKELY(bh_sub_ok(a.u.i, b.u.i, &r))) return bh_int(r);
    return bh_arith('-', a, b);
}
BH_INLINE Value bh_mul(Value a, Value b) {
    int64_t r;
    if (BH_LIKELY(a.tag == T_INT && b.tag == T_INT) && BH_LIKELY(bh_mul_ok(a.u.i, b.u.i, &r))) return bh_int(r);
    return bh_mul_slow(a, b);
}
BH_INLINE Value bh_div(Value a, Value b) {
    if (BH_LIKELY(a.tag == T_INT && b.tag == T_INT && b.u.i > 0)) {
        if (a.u.i % b.u.i == 0) return bh_int(a.u.i / b.u.i);       /* 能整除就保持整数 */
        return bh_float((double)a.u.i / (double)b.u.i);
    }
    return bh_arith('/', a, b);
}
BH_INLINE Value bh_mod(Value a, Value b) {
    if (BH_LIKELY(a.tag == T_INT && b.tag == T_INT && b.u.i > 0)) {
        int64_t r = a.u.i % b.u.i;
        if (r < 0) r += b.u.i;                                   /* 余数的正负跟着除数走 */
        return bh_int(r);
    }
    return bh_arith('%', a, b);
}

static Value bh_neg(Value v) {
    BigRef r;
    if (!bh_isnum(v)) bh_fail("负号只能用在数字前面，这里是%s", bh_tn(v));
    if (v.tag == T_FLOAT) return bh_float(-v.u.f);
    if (v.tag == T_INT && v.u.i != INT64_MIN) return bh_int(-v.u.i);
    big_ref(&r, v);
    return big_make(!r.neg, r.d, r.n);
}

/* 两个数比大小，返回 -1、0、1。真假在这里当作 1 和 0。比不了（有一边不是数）时 *bad 置 1 */
static int bh_num_cmp(Value a, Value b, int *bad) {
    BigRef x, y;
    if (a.tag == T_FLOAT || b.tag == T_FLOAT) {
        double d;
        if (a.tag == T_FLOAT && b.tag == T_FLOAT) {
            if (a.u.f < b.u.f) return -1;
            if (a.u.f > b.u.f) return 1;
            if (a.u.f != b.u.f) *bad = 1;
            return 0;
        }
        if (a.tag == T_FLOAT) return -bh_num_cmp(b, a, bad);
        d = b.u.f;
        if (d != d) { *bad = 1; return 0; }
        if (isinf(d)) return d > 0 ? -1 : 1;
        return bh_cmp_int_double(a, d);
    }
    if (a.tag != T_BIG && b.tag != T_BIG) return (a.u.i > b.u.i) - (a.u.i < b.u.i);
    big_ref(&x, a);
    big_ref(&y, b);
    return big_cmp(&x, &y);
}

/* ======================================================================
 * 字典
 *
 * 和 Python 一样：条目保持放进去的先后顺序；1、1.0、真 算同一个键（它们相等）。
 * 所以散列值也要让相等的数一样：整数（不管多大）、值是整数的小数，都按
 * 「绝对值除以 2^61-1 的余数」来算。
 * ====================================================================== */

static int bh_same(Value a, Value b, int depth);

#define BH_HASH_P ((((uint64_t)1) << 61) - 1)

static uint64_t bh_rot61(uint64_t x, int k) {        /* x 乘以 2 的 k 次方，除以 2^61-1 的余数（x 比 2^61-1 小） */
    k %= 61;
    if (k == 0) return x;
    return ((x << k) & BH_HASH_P) | (x >> (61 - k));
}

static uint64_t bh_hash_mix(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

static uint64_t bh_hash_num(int neg, uint64_t m) {     /* m 是绝对值除以 2^61-1 的余数 */
    if (neg && m) m = BH_HASH_P - m;
    return bh_hash_mix(m ^ 0x2545F4914F6CDD1DULL);
}

static uint64_t bh_hash_i64(int64_t v) {
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    return bh_hash_num(v < 0, u % BH_HASH_P);
}

static uint64_t bh_hash(Value v) {
    switch (v.tag) {
    case T_NIL: return 0x9E3779B97F4A7C15ULL;
    case T_BOOL: case T_INT: return bh_hash_i64(v.u.i);
    case T_BIG: {
        Big *b = (Big *)v.u.p;
        uint64_t h = 0;
        int64_t i;
        for (i = b->n - 1; i >= 0; i--) {
            h = bh_rot61(h, 32) + b->d[i];
            if (h >= BH_HASH_P) h -= BH_HASH_P;
        }
        return bh_hash_num(b->neg, h);
    }
    case T_FLOAT: {
        double d = v.u.f, f;
        uint64_t bits;
        int ex;
        if (isinf(d)) return d > 0 ? 0x7FF0000000000001ULL : 0xFFF0000000000001ULL;
        if (d == floor(d)) {                                   /* 值是整数的小数：和同样大的整数一样 */
            if (d >= -9223372036854775808.0 && d < 9223372036854775808.0) return bh_hash_i64((int64_t)d);
            f = frexp(fabs(d), &ex);                           /* |d| = m * 2^(ex-53)，m 是 53 位的整数 */
            return bh_hash_num(d < 0, bh_rot61((uint64_t)ldexp(f, 53), ex - 53));
        }
        memcpy(&bits, &d, sizeof bits);
        return bh_hash_mix(bits ^ 0x9E3779B97F4A7C15ULL);
    }
    case T_STR: {
        Str *s = (Str *)v.u.p;
        uint64_t h = 1469598103934665603ULL;
        int64_t i;
        for (i = 0; i < s->len; i++) { h ^= s->cp[i]; h *= 1099511628211ULL; }
        return bh_hash_mix(h);
    }
    default: return 0;
    }
}

static void bh_check_key(Value k) {
    if (k.tag == T_FLOAT && k.u.f != k.u.f) bh_fail("nan 不能当字典的键");
    if (!(k.tag == T_NIL || k.tag == T_BOOL || k.tag == T_INT || k.tag == T_BIG || k.tag == T_FLOAT || k.tag == T_STR))
        bh_fail("%s不能当字典的键（只有数字、文字、真假和空可以）", bh_tn(k));
}

static Dict *bh_dict_alloc(void) {
    Dict *d = (Dict *)bh_new(K_DICT, sizeof(Dict));
    d->len = d->used = d->cap = d->nslots = 0;
    d->e = NULL;
    d->slots = NULL;
    return d;
}

/* 找键：返回它在散列表里的位置，找不到返回 -1 */
static int64_t bh_dict_find(const Dict *d, Value k, uint64_t h) {
    uint64_t mask, i;
    if (!d->nslots) return -1;
    mask = (uint64_t)d->nslots - 1;
    for (i = h & mask;; i = (i + 1) & mask) {
        int64_t at = d->slots[i];
        if (at == -1) return -1;
        if (at >= 0 && d->e[at].hash == h && bh_same(d->e[at].key, k, 0)) return (int64_t)i;
    }
}

/* 重新整理：去掉删除过的条目，空间至少能放 want 个。散列表的格数是条目空间的两倍 */
static void bh_dict_rebuild(Dict *d, int64_t want) {
    int64_t cap = 8, ns, i, j = 0;
    DictEntry *e;
    int64_t *slots;
    while (cap < want) cap *= 2;
    ns = cap * 2;
    e = (DictEntry *)bh_xmalloc((size_t)cap * sizeof(DictEntry));
    slots = (int64_t *)bh_xmalloc((size_t)ns * sizeof(int64_t));
    for (i = 0; i < ns; i++) slots[i] = -1;
    for (i = 0; i < d->used; i++) {
        uint64_t p;
        if (d->e[i].key.tag == T_UNDEF) continue;
        e[j] = d->e[i];
        for (p = e[j].hash & (uint64_t)(ns - 1); slots[p] != -1; p = (p + 1) & (uint64_t)(ns - 1)) {}
        slots[p] = j++;
    }
    free(d->e);
    free(d->slots);
    d->e = e;
    d->slots = slots;
    d->cap = cap;
    d->nslots = ns;
    d->used = j;
    bh_since_gc += (size_t)cap * sizeof(DictEntry) + (size_t)ns * sizeof(int64_t);
    if (bh_since_gc > bh_gc_threshold) bh_gc_wanted = 1;
}

static void bh_dict_set(Dict *d, Value k, Value v) {
    uint64_t h, p;
    int64_t at;
    bh_check_key(k);
    h = bh_hash(k);
    at = bh_dict_find(d, k, h);
    if (at >= 0) { d->e[d->slots[at]].val = v; return; }     /* 已经有这个键：只换值，键保持原样 */
    if (d->used == d->cap) bh_dict_rebuild(d, d->len * 2 + 1);
    for (p = h & (uint64_t)(d->nslots - 1); d->slots[p] != -1; p = (p + 1) & (uint64_t)(d->nslots - 1)) {}
    d->slots[p] = d->used;
    d->e[d->used].hash = h;
    d->e[d->used].key = k;
    d->e[d->used].val = v;
    d->used++;
    d->len++;
}

/* 取值。没有这个键时：*found 置 0 */
static Value bh_dict_get(const Dict *d, Value k, int *found) {
    int64_t at;
    bh_check_key(k);
    at = bh_dict_find(d, k, bh_hash(k));
    *found = at >= 0;
    return at >= 0 ? d->e[d->slots[at]].val : BH_NIL;
}

static Value bh_dict_need(const Dict *d, Value k) {
    int found;
    Value v = bh_dict_get(d, k, &found);
    if (!found) bh_fail("字典里没有 %s 这个键", bh_shown(k));
    return v;
}

static Value bh_dict_remove(Dict *d, Value k) {
    int64_t at, entry;
    Value v;
    bh_check_key(k);
    at = bh_dict_find(d, k, bh_hash(k));
    if (at < 0) bh_fail("字典里没有 %s 这个键", bh_shown(k));
    entry = d->slots[at];
    v = d->e[entry].val;
    d->e[entry].key.tag = T_UNDEF;
    d->e[entry].val = BH_NIL;
    d->slots[at] = -2;
    d->len--;
    return v;
}

/* 字典的字面量：【键：值，键：值】，kv 里键和值交替排着 */
static Value bh_dict_make(int64_t pairs, const Value *kv) {
    Dict *d = bh_dict_alloc();
    int64_t i;
    for (i = 0; i < pairs; i++) bh_dict_set(d, kv[2 * i], kv[2 * i + 1]);
    return bh_obj(T_DICT, d);
}

static int bh_numlike(Value v) { return v.tag == T_BOOL || bh_isnum(v); }

/* 列表内部的相等：真假在这里当作 1 和 0 */
static int bh_same(Value a, Value b, int depth) {
    if (bh_numlike(a) && bh_numlike(b)) {
        int bad = 0, c = bh_num_cmp(a, b, &bad);
        return !bad && c == 0;
    }
    if (a.tag != b.tag) return 0;
    switch (a.tag) {
    case T_NIL: return 1;
    case T_STR: return bh_strcmp((Str *)a.u.p, (Str *)b.u.p) == 0;
    case T_LIST: {
        List *x = (List *)a.u.p, *y = (List *)b.u.p;
        int64_t i;
        if (x == y) return 1;
        if (x->len != y->len) return 0;
        if (depth > 500) bh_fail(BH_TOO_DEEP);
        for (i = 0; i < x->len; i++)
            if (!bh_same(x->items[i], y->items[i], depth + 1)) return 0;
        return 1;
    }
    case T_DICT: {                                  /* 键一样、对应的值也相等，先后顺序不管 */
        Dict *x = (Dict *)a.u.p, *y = (Dict *)b.u.p;
        int64_t i, at;
        if (x == y) return 1;
        if (x->len != y->len) return 0;
        if (depth > 500) bh_fail(BH_TOO_DEEP);
        for (i = 0; i < x->used; i++) {
            if (x->e[i].key.tag == T_UNDEF) continue;
            at = bh_dict_find(y, x->e[i].key, x->e[i].hash);
            if (at < 0 || !bh_same(x->e[i].val, y->e[y->slots[at]].val, depth + 1)) return 0;
        }
        return 1;
    }
    default: return a.u.p == b.u.p;
    }
}

static int bh_equal(Value a, Value b) {
    if ((a.tag == T_BOOL) != (b.tag == T_BOOL)) return 0;   /* 真 和 1 不算相等 */
    return bh_same(a, b, 0);
}

/* op：0 等于，1 不等于，2 小于，3 大于，4 小于等于，5 大于等于 */
static BH_NOINLINE int bh_cmp_slow(int op, Value a, Value b) {
    int c, bad = 0;
    if (op == 0) return bh_equal(a, b);
    if (op == 1) return !bh_equal(a, b);
    if (bh_isnum(a) && bh_isnum(b)) {
        c = bh_num_cmp(a, b, &bad);
        if (bad) return 0;
    } else if (a.tag == T_STR && b.tag == T_STR) {
        c = bh_strcmp((Str *)a.u.p, (Str *)b.u.p);
    } else {
        bh_fail("%s和%s没法比大小", bh_tn(a), bh_tn(b));
        return 0;
    }
    return op == 2 ? c < 0 : op == 3 ? c > 0 : op == 4 ? c <= 0 : c >= 0;
}


/* op 是常数，内联以后 switch 会被 C 编译器直接算掉 */
BH_INLINE int bh_cmp(int op, Value a, Value b) {
    if (BH_LIKELY(a.tag == T_INT && b.tag == T_INT)) {
        int64_t x = a.u.i, y = b.u.i;
        switch (op) {
        case 0: return x == y;
        case 1: return x != y;
        case 2: return x < y;
        case 3: return x > y;
        case 4: return x <= y;
        default: return x >= y;
        }
    }
    return bh_cmp_slow(op, a, b);
}

static int64_t bh_check_index(Value i, int64_t len) {
    if (!bh_isint(i)) bh_fail("下标必须是整数，这里是%s", bh_tn(i));
    if (i.tag == T_BIG || i.u.i < -len || i.u.i >= len)
        bh_fail("下标 %s 超出了范围（一共 %lld 个，下标从 0 开始数）", bh_shown(i), (long long)len);
    return i.u.i < 0 ? i.u.i + len : i.u.i;
}

static BH_NOINLINE Value bh_index_slow(Value c, Value i) {
    if (c.tag == T_LIST) {
        List *l = (List *)c.u.p;
        return l->items[bh_check_index(i, l->len)];
    }
    if (c.tag == T_STR) {
        Str *s = (Str *)c.u.p;
        return bh_char(s->cp[bh_check_index(i, s->len)]);
    }
    if (c.tag == T_DICT) return bh_dict_need((Dict *)c.u.p, i);
    bh_fail("只有列表、文字和字典可以用【下标】取值，这里是%s", bh_tn(c));
    return BH_NIL;
}

static BH_NOINLINE void bh_set_index_slow(Value c, Value i, Value v) {
    List *l;
    if (c.tag == T_DICT) { bh_dict_set((Dict *)c.u.p, i, v); return; }
    if (c.tag != T_LIST) bh_fail("只有列表和字典里的东西可以这样修改，这里是%s", bh_tn(c));
    l = (List *)c.u.p;
    l->items[bh_check_index(i, l->len)] = v;
}


BH_INLINE Value bh_index(Value c, Value i) {
    if (BH_LIKELY(c.tag == T_LIST && i.tag == T_INT)) {
        List *l = (List *)c.u.p;
        if (BH_LIKELY((uint64_t)i.u.i < (uint64_t)l->len)) return l->items[i.u.i];
    }
    return bh_index_slow(c, i);
}
BH_INLINE void bh_set_index(Value c, Value i, Value v) {
    if (BH_LIKELY(c.tag == T_LIST && i.tag == T_INT)) {
        List *l = (List *)c.u.p;
        if (BH_LIKELY((uint64_t)i.u.i < (uint64_t)l->len)) { l->items[i.u.i] = v; return; }
    }
    bh_set_index_slow(c, i, v);
}

/* 「对于」用：先把要逐个取出的东西抄一份，循环里改原来的列表也不受影响 */
static Value bh_iter(Value v) {
    if (v.tag == T_LIST) return bh_list_make(((List *)v.u.p)->len, ((List *)v.u.p)->items);
    if (v.tag == T_STR) {
        Str *s = (Str *)v.u.p;
        List *l = bh_list_alloc(s->len);
        int64_t i;
        for (i = 0; i < s->len; i++) bh_push(l, bh_char(s->cp[i]));
        return bh_obj(T_LIST, l);
    }
    if (v.tag == T_DICT) {                          /* 字典：逐个取出键 */
        Dict *d = (Dict *)v.u.p;
        List *l = bh_list_alloc(d->len);
        int64_t i;
        for (i = 0; i < d->used; i++)
            if (d->e[i].key.tag != T_UNDEF) bh_push(l, d->e[i].key);
        return bh_obj(T_LIST, l);
    }
    bh_fail("「对于」只能逐个取出列表、文字或字典里的东西，这里是%s", bh_tn(v));
    return BH_NIL;
}

static int64_t bh_count(Value v) {
    if (!bh_isint(v)) bh_fail("「重复」和「次」中间要放一个整数，这里是%s", bh_tn(v));
    return bh_small(v);
}

/* ---------- 变量 ---------- */

/* 和编译器里的「长关键字」表一致：两个字以上的关键字，长的在前 */
static const char *bh_reserved[] = {
    "否则如果", "里的每个", "中的每个", "大于等于", "小于等于", "不等于",
    "函数", "返回", "全局", "如果", "否则", "对于", "重复", "跳出", "继续",
    "除以", "大于", "小于", "等于", "并且", "或者", "不是", NULL
};

static BH_NOINLINE Value bh_chk_fail(const char *name) {
    int i;
    for (i = 0; bh_reserved[i]; i++)
        if (strstr(name, bh_reserved[i]) && strcmp(name, bh_reserved[i]) != 0)
            bh_fail("找不到名字「%s」，是不是还没有赋值，或者打错字了？\n"
                    "提示：「%s」被当成了一个名字。如果其中的「%s」是单独的一个词，要在它前后加上空格。",
                    name, name, bh_reserved[i]);
    bh_fail("找不到名字「%s」，是不是还没有赋值，或者打错字了？", name);
    return BH_NIL;
}


BH_INLINE Value bh_chk(Value v, const char *name) {
    if (BH_LIKELY(v.tag != T_UNDEF)) return v;
    return bh_chk_fail(name);
}

static void bh_modify_outer(const char *name) {
    bh_fail("函数里面不能直接修改外面的变量「%s」。如果它是最外层的变量，先在函数里写一行：全局 %s", name, name);
}

/* ---------- 函数 ---------- */

static Frame *bh_frame_new(Frame *up, int64_t n) {
    size_t size = sizeof(Frame) + (size_t)n * sizeof(Value);
    Frame *f = (Frame *)bh_new(K_FRAME, size);
    memset(f->v, 0, (size_t)(n + 1) * sizeof(Value));
    f->up = up;
    f->n = n;
    return f;
}

static Value bh_func(Value (*code)(Frame *, Value *), int arity, const char *name, Frame *up) {
    Func *f = (Func *)bh_new(K_FUNC, sizeof(Func));
    f->code = code;
    f->arity = arity;
    f->name = name;
    f->up = up;
    return bh_obj(T_FUNC, f);
}

static BH_NOINLINE Value bh_call_slow(Value f, int n, Value *args, const char *callee) {
    if (f.tag == T_BUILTIN) {
        const Builtin *b = (const Builtin *)f.u.p;
        if (n < b->min || (b->max >= 0 && n > b->max)) {
            if (b->max < 0) bh_fail("「%s」至少需要 %d 个参数，这里给了 %d 个", b->name, b->min, n);
            if (b->min == b->max) bh_fail("「%s」需要 %d 个参数，这里给了 %d 个", b->name, b->min, n);
            bh_fail("「%s」需要 %d 到 %d 个参数，这里给了 %d 个", b->name, b->min, b->max, n);
        }
        return b->fn(n, args);
    }
    if (f.tag == T_FUNC) {       /* 个数对的情况在 bh_call 里就处理完了，到这里一定是个数不对 */
        Func *fn = (Func *)f.u.p;
        bh_fail("函数「%s」需要 %d 个参数，这里给了 %d 个", fn->name, fn->arity, n);
    }
    if (callee) bh_fail("「%s」是%s，不是函数，不能加括号调用", callee, bh_tn(f));
    bh_fail("这个值是%s，不是函数，不能加括号调用", bh_tn(f));
    return BH_NIL;
}

/* 调用函数。最常见的情况是调用白话函数、参数个数也对：
 * 检查栈还够不够用，记下这次调用（出错时显示调用过程用），然后跳过去 */
BH_INLINE Value bh_call(Value f, int n, Value *args, const char *callee) {
    if (BH_LIKELY(f.tag == T_FUNC) && BH_LIKELY(n == ((Func *)f.u.p)->arity)) {
        Func *fn = (Func *)f.u.p;
        BhCall rec;
        Value result;
        uintptr_t at = (uintptr_t)&rec;
        if (BH_UNLIKELY((at < bh_stack_base ? bh_stack_base - at : at - bh_stack_base) > bh_stack_limit)) bh_fail(BH_TOO_DEEP);
        rec.prev = bh_calls;
        rec.name = fn->name;
        rec.line = bh_line;
        bh_calls = &rec;
        result = fn->code(fn->up, args);
        bh_calls = rec.prev;
        bh_line = rec.line;
        return result;
    }
    return bh_call_slow(f, n, args, callee);
}

/* ---------- 内置函数 ---------- */

static void bh_print_to(FILE *f, int n, Value *a) {
    SB b = {NULL, 0, 0};
    int i;
    for (i = 0; i < n; i++) {
        if (i) sb_put(&b, ' ');
        bh_show_into(&b, a[i], 0, 0);
    }
    sb_put(&b, '\n');
    bh_fwrite(f, b.cp, b.len);
    free(b.cp);
}
static Value bi_print(int n, Value *a) { bh_print_to(stdout, n, a); return BH_NIL; }
static Value bi_eprint(int n, Value *a) { fflush(stdout); bh_print_to(stderr, n, a); return BH_NIL; }

static Value bi_input(int n, Value *a) {
    size_t cap = 256, len = 0;
    char *buf = (char *)bh_xmalloc(cap);
    int c;
    Value result;
    if (n) {
        Str *prompt = (Str *)bh_show(a[0], 0).u.p;
        bh_fwrite(stdout, prompt->cp, prompt->len);
    }
    fflush(stdout);
    while ((c = fgetc(stdin)) != EOF && c != '\n') {
        if (len + 1 >= cap) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
            if (!buf) bh_fail("内存不够了");
        }
        buf[len++] = (char)c;
    }
    if (len == 0 && feof(stdin)) bh_fail("没有可以读取的输入了");
    if (len && buf[len - 1] == '\r') len--;
    result = bh_obj(T_STR, bh_decode(buf, len));
    free(buf);
    return result;
}

static Value bi_len(int n, Value *a) {
    (void)n;
    if (a[0].tag == T_STR) return bh_int(((Str *)a[0].u.p)->len);
    if (a[0].tag == T_LIST) return bh_int(((List *)a[0].u.p)->len);
    if (a[0].tag == T_DICT) return bh_int(((Dict *)a[0].u.p)->len);
    bh_fail("只有文字、列表和字典有长度，这里是%s", bh_tn(a[0]));
    return BH_NIL;
}

/* 范围（1，5）是【1，2，3，4，5】，两头都算 */
static Value bi_range(int n, Value *a) {
    int64_t start = 1, stop, step = 1, i;
    List *l;
    int k;
    for (k = 0; k < n; k++) {
        if (!bh_isint(a[k])) bh_fail("「范围」的参数必须是整数");
        if (a[k].tag == T_BIG) bh_fail(BH_TOO_BIG);
    }
    if (n == 1) stop = a[0].u.i;
    else { start = a[0].u.i; stop = a[1].u.i; if (n == 3) step = a[2].u.i; }
    if (step == 0) bh_fail("「范围」的步长不能是 0");
    l = bh_list_alloc(16);
    if (step > 0) {
        for (i = start; i <= stop; i += step) { bh_push(l, bh_int(i)); if (i > INT64_MAX - step) break; }
    } else {
        for (i = start; i >= stop; i += step) { bh_push(l, bh_int(i)); if (i < INT64_MIN - step) break; }
    }
    return bh_obj(T_LIST, l);
}

/* 「对于 范围（……）里的每个」：参数都是普通整数时，不用真的造出这个列表，边数边给。
 * 别的情况（参数不对要报错、大整数）交给「范围」本身，造出列表来逐个取 */
typedef struct { int64_t start, step, count; int is_list; } BhRange;

static Value bh_range_begin(BhRange *r, int n, Value *a) {
    int64_t start = 1, stop, step = 1;
    uint64_t span, q;
    int k;
    Value l;
    for (k = 0; k < n; k++)
        if (a[k].tag != T_INT) goto slow;
    if (n == 1) stop = a[0].u.i;
    else { start = a[0].u.i; stop = a[1].u.i; if (n == 3) step = a[2].u.i; }
    if (step == 0) goto slow;
    r->is_list = 0;
    r->start = start;
    r->step = step;
    if (step > 0 ? stop < start : stop > start) { r->count = 0; return BH_NIL; }
    if (step > 0) { span = (uint64_t)stop - (uint64_t)start; q = span / (uint64_t)step; }
    else { span = (uint64_t)start - (uint64_t)stop; q = span / ((uint64_t)0 - (uint64_t)step); }
    if (q >= (uint64_t)INT64_MAX) goto slow;
    r->count = (int64_t)q + 1;
    return BH_NIL;
slow:
    l = bi_range(n, a);
    r->is_list = 1;
    r->count = ((List *)l.u.p)->len;
    return l;
}

BH_INLINE Value bh_range_at(const BhRange *r, Value list, int64_t i) {
    if (r->is_list) return ((List *)list.u.p)->items[i];
    return bh_int((int64_t)((uint64_t)r->start + (uint64_t)i * (uint64_t)r->step));
}

static int bh_isspace(uint32_t c) {
    return c == ' ' || (c >= 9 && c <= 13) || (c >= 0x1C && c <= 0x1F) || c == 0x85 || c == 0xA0
        || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029
        || c == 0x202F || c == 0x205F || c == 0x3000;
}

/* 去掉两头的空白，全角数字换成半角，得到一段可以交给 C 解析的文本。用完要 free */
static char *bh_number_text(const Str *s) {
    int64_t i = 0, j = s->len, k;
    char *o;
    while (i < j && bh_isspace(s->cp[i])) i++;
    while (j > i && bh_isspace(s->cp[j - 1])) j--;
    o = (char *)bh_xmalloc((size_t)(j - i) + 1);
    for (k = i; k < j; k++) {
        uint32_t c = s->cp[k];
        if (c >= 0xFF10 && c <= 0xFF19) c -= 0xFEE0;
        o[k - i] = c < 128 ? (char)c : '?';
    }
    o[j - i] = 0;
    return o;
}

static int bh_digit(char c) { return c >= '0' && c <= '9'; }

static int bh_int_syntax(const char *p) {       /* 是不是「正负号 + 一串数字」 */
    if (*p == '+' || *p == '-') p++;
    if (!bh_digit(*p)) return 0;
    while (bh_digit(*p)) p++;
    return *p == 0;
}

static int bh_word_is(const char *p, const char *word) {
    for (; *word; p++, word++)
        if ((*p | 32) != *word) return 0;
    return *p == 0;
}

static int bh_parse_float(const char *z, double *out) {
    const char *p = z;
    int digits = 0;
    if (*p == '+' || *p == '-') p++;
    if (bh_word_is(p, "inf") || bh_word_is(p, "infinity") || bh_word_is(p, "nan")) {
        *out = strtod(z, NULL);
        return 1;
    }
    while (bh_digit(*p)) { p++; digits++; }
    if (*p == '.') { p++; while (bh_digit(*p)) { p++; digits++; } }
    if (!digits) return 0;
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (!bh_digit(*p)) return 0;
        while (bh_digit(*p)) p++;
    }
    if (*p) return 0;
    *out = strtod(z, NULL);
    return 1;
}

static Value bi_int(int n, Value *a) {
    Value v = a[0], result;
    double d;
    (void)n;
    if (bh_isint(v)) return v;
    if (v.tag == T_FLOAT && !isinf(v.u.f) && v.u.f == v.u.f) return bh_from_double(v.u.f);
    if (v.tag == T_STR) {
        char *z = bh_number_text((Str *)v.u.p);
        int ok = 1;
        if (bh_int_syntax(z)) result = bh_big_lit(z);
        else if (bh_parse_float(z, &d) && !isinf(d) && d == d) result = bh_from_double(d);
        else ok = 0;
        free(z);
        if (ok) return result;
    }
    bh_fail("没法把 %s 变成整数", bh_shown(v));
    return BH_NIL;
}

static Value bi_float(int n, Value *a) {
    Value v = a[0];
    double d;
    (void)n;
    if (v.tag == T_FLOAT) return v;
    if (v.tag == T_INT) return bh_float((double)v.u.i);
    if (v.tag == T_BIG) {
        d = mag_to_double(((Big *)v.u.p)->d, ((Big *)v.u.p)->n);
        if (!isinf(d)) return bh_float(((Big *)v.u.p)->neg ? -d : d);
    } else if (v.tag == T_STR) {
        char *z = bh_number_text((Str *)v.u.p);
        int ok = bh_parse_float(z, &d);
        free(z);
        if (ok) return bh_float(d);
    }
    bh_fail("没法把 %s 变成小数", bh_shown(v));
    return BH_NIL;
}

static Value bi_str(int n, Value *a) { (void)n; return bh_show(a[0], 0); }
static Value bi_type(int n, Value *a) { (void)n; return bh_str_lit(bh_tn(a[0])); }

static Value bi_append(int n, Value *a) {
    (void)n;
    if (a[0].tag != T_LIST) bh_fail("「添加」的第一个参数必须是列表，这里是%s", bh_tn(a[0]));
    bh_push((List *)a[0].u.p, a[1]);
    return BH_NIL;
}

static Value bi_remove(int n, Value *a) {
    List *l;
    int64_t i;
    Value removed;
    (void)n;
    if (a[0].tag == T_DICT) return bh_dict_remove((Dict *)a[0].u.p, a[1]);
    if (a[0].tag != T_LIST) bh_fail("「删除」的第一个参数必须是列表或字典，这里是%s", bh_tn(a[0]));
    l = (List *)a[0].u.p;
    if (a[1].tag != T_INT || a[1].u.i < -l->len || a[1].u.i >= l->len)
        bh_fail("「删除」的第二个参数是下标，%s 不在范围里（一共 %lld 个，下标从 0 开始数）",
                bh_shown(a[1]), (long long)l->len);
    i = a[1].u.i < 0 ? a[1].u.i + l->len : a[1].u.i;
    removed = l->items[i];
    memmove(l->items + i, l->items + i + 1, (size_t)(l->len - i - 1) * sizeof(Value));
    l->len--;
    return removed;
}

/* 排序时的「小于」。类型对不上就报错 */
static int bh_sort_less(Value a, Value b, int depth) {
    if (bh_numlike(a) && bh_numlike(b)) {
        int bad = 0, c = bh_num_cmp(a, b, &bad);
        return !bad && c < 0;
    }
    if (a.tag == T_STR && b.tag == T_STR) return bh_strcmp((Str *)a.u.p, (Str *)b.u.p) < 0;
    if (a.tag == T_LIST && b.tag == T_LIST && depth < 500) {
        List *x = (List *)a.u.p, *y = (List *)b.u.p;
        int64_t i, m = x->len < y->len ? x->len : y->len;
        for (i = 0; i < m; i++)
            if (!bh_same(x->items[i], y->items[i], 0)) return bh_sort_less(x->items[i], y->items[i], depth + 1);
        return x->len < y->len;
    }
    bh_fail("列表里的东西不是同一类，没法排序");
    return 0;
}

static void bh_merge_sort(Value *v, Value *tmp, int64_t n) {
    int64_t mid = n / 2, i = 0, j = mid, k = 0;
    if (n < 2) return;
    bh_merge_sort(v, tmp, mid);
    bh_merge_sort(v + mid, tmp, n - mid);
    while (i < mid && j < n) {
        if (bh_sort_less(v[j], v[i], 0)) tmp[k++] = v[j++];   /* 相等时左边的在前：稳定排序 */
        else tmp[k++] = v[i++];
    }
    while (i < mid) tmp[k++] = v[i++];
    while (j < n) tmp[k++] = v[j++];
    memcpy(v, tmp, (size_t)n * sizeof(Value));
}

static Value bi_sorted(int n, Value *a) {
    Value out;
    List *l;
    (void)n;
    if (a[0].tag != T_LIST) bh_fail("「排序」的参数必须是列表，这里是%s", bh_tn(a[0]));
    out = bh_list_make(((List *)a[0].u.p)->len, ((List *)a[0].u.p)->items);
    l = (List *)out.u.p;
    if (l->len > 1) {
        Value *tmp = (Value *)bh_xmalloc((size_t)l->len * sizeof(Value));
        bh_merge_sort(l->items, tmp, l->len);
        free(tmp);
    }
    return out;
}

static uint64_t bh_random(void) {
    static uint64_t state = 0;
    if (!state) state = ((uint64_t)time(NULL) << 20) ^ (uint64_t)clock() ^ (uint64_t)(uintptr_t)&state ^ 0x9E3779B97F4A7C15ULL;
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1DULL;
}

static Value bi_randint(int n, Value *a) {
    uint64_t span, r;
    (void)n;
    if (a[0].tag == T_BIG || a[1].tag == T_BIG) bh_fail(BH_TOO_BIG);
    if (a[0].tag != T_INT || a[1].tag != T_INT || a[0].u.i > a[1].u.i)
        bh_fail("「随机整数」需要两个整数，而且前一个不能比后一个大");
    span = (uint64_t)a[1].u.i - (uint64_t)a[0].u.i + 1;
    r = bh_random() >> 11;
    if (span) r %= span;
    return bh_int((int64_t)((uint64_t)a[0].u.i + r));
}

static Value bi_slice(int n, Value *a) {
    int64_t total, start, len;
    (void)n;
    if (a[0].tag != T_STR && a[0].tag != T_LIST)
        bh_fail("「截取」的第一个参数必须是文字或列表，这里是%s", bh_tn(a[0]));
    if (!bh_isint(a[1]) || !bh_isint(a[2]) || bh_small(a[1]) < 0 || bh_small(a[2]) < 0)
        bh_fail("「截取」的开始位置和长度必须是不小于 0 的整数");
    total = a[0].tag == T_STR ? ((Str *)a[0].u.p)->len : ((List *)a[0].u.p)->len;
    start = bh_small(a[1]) < total ? bh_small(a[1]) : total;
    len = bh_small(a[2]) < total - start ? bh_small(a[2]) : total - start;
    if (a[0].tag == T_STR) {
        Str *s = bh_str_alloc(len);
        if (len) memcpy(s->cp, ((Str *)a[0].u.p)->cp + start, (size_t)len * sizeof(uint32_t));
        return bh_obj(T_STR, s);
    }
    return bh_list_make(len, ((List *)a[0].u.p)->items + start);
}

static Value bi_ord(int n, Value *a) {
    (void)n;
    if (a[0].tag != T_STR || ((Str *)a[0].u.p)->len != 1) bh_fail("「编码」需要一个单独的字，这里是 %s", bh_shown(a[0]));
    return bh_int(((Str *)a[0].u.p)->cp[0]);
}

static Value bi_chr(int n, Value *a) {
    (void)n;
    if (a[0].tag != T_INT || a[0].u.i < 0 || a[0].u.i > 0x10FFFF || (a[0].u.i >= 0xD800 && a[0].u.i <= 0xDFFF))
        bh_fail("「字符」需要一个有效的文字编码，这里是 %s", bh_shown(a[0]));
    return bh_char((uint32_t)a[0].u.i);
}

static Value bi_join(int n, Value *a) {
    SB b = {NULL, 0, 0};
    List *l;
    int64_t i;
    if (a[0].tag != T_LIST) bh_fail("「连接」的第一个参数必须是列表，这里是%s", bh_tn(a[0]));
    if (n == 2 && a[1].tag != T_STR) bh_fail("「连接」的分隔符必须是文字，这里是%s", bh_tn(a[1]));
    l = (List *)a[0].u.p;
    for (i = 0; i < l->len; i++) {
        if (i && n == 2) sb_str(&b, (Str *)a[1].u.p);
        bh_show_into(&b, l->items[i], 0, 0);
    }
    return sb_done(&b);
}

static int64_t bh_str_find(const Str *text, const Str *part, int64_t from) {
    int64_t i;
    for (i = from; i + part->len <= text->len; i++)
        if (part->len == 0 || memcmp(text->cp + i, part->cp, (size_t)part->len * sizeof(uint32_t)) == 0) return i;
    return -1;
}

static Value bi_find(int n, Value *a) {
    (void)n;
    if (a[0].tag == T_STR) {
        if (a[1].tag != T_STR) bh_fail("在文字里只能查找文字，这里是%s", bh_tn(a[1]));
        return bh_int(bh_str_find((Str *)a[0].u.p, (Str *)a[1].u.p, 0));
    }
    if (a[0].tag == T_LIST) {
        List *l = (List *)a[0].u.p;
        int64_t i;
        for (i = 0; i < l->len; i++)
            if (bh_equal(l->items[i], a[1])) return bh_int(i);
        return bh_int(-1);
    }
    bh_fail("「查找」的第一个参数必须是文字或列表，这里是%s", bh_tn(a[0]));
    return BH_NIL;
}

static Value bi_split(int n, Value *a) {
    Str *text, *sep, *piece;
    List *out;
    int64_t from = 0, at;
    (void)n;
    if (a[0].tag != T_STR || a[1].tag != T_STR) bh_fail("「分割」的两个参数都必须是文字");
    text = (Str *)a[0].u.p;
    sep = (Str *)a[1].u.p;
    if (sep->len == 0) bh_fail("「分割」的分隔符不能是空的");
    out = bh_list_alloc(8);
    for (;;) {
        int found = (at = bh_str_find(text, sep, from)) >= 0;
        if (!found) at = text->len;
        piece = bh_str_alloc(at - from);
        if (at > from) memcpy(piece->cp, text->cp + from, (size_t)(at - from) * sizeof(uint32_t));
        bh_push(out, bh_obj(T_STR, piece));
        if (!found) break;
        from = at + sep->len;
    }
    return bh_obj(T_LIST, out);
}

static char *bh_path(Value v) {                 /* 用完要 free */
    if (v.tag != T_STR) bh_fail("文件路径必须是文字，这里是%s", bh_tn(v));
    return bh_cstr((Str *)v.u.p);
}

static Value bi_read_file(int n, Value *a) {
    char *path = bh_path(a[0]);
    FILE *f = fopen(path, "rb");
    size_t cap = 65536, len = 0, got;
    char *buf;
    Value result;
    (void)n;
    free(path);
    if (!f) return BH_NIL;                    /* 打不开就返回 空，由程序自己决定怎么办 */
    buf = (char *)bh_xmalloc(cap);
    while ((got = fread(buf + len, 1, cap - len, f)) > 0) {
        len += got;
        if (len == cap) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
            if (!buf) bh_fail("内存不够了");
        }
    }
    fclose(f);
    result = bh_obj(T_STR, bh_decode(buf, len));
    free(buf);
    return result;
}

static Value bi_write_file(int n, Value *a) {
    char *path = bh_path(a[0]);
    FILE *f;
    (void)n;
    if (a[1].tag != T_STR) bh_fail("「写文件」的内容必须是文字，这里是%s", bh_tn(a[1]));
    f = fopen(path, "wb");
    free(path);
    if (!f) return BH_FALSE;                  /* 写成功返回 真，失败返回 假 */
    bh_fwrite(f, ((Str *)a[1].u.p)->cp, ((Str *)a[1].u.p)->len);
    return bh_bool(fclose(f) == 0);
}

static Value bi_args(int n, Value *a) {
    List *l = bh_list_alloc(bh_argc);
    int i;
    (void)n; (void)a;
    for (i = 1; i < bh_argc; i++) bh_push(l, bh_str_lit(bh_argv[i]));
    return bh_obj(T_LIST, l);
}

static Value bi_exit(int n, Value *a) {
    if (n && a[0].tag != T_INT) bh_fail("「退出」的参数必须是整数，这里是%s", bh_tn(a[0]));
    fflush(stdout);
    exit(n ? (int)a[0].u.i : 0);
    return BH_NIL;
}

static Value bi_system(int n, Value *a) {
    int status;
    char *command;
    (void)n;
    if (a[0].tag != T_STR) bh_fail("「运行命令」的参数必须是文字，这里是%s", bh_tn(a[0]));
    fflush(stdout);
    fflush(stderr);
    command = bh_cstr((Str *)a[0].u.p);
    status = system(command);
    free(command);
#ifndef _WIN32
    if (status != -1 && WIFEXITED(status)) status = WEXITSTATUS(status);
    else if (status != 0) status = 1;
#endif
    return bh_int(status);
}

static Value bi_runtime_source(int n, Value *a) {
    SB b = {NULL, 0, 0};
    int i;
    (void)n; (void)a;
    if (!bh_rt_lines) bh_fail("这个程序里没有带运行时源码");
    for (i = 0; i < bh_rt_lines_n; i++) sb_utf8(&b, bh_rt_lines[i]);
    return sb_done(&b);
}

/* ---------- 0.3 版新增：字典、按行读文件、格式化、文字处理 ---------- */

static Value bi_contains(int n, Value *a) {
    (void)n;
    if (a[0].tag == T_DICT) {
        bh_check_key(a[1]);
        return bh_bool(bh_dict_find((Dict *)a[0].u.p, a[1], bh_hash(a[1])) >= 0);
    }
    if (a[0].tag == T_LIST) {
        List *l = (List *)a[0].u.p;
        int64_t i;
        for (i = 0; i < l->len; i++)
            if (bh_equal(l->items[i], a[1])) return BH_TRUE;
        return BH_FALSE;
    }
    if (a[0].tag == T_STR) {
        if (a[1].tag != T_STR) bh_fail("在文字里只能查找文字，这里是%s", bh_tn(a[1]));
        return bh_bool(bh_str_find((Str *)a[0].u.p, (Str *)a[1].u.p, 0) >= 0);
    }
    bh_fail("「包含」的第一个参数必须是列表、文字或字典，这里是%s", bh_tn(a[0]));
    return BH_NIL;
}

static Dict *bh_need_dict(Value v, const char *who) {
    if (v.tag != T_DICT) bh_fail("「%s」的参数必须是字典，这里是%s", who, bh_tn(v));
    return (Dict *)v.u.p;
}

static Value bi_keys(int n, Value *a) {
    Dict *d = bh_need_dict(a[0], "所有键");
    List *l = bh_list_alloc(d->len);
    int64_t i;
    (void)n;
    for (i = 0; i < d->used; i++)
        if (d->e[i].key.tag != T_UNDEF) bh_push(l, d->e[i].key);
    return bh_obj(T_LIST, l);
}

static Value bi_values(int n, Value *a) {
    Dict *d = bh_need_dict(a[0], "所有值");
    List *l = bh_list_alloc(d->len);
    int64_t i;
    (void)n;
    for (i = 0; i < d->used; i++)
        if (d->e[i].key.tag != T_UNDEF) bh_push(l, d->e[i].val);
    return bh_obj(T_LIST, l);
}

static Value bi_items(int n, Value *a) {
    Dict *d = bh_need_dict(a[0], "所有项");
    List *l = bh_list_alloc(d->len);
    int64_t i;
    (void)n;
    for (i = 0; i < d->used; i++) {
        Value pair[2];
        if (d->e[i].key.tag == T_UNDEF) continue;
        pair[0] = d->e[i].key;
        pair[1] = d->e[i].val;
        bh_push(l, bh_list_make(2, pair));
    }
    return bh_obj(T_LIST, l);
}

static Value bi_get(int n, Value *a) {
    int found;
    Value v;
    if (a[0].tag != T_DICT) bh_fail("「取」的第一个参数必须是字典，这里是%s", bh_tn(a[0]));
    v = bh_dict_get((Dict *)a[0].u.p, a[1], &found);
    if (found) return v;
    return n == 3 ? a[2] : BH_NIL;
}

/* 格式化（“{}今年{}岁”，名字，年龄）：模板里的 {} 依次换成后面的值。
 * 全角的 ｛｝ 也行；想要大括号本身就连写两个：{{ }} */
static Value bi_format(int n, Value *a) {
    SB b = {NULL, 0, 0};
    Str *t;
    int64_t i = 0, count = 0;
    if (a[0].tag != T_STR) bh_fail("「格式化」的第一个参数必须是文字，这里是%s", bh_tn(a[0]));
    t = (Str *)a[0].u.p;
    while (i < t->len) {
        uint32_t c = t->cp[i];
        if (c == '{' || c == 0xFF5B) {
            uint32_t close = c == '{' ? '}' : 0xFF5D;
            if (i + 1 < t->len && t->cp[i + 1] == c) { sb_put(&b, c); i += 2; continue; }
            if (i + 1 < t->len && t->cp[i + 1] == close) {
                if (count < n - 1) bh_show_into(&b, a[1 + count], 0, 0);
                count++;
                i += 2;
                continue;
            }
        } else if ((c == '}' || c == 0xFF5D) && i + 1 < t->len && t->cp[i + 1] == c) {
            sb_put(&b, c);
            i += 2;
            continue;
        }
        sb_put(&b, c);
        i++;
    }
    if (count != n - 1) {
        free(b.cp);
        bh_fail("「格式化」的模板里有 %lld 个 {}，后面却给了 %d 个值", (long long)count, n - 1);
    }
    return sb_done(&b);
}

static Value bi_fixed(int n, Value *a) {
    char buf[512];
    double x;
    (void)n;
    if (!bh_isnum(a[0])) bh_fail("「保留小数」的第一个参数必须是数字，这里是%s", bh_tn(a[0]));
    if (a[1].tag != T_INT || a[1].u.i < 0 || a[1].u.i > 100) bh_fail("「保留小数」的位数必须是 0 到 100 之间的整数");
    x = bh_tof(a[0]);
    snprintf(buf, sizeof buf, "%.*f", (int)a[1].u.i, x);
    return bh_ascii_str(buf, strlen(buf));
}

/* 读行：读出整个文件，按行分开。每行末尾的换行不要；文件最后的换行不会多出一个空行 */
static Value bi_read_lines(int n, Value *a) {
    Value text = bi_read_file(n, a);
    Str *s, *piece;
    List *out;
    int64_t from = 0, i, end;
    if (text.tag == T_NIL) return text;
    s = (Str *)text.u.p;
    out = bh_list_alloc(16);
    for (i = 0; i <= s->len; i++) {
        if (i < s->len && s->cp[i] != '\n') continue;
        if (i == s->len && from == s->len) break;
        end = i;
        if (end > from && s->cp[end - 1] == '\r') end--;
        piece = bh_str_alloc(end - from);
        if (end > from) memcpy(piece->cp, s->cp + from, (size_t)(end - from) * sizeof(uint32_t));
        bh_push(out, bh_obj(T_STR, piece));
        from = i + 1;
    }
    return bh_obj(T_LIST, out);
}

static Value bi_append_file(int n, Value *a) {
    char *path = bh_path(a[0]);
    FILE *f;
    (void)n;
    if (a[1].tag != T_STR) { free(path); bh_fail("「追加文件」的内容必须是文字，这里是%s", bh_tn(a[1])); }
    f = fopen(path, "ab");
    free(path);
    if (!f) return BH_FALSE;
    bh_fwrite(f, ((Str *)a[1].u.p)->cp, ((Str *)a[1].u.p)->len);
    return bh_bool(fclose(f) == 0);
}

static Value bi_replace(int n, Value *a) {
    SB b = {NULL, 0, 0};
    Str *t, *old, *rep;
    int64_t from = 0, at;
    (void)n;
    if (a[0].tag != T_STR || a[1].tag != T_STR || a[2].tag != T_STR) bh_fail("「替换」的三个参数都必须是文字");
    t = (Str *)a[0].u.p;
    old = (Str *)a[1].u.p;
    rep = (Str *)a[2].u.p;
    if (old->len == 0) bh_fail("「替换」要换掉的文字不能是空的");
    while ((at = bh_str_find(t, old, from)) >= 0) {
        sb_cp(&b, t->cp + from, at - from);
        sb_str(&b, rep);
        from = at + old->len;
    }
    sb_cp(&b, t->cp + from, t->len - from);
    return sb_done(&b);
}

static Value bi_strip(int n, Value *a) {
    Str *s, *r;
    int64_t lo = 0, hi;
    (void)n;
    if (a[0].tag != T_STR) bh_fail("「去空白」的参数必须是文字，这里是%s", bh_tn(a[0]));
    s = (Str *)a[0].u.p;
    hi = s->len;
    while (lo < hi && bh_isspace(s->cp[lo])) lo++;
    while (hi > lo && bh_isspace(s->cp[hi - 1])) hi--;
    r = bh_str_alloc(hi - lo);
    if (hi > lo) memcpy(r->cp, s->cp + lo, (size_t)(hi - lo) * sizeof(uint32_t));
    return bh_obj(T_STR, r);
}

static const Builtin bh_builtins[] = {
    {"打印", bi_print, 0, -1}, {"输入", bi_input, 0, 1}, {"长度", bi_len, 1, 1}, {"范围", bi_range, 1, 3},
    {"整数", bi_int, 1, 1}, {"小数", bi_float, 1, 1}, {"文字", bi_str, 1, 1}, {"类型", bi_type, 1, 1},
    {"添加", bi_append, 2, 2}, {"删除", bi_remove, 2, 2}, {"排序", bi_sorted, 1, 1}, {"随机整数", bi_randint, 2, 2},
    {"截取", bi_slice, 3, 3}, {"编码", bi_ord, 1, 1}, {"字符", bi_chr, 1, 1}, {"连接", bi_join, 1, 2},
    {"查找", bi_find, 2, 2}, {"分割", bi_split, 2, 2}, {"读文件", bi_read_file, 1, 1}, {"写文件", bi_write_file, 2, 2},
    {"命令行参数", bi_args, 0, 0}, {"退出", bi_exit, 0, 1}, {"运行命令", bi_system, 1, 1}, {"报错", bi_eprint, 0, -1},
    {"运行时源码", bi_runtime_source, 0, 0},
    {"包含", bi_contains, 2, 2}, {"所有键", bi_keys, 1, 1}, {"所有值", bi_values, 1, 1}, {"所有项", bi_items, 1, 1},
    {"取", bi_get, 2, 3}, {"格式化", bi_format, 1, -1}, {"保留小数", bi_fixed, 2, 2}, {"读行", bi_read_lines, 1, 1},
    {"追加文件", bi_append_file, 2, 2}, {"替换", bi_replace, 3, 3}, {"去空白", bi_strip, 1, 1},
    {NULL, NULL, 0, 0}
};

/* ---------- 程序启动 ---------- */

/* 告诉回收器两个「根」数组在哪：最外层的变量 G 和文字常量 K。
 * 全局变量里和内置函数同名的，先填上内置函数 */
static void bh_init_globals(Value *globals, const char **names, int n, Value *consts, int n_consts) {
    int i, k;
    bh_root_arrays[0] = globals; bh_root_counts[0] = n;
    bh_root_arrays[1] = consts; bh_root_counts[1] = n_consts;
    for (i = 0; i < n; i++)
        for (k = 0; bh_builtins[k].name; k++)
            if (strcmp(names[i], bh_builtins[k].name) == 0) globals[i] = bh_obj(T_BUILTIN, (void *)&bh_builtins[k]);
}

static void bh_init(int argc, char **argv, char *stack_base) {
    const char *stress = getenv("BH_GC_STRESS");
    bh_argc = argc;
    bh_argv = argv;
    bh_stack_base = (uintptr_t)stack_base;
    if (stress && atol(stress) > 0) {
        bh_gc_stress = bh_gc_countdown = atol(stress);
        bh_gc_wanted = 1;
    }
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#else
    {   /* 递归太深时要赶在栈用完之前报错，所以先查一下栈有多大 */
        struct rlimit rl;
        size_t size = (size_t)8 << 20, margin;
        if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur > 0)
            size = (size_t)rl.rlim_cur;
        if (size > ((size_t)64 << 20)) size = (size_t)64 << 20;
        /* 留出一截余量：栈顶还放着环境变量和命令行参数，最后一层函数也要有地方用 */
        margin = size / 8 > 131072 ? size / 8 : 131072;
        bh_stack_limit = size > 2 * margin ? size - margin : size / 2;
    }
#endif
}

/* ======================================================================
 * 运行时到此结束，下面是编译器根据白话程序生成的代码
 * ====================================================================== */
