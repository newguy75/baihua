/* 由白话编译器生成，源文件：baihuac.bh */
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

static Value G[258];
static const char *bh_gnames[] = {"关键字表", "文字运算符", "对应符号", "长关键字", "字数", "词", "全角表", "半角表", "缩进字符", "缩进宽度", "空白字符", "引号开", "引号关", "双字符号", "单字符号", "赋值符号", "比较符号", "字母范围", "源码行表", "下标", "码", "半角", "是字母", "是数字", "名字字符", "头", "尾", "去空白", "片", "半角串", "去前导零", "这行", "语法出错", "名字", "空格提示", "行号", "文本", "括号栈", "位置", "总长", "宽度", "缩进栈", "记号表", "原字", "字", "号", "指针", "结尾们", "转义", "内容", "数值", "后一个", "符号", "开", "占", "词法分析", "记号", "游标", "循环层数", "函数层数", "看", "这个", "前进", "是这个", "接受", "种类", "说", "某个", "行", "名字们", "语法错", "想要", "期待", "期待名字", "语句结束了", "项", "并入", "语句表", "解析语句", "解析程序", "解析如果", "解析当", "解析对于", "解析重复", "解析函数", "解析简单行", "解析简单语句", "最后", "消息", "名", "解析表达式", "值", "符", "式", "解析块", "体", "解析循环体", "条件", "分支们", "别的", "来源", "次数", "参数们", "参数", "外面的循环", "解析或", "解析且", "左", "解析非", "解析比较", "解析加减", "其余", "首", "解析乘除", "解析一元", "解析后缀", "解析基本", "节点", "解析一串", "项们", "对们", "键", "全局名表", "常量表", "函数成品", "函数个数", "环境", "内置名表", "内置表", "全局被赋值", "直接调用过", "代码位", "局部位", "参数位", "声明位", "内层位", "临时位", "行位", "外层位", "模块位", "缩进位", "最多位", "整型位", "根位", "最多根位", "叶子位", "新环境", "出", "开块", "关块", "否则块", "格", "新临时", "根临时", "不用保护", "保根", "新整型", "定行", "全局槽", "常量槽", "C文字", "数字常量", "至少一", "记下", "句", "收集", "分支", "起", "止", "片们", "两半", "读内置表", "找函数", "局部", "声明", "标记", "里面的", "找全局赋值", "被调", "外", "个数", "直接内置", "一串可能回收", "摊平", "可能回收", "对", "结果", "有", "后面可能回收", "是叶子", "全局取值", "槽", "候选", "路径", "外层取值", "临", "声", "前缀", "读变量", "本地", "写变量", "运算函数", "算", "后面", "式们", "算一串", "数组式", "右", "算比较", "算逻辑", "容器", "层数", "编语句", "编语句表", "编赋值", "编如果", "是范围调用", "区", "份", "总", "编函数", "用到这里", "根用到这里", "运算", "目标", "旧", "外面", "编号", "根数", "内层", "格数", "添加C临时", "结尾", "框", "名们", "树", "模块头", "成品", "一行", "带源码", "主体", "制造", "编译", "引", "参数表", "有下一个", "输入文件", "源码", "运行时文件", "运行时", "只要C", "输出名", "基名", "C代码", "C文件", "C编译器", "状态", "主程序", NULL};
static Value K[358];
static const char *bh_srclines[] = {
    "# 白话编译器，用白话自己写的。",
    "# 把 .bh 程序翻译成 C，再调用系统的 C 编译器，得到一个可以直接运行的程序。",
    "#",
    "# 用法：",
    "#     baihuac 程序.bh               生成可执行文件「程序」",
    "#     baihuac 程序.bh -o 名字       指定可执行文件的名字",
    "#     baihuac --emit-c 程序.bh      只生成 C 代码（程序.c），不调用 C 编译器",
    "#",
    "# 三个步骤，下面的代码也按这个顺序排：",
    "#     源代码 -> 【一、词法分析】-> 记号 -> 【二、语法分析】-> 语法树 -> 【三、生成代码】-> C 程序",
    "",
    "# ======================================================================",
    "# 一、词法分析：把一长串字符切成一个个记号",
    "# ======================================================================",
    "# 一个记号是【种类，值，行号，原样写法】。",
    "# 种类：NUM 数字、STR 文字、NAME 名字、KW 关键字、OP 运算符和标点、",
    "#       NEWLINE 一行结束、INDENT 缩进增加、DEDENT 缩进减少、EOF 程序结束",
    "",
    "关键字表 = 【“函数”，“返回”，“全局”，“如果”，“否则”，“否则如果”，“当”，“对于”，“里的每个”，“中的每个”，“重复”，“次”，“跳出”，“继续”，“真”，“假”，“空”】",
    "",
    "# 用文字写的运算符，词法分析时直接换成对应的符号",
    "文字运算符 = 【“除以”，“大于”，“小于”，“大于等于”，“小于等于”，“等于”，“是”，“不等于”，“并且”，“或者”，“不是”】",
    "对应符号 = 【“/”，“>”，“<”，“>=”，“<=”，“==”，“==”，“!=”，“and”，“or”，“not”】",
    "",
    "# 两个字以上的关键字，长的排前面。报错时用来猜「是不是忘了加空格」",
    "长关键字 = 【】",
    "对于 【4，3，2】 里的每个 字数：",
    "对于 关键字表 + 文字运算符 里的每个 词：",
    "如果 长度（词） 是 字数：",
    "添加（长关键字，词）",
    "",
    "# 中文标点 -> 英文标点。全角的 （ ） ， ： ； ！ ＝ 可以用编码换算（见 半角），这里只列换算不了的",
    "全角表 = 【“【”，“】”，“、”，“。”，“《”，“》”，“×”，“÷”，“≤”，“≥”，“≠”，字符（12288），字符（160），字符（8203），字符（65279）】",
    "半角表 = 【“[”，“]”，“,”，“;”，“<”，“>”，“*”，“/”，“<=”，“>=”，“!=”，“ ”，“ ”，“ ”，“ ”】",
    "",
    "# 行首的空白各算几格缩进：全角空格算两格，Tab 算四格",
    "缩进字符 = 【“ ”，“\\t”，字符（12288），字符（160），字符（8203），字符（65279）】",
    "缩进宽度 = 【1，4，2，1，0，0】",
    "空白字符 = 【“ ”，“\\t”，“\\n”，字符（13），字符（11），字符（12），字符（12288），字符（160）】",
    "",
    "# 开头的引号，以及各自可以用什么来结尾",
    "引号开 = 【“\"”，“'”，「“」，「”」，“‘”，“’”，“「”，“『”】",
    "引号关 = 【“\"”，“'”，「”“」，「”“」，“’‘”，“’‘”，“」”，“』”】",
    "",
    "双字符号 = 【“==”，“!=”，“<=”，“>=”，“+=”，“-=”，“*=”，“/=”】",
    "单字符号 = “+-*/%=<>!()[],:;”",
    "赋值符号 = 【“=”，“+=”，“-=”，“*=”，“/=”】",
    "比较符号 = 【“==”，“!=”，“<”，“>”，“<=”，“>=”】",
    "",
    "# 名字里可以出现的文字，一对一对的编码范围：汉字、英文字母、带声调的拉丁字母、",
    "# 希腊和西里尔字母、假名、谚文",
    "字母范围 = 【19968，40959，65，90，97，122，192，214，216，246，248，591，880，1023，1024，1327，12353，12447，12449，12543，13312，19903，44032，55203，63744，64255，131072，201551】",
    "",
    "源码行表 = 【】",
    "",
    "",
    "函数 半角（字）：",
    "# 把中文（全角）符号换成对应的英文（半角）符号，其他字符原样返回",
    "下标 = 查找（全角表，字）",
    "如果 下标 大于等于 0：",
    "返回 半角表【下标】",
    "码 = 编码（字）",
    "如果 码 大于等于 65281 并且 码 小于等于 65374：",
    "返回 字符（码 - 65248）",
    "返回 字",
    "",
    "",
    "函数 是字母（字）：",
    "如果 长度（字） 不是 1：",
    "返回 假",
    "码 = 编码（字）",
    "下标 = 0",
    "当 下标 小于 长度（字母范围）：",
    "如果 码 大于等于 字母范围【下标】 并且 码 小于等于 字母范围【下标 + 1】：",
    "返回 真",
    "下标 += 2",
    "返回 假",
    "",
    "",
    "函数 是数字（字）：",
    "返回 长度（字） 是 1 并且 编码（字） 大于等于 48 并且 编码（字） 小于等于 57",
    "",
    "",
    "函数 名字字符（字）：",
    "返回 是字母（字） 或者 字 是 “_” 或者 是数字（字）",
    "",
    "",
    "函数 去空白（文）：",
    "头 = 0",
    "尾 = 长度（文）",
    "当 头 小于 尾 并且 查找（空白字符，文【头】） 大于等于 0：",
    "头 += 1",
    "当 尾 大于 头 并且 查找（空白字符，文【尾 - 1】） 大于等于 0：",
    "尾 -= 1",
    "返回 截取（文，头，尾 - 头）",
    "",
    "",
    "函数 半角串（文本，起，止）：",
    "片 = 【】",
    "下标 = 起",
    "当 下标 小于 止：",
    "添加（片，半角（文本【下标】））",
    "下标 += 1",
    "返回 连接（片）",
    "",
    "",
    "函数 去前导零（文）：",
    "头 = 0",
    "当 头 小于 长度（文） - 1 并且 文【头】 是 “0”：",
    "头 += 1",
    "返回 截取（文，头，长度（文） - 头）",
    "",
    "",
    "函数 语法出错（消息，行）：",
    "# 报出语法错误，指出是哪一行，然后结束编译",
    "报错（“语法错误（第 ” + 行 + “ 行）：” + 消息）",
    "如果 行 大于等于 1 并且 行 小于等于 长度（源码行表）：",
    "这行 = 去空白（源码行表【行 - 1】）",
    "如果 这行 不是 “”：",
    "报错（“  >  ” + 这行）",
    "退出（1）",
    "",
    "",
    "函数 空格提示（名字们）：",
    "# 中文没有空格，很容易把关键字和名字连着写。名字里夹着关键字时给一句提示",
    "对于 名字们 里的每个 名字：",
    "对于 长关键字 里的每个 词：",
    "如果 查找（名字，词） 大于等于 0 并且 词 不是 名字：",
    "返回 “\\n提示：「” + 名字 + “」被当成了一个名字。如果其中的「” + 词 + “」是单独的一个词，要在它前后加上空格。”",
    "返回 “”",
    "",
    "",
    "函数 词法分析（）：",
    "记号表 = 【】",
    "缩进栈 = 【0】          # 每进入一层代码块，就把那一层的缩进宽度压进来",
    "括号栈 = 【】           # 还没关上的括号。括号里面可以随便换行",
    "行号 = 0",
    "对于 源码行表 里的每个 文本：",
    "行号 += 1",
    "位置 = 0",
    "总长 = 长度（文本）",
    "",
    "如果 长度（括号栈） 是 0：",
    "# 一行的开头：量一量缩进",
    "宽度 = 0",
    "当 位置 小于 总长 并且 查找（缩进字符，文本【位置】） 大于等于 0：",
    "宽度 += 缩进宽度【查找（缩进字符，文本【位置】）】",
    "位置 += 1",
    "如果 位置 是 总长 或者 半角（文本【位置】） 是 “#”：",
    "继续                      # 空行和整行注释直接跳过",
    "如果 宽度 大于 缩进栈【-1】：",
    "添加（缩进栈，宽度）",
    "添加（记号表，【“INDENT”，空，行号，“”】）",
    "否则：",
    "当 宽度 小于 缩进栈【-1】：",
    "删除（缩进栈，-1）",
    "添加（记号表，【“DEDENT”，空，行号，“”】）",
    "如果 宽度 不是 缩进栈【-1】：",
    "语法出错（“缩进对不齐：这一行开头的空格数和前面哪一层都对不上”，行号）",
    "",
    "当 位置 小于 总长：",
    "原字 = 文本【位置】             # 原始字符，可能是全角的",
    "字 = 半角（原字）               # 换算成半角之后的样子",
    "",
    "如果 字 是 “ ” 或者 字 是 “\\t”：",
    "位置 += 1",
    "",
    "否则如果 字 是 “#”：            # 注释：从 # 到行尾都不看",
    "跳出",
    "",
    "否则如果 查找（引号开，原字） 大于等于 0 或者 查找（引号开，字） 大于等于 0：",
    "# 文字。引号里面的内容原样保留",
    "号 = 查找（引号开，原字）",
    "如果 号 小于 0：",
    "号 = 查找（引号开，字）",
    "结尾们 = 引号关【号】",
    "内容 = 【】",
    "指针 = 位置 + 1",
    "当 指针 小于 总长 并且 查找（结尾们，文本【指针】） 小于 0 并且 查找（结尾们，半角（文本【指针】）） 小于 0：",
    "如果 文本【指针】 是 “\\\\” 并且 指针 + 1 小于 总长：",
    "指针 += 1               # 反斜杠转义：\\n 换行，\\t 制表符",
    "转义 = 文本【指针】",
    "如果 转义 是 “n”：",
    "添加（内容，“\\n”）",
    "否则如果 转义 是 “t”：",
    "添加（内容，“\\t”）",
    "否则：",
    "添加（内容，转义）",
    "否则：",
    "添加（内容，文本【指针】）",
    "指针 += 1",
    "如果 指针 大于等于 总长：",
    "语法出错（“文字的引号没有关上（开头的引号是 ” + 原字 + “ ）”，行号）",
    "添加（记号表，【“STR”，连接（内容），行号，截取（文本，位置，指针 + 1 - 位置）】）",
    "位置 = 指针 + 1",
    "",
    "否则如果 是数字（字）：",
    "指针 = 位置",
    "当 指针 小于 总长 并且 是数字（半角（文本【指针】））：",
    "指针 += 1",
    "数值 = 半角串（文本，位置，指针）",
    "# 小数点：中文输入法可能把 . 打成 。 只要它后面紧跟着数字，就当小数点",
    "如果 指针 + 1 小于 总长 并且 查找（“.。．”，文本【指针】） 大于等于 0 并且 是数字（半角（文本【指针 + 1】））：",
    "尾 = 指针 + 1",
    "当 尾 小于 总长 并且 是数字（半角（文本【尾】））：",
    "尾 += 1",
    "数值 = 数值 + “.” + 半角串（文本，指针 + 1，尾）",
    "指针 = 尾",
    "否则：",
    "数值 = 去前导零（数值）",
    "添加（记号表，【“NUM”，数值，行号，截取（文本，位置，指针 - 位置）】）",
    "位置 = 指针",
    "",
    "否则如果 是字母（字） 或者 字 是 “_”：",
    "# 一串连续的汉字、字母、数字、下划线：可能是名字、关键字或文字运算符",
    "指针 = 位置",
    "当 指针 小于 总长 并且 名字字符（半角（文本【指针】））：",
    "指针 += 1",
    "词 = 半角串（文本，位置，指针）",
    "号 = 查找（文字运算符，词）",
    "如果 号 大于等于 0：",
    "添加（记号表，【“OP”，对应符号【号】，行号，词】）",
    "否则如果 查找（关键字表，词） 大于等于 0：",
    "添加（记号表，【“KW”，词，行号，词】）",
    "否则：",
    "添加（记号表，【“NAME”，词，行号，词】）",
    "位置 = 指针",
    "",
    "否则：",
    "后一个 = “”",
    "如果 位置 + 1 小于 总长：",
    "后一个 = 半角（文本【位置 + 1】）",
    "如果 长度（字） 是 2：                                  # ≤ ≥ ≠ 这种一个字符顶两个的",
    "符号 = 字",
    "占 = 1",
    "否则如果 查找（双字符号，字 + 后一个） 大于等于 0：      # == != <= >= += 等",
    "符号 = 字 + 后一个",
    "占 = 2",
    "否则如果 查找（单字符号，字） 大于等于 0：",
    "符号 = 字",
    "占 = 1",
    "否则：",
    "语法出错（“不认识的符号「” + 原字 + “」”，行号）",
    "如果 符号 是 “(” 或者 符号 是 “[”：",
    "添加（括号栈，【符号，原字，行号】）",
    "否则如果 符号 是 “)” 或者 符号 是 “]”：",
    "如果 长度（括号栈） 是 0：",
    "语法出错（“多了一个「” + 原字 + “」，前面没有和它配对的括号”，行号）",
    "开 = 括号栈【-1】",
    "删除（括号栈，-1）",
    "如果 （开【0】 是 “(”） 不是 （符号 是 “)”）：",
    "语法出错（“括号没有配对：「” + 开【1】 + “」不能用「” + 原字 + “」来关”，行号）",
    "添加（记号表，【“OP”，符号，行号，截取（文本，位置，占）】）",
    "位置 += 占",
    "",
    "如果 长度（括号栈） 是 0：",
    "添加（记号表，【“NEWLINE”，空，行号，“”】）",
    "",
    "如果 长度（括号栈） 大于 0：",
    "开 = 括号栈【-1】",
    "语法出错（“括号「” + 开【1】 + “」没有关上”，开【2】）",
    "重复 长度（缩进栈） - 1 次：",
    "添加（记号表，【“DEDENT”，空，长度（源码行表），“”】）",
    "添加（记号表，【“EOF”，空，长度（源码行表），“”】）",
    "返回 记号表",
    "",
    "",
    "# ======================================================================",
    "# 二、语法分析：把记号排成一棵语法树",
    "# ======================================================================",
    "# 递归下降：每个「解析××」函数负责认出一种语法结构，返回一个节点。",
    "# 节点是【种类，行号，其他内容……】，例如：",
    "#     【“num”，3，“42”】                    第 3 行的数字 42",
    "#     【“binop”，3，“+”，左边，右边】        第 3 行的一次加法",
    "#",
    "# 表达式的优先级，从低到高：",
    "#     或者 < 并且 < 不是 < 比较 < + - < * / % < 负号 < 调用和下标",
    "",
    "记号 = 【】",
    "游标 = 0",
    "循环层数 = 0          # 现在套在几层循环里（检查「跳出」「继续」用）",
    "函数层数 = 0          # 现在套在几层函数里（检查「返回」用）",
    "",
    "",
    "函数 看（）：",
    "返回 记号【游标】",
    "",
    "",
    "函数 前进（）：",
    "全局 游标",
    "这个 = 记号【游标】",
    "如果 这个【0】 不是 “EOF”：",
    "游标 += 1",
    "返回 这个",
    "",
    "",
    "函数 是这个（种类，值）：",
    "# 当前记号是不是这个种类、这个值。值给「空」表示不管值是什么",
    "这个 = 记号【游标】",
    "如果 这个【0】 不是 种类：",
    "返回 假",
    "返回 值 是 空 或者 这个【1】 是 值",
    "",
    "",
    "函数 接受（种类，值）：",
    "如果 是这个（种类，值）：",
    "返回 前进（）",
    "返回 空",
    "",
    "",
    "函数 说（某记号）：",
    "# 把一个记号说成人话，报错时用",
    "种类 = 某记号【0】",
    "如果 种类 是 “NEWLINE”：返回 “换行”",
    "如果 种类 是 “INDENT”：返回 “缩进”",
    "如果 种类 是 “DEDENT”：返回 “缩进的结束”",
    "如果 种类 是 “EOF”：返回 “程序的结尾”",
    "返回 “「” + 某记号【3】 + “」”",
    "",
    "",
    "函数 语法错（消息）：",
    "这个 = 记号【游标】",
    "行 = 这个【2】",
    "如果 这个【0】 是 “EOF” 并且 游标 大于 0：       # 程序突然结束：指向最后一行有内容的地方",
    "行 = 记号【游标 - 1】【2】",
    "名字们 = 【】",
    "对于 记号 里的每个 某个：",
    "如果 某个【0】 是 “NAME” 并且 某个【2】 是 行：",
    "添加（名字们，某个【1】）",
    "语法出错（消息 + 空格提示（名字们），行）",
    "",
    "",
    "函数 期待（种类，值）：",
    "如果 不是 是这个（种类，值）：",
    "想要 = “「” + 值 + “」”",
    "如果 值 是 “:”：想要 = “冒号「：」”",
    "如果 值 是 “(”：想要 = “左括号「（」”",
    "如果 值 是 “)”：想要 = “右括号「）」”",
    "如果 值 是 “]”：想要 = “右方括号「】」”",
    "语法错（“这里需要” + 想要 + “，但出现的是” + 说（看（）））",
    "返回 前进（）",
    "",
    "",
    "函数 期待名字（什么）：",
    "这个 = 看（）",
    "如果 这个【0】 是 “NAME”：",
    "返回 前进（）【1】",
    "如果 这个【0】 是 “KW” 或者 查找（文字运算符，这个【3】） 大于等于 0：",
    "语法错（“「” + 这个【3】 + “」是白话语言自己要用的词，不能拿来当” + 什么）",
    "语法错（“这里需要一个” + 什么 + “，但出现的是” + 说（这个））",
    "",
    "",
    "函数 语句结束了（）：",
    "返回 是这个（“NEWLINE”，空） 或者 是这个（“EOF”，空） 或者 是这个（“OP”，“;”）",
    "",
    "",
    "函数 并入（表，新的）：",
    "对于 新的 里的每个 项：",
    "添加（表，项）",
    "",
    "",
    "# ---------- 语句 ----------",
    "",
    "函数 解析程序（）：",
    "语句表 = 【】",
    "当 不是 是这个（“EOF”，空）：",
    "并入（语句表，解析语句（））",
    "返回 语句表",
    "",
    "",
    "函数 解析语句（）：",
    "# 解析一行。返回列表，因为一行里可以用 ； 或 。 隔开写好几句",
    "这个 = 看（）",
    "如果 这个【0】 是 “INDENT”：",
    "语法错（“这一行不该缩进：只有冒号下面的内容才需要缩进”）",
    "如果 这个【0】 是 “KW”：",
    "词 = 这个【1】",
    "如果 词 是 “如果”：返回 【解析如果（）】",
    "如果 词 是 “当”：返回 【解析当（）】",
    "如果 词 是 “对于”：返回 【解析对于（）】",
    "如果 词 是 “重复”：返回 【解析重复（）】",
    "如果 词 是 “函数”：返回 【解析函数（）】",
    "返回 解析简单行（）",
    "",
    "",
    "函数 解析简单行（）：",
    "语句表 = 【解析简单语句（）】",
    "当 接受（“OP”，“;”）：",
    "如果 是这个（“NEWLINE”，空） 或者 是这个（“EOF”，空）：",
    "跳出                              # 行尾的句号或分号，可写可不写",
    "添加（语句表，解析简单语句（））",
    "如果 不是 （是这个（“EOF”，空） 或者 接受（“NEWLINE”，空））：",
    "这个 = 看（）",
    "消息 = “这一句到这里应该结束了，但后面还有” + 说（这个）",
    "最后 = 语句表【-1】",
    "如果 最后【0】 是 “expr” 并且 最后【2】【0】 是 “name” 并且 查找（【“NUM”，“STR”，“NAME”】，这个【0】） 大于等于 0：",
    "名 = 最后【2】【2】",
    "消息 = 消息 + “\\n提示：如果是想调用「” + 名 + “」，要加括号，例如：” + 名 + “（” + 这个【3】 + “）”",
    "否则如果 这个【3】 是 “、”：",
    "消息 = 消息 + “\\n提示：顿号在这里相当于逗号。如果想做除法，请写 / 或者「除以」。”",
    "语法错（消息）",
    "返回 语句表",
    "",
    "",
    "函数 解析简单语句（）：",
    "这个 = 看（）",
    "行 = 这个【2】",
    "如果 这个【0】 是 “KW”：",
    "词 = 这个【1】",
    "如果 词 是 “返回”：",
    "如果 函数层数 是 0：",
    "语法错（“「返回」只能写在函数里面”）",
    "前进（）",
    "值 = 空",
    "如果 不是 语句结束了（）：",
    "值 = 解析表达式（）",
    "返回 【“return”，行，值】",
    "如果 词 是 “跳出” 或者 词 是 “继续”：",
    "如果 循环层数 是 0：",
    "语法错（“「” + 词 + “」只能写在循环里面”）",
    "前进（）",
    "如果 词 是 “跳出”：",
    "返回 【“break”，行】",
    "返回 【“continue”，行】",
    "如果 词 是 “全局”：",
    "前进（）",
    "名字们 = 【期待名字（“变量名”）】",
    "当 接受（“OP”，“,”）：",
    "添加（名字们，期待名字（“变量名”））",
    "返回 【“global”，行，名字们】",
    "",
    "式 = 解析表达式（）",
    "符 = 看（）",
    "如果 符【0】 是 “OP” 并且 查找（赋值符号，符【1】） 大于等于 0：",
    "如果 式【0】 不是 “name” 并且 式【0】 不是 “index”：",
    "语法错（“「” + 符【3】 + “」的左边只能是名字，或者 列表【下标】”）",
    "前进（）",
    "返回 【“assign”，行，式，符【1】，解析表达式（）】",
    "返回 【“expr”，行，式】",
    "",
    "",
    "函数 解析块（）：",
    "# 冒号后面的代码块：换行后缩进的若干行，或者直接写在同一行的一句",
    "如果 是这个（“OP”，“=”）：",
    "语法错（“判断是否相等要写「==」「等于」或者「是」，单独一个「=」是赋值”）",
    "期待（“OP”，“:”）",
    "如果 不是 是这个（“NEWLINE”，空）：",
    "返回 解析简单行（）",
    "前进（）",
    "如果 不是 接受（“INDENT”，空）：",
    "语法错（“冒号下面的内容需要缩进（开头空四格）”）",
    "语句表 = 【】",
    "当 不是 是这个（“DEDENT”，空） 并且 不是 是这个（“EOF”，空）：",
    "并入（语句表，解析语句（））",
    "接受（“DEDENT”，空）",
    "返回 语句表",
    "",
    "",
    "函数 解析循环体（）：",
    "全局 循环层数",
    "循环层数 += 1",
    "体 = 解析块（）",
    "循环层数 -= 1",
    "返回 体",
    "",
    "",
    "函数 解析如果（）：",
    "行 = 前进（）【2】                        # 吃掉「如果」",
    "条件 = 解析表达式（）",
    "分支们 = 【【条件，解析块（）】】",
    "别的 = 空",
    "当 真：",
    "如果 接受（“KW”，“否则如果”）：",
    "条件 = 解析表达式（）",
    "添加（分支们，【条件，解析块（）】）",
    "否则如果 接受（“KW”，“否则”）：",
    "如果 接受（“KW”，“如果”）：          # 「否则 如果」中间有空格也行",
    "条件 = 解析表达式（）",
    "添加（分支们，【条件，解析块（）】）",
    "否则：",
    "别的 = 解析块（）",
    "跳出",
    "否则：",
    "跳出",
    "返回 【“if”，行，分支们，别的】",
    "",
    "",
    "函数 解析当（）：",
    "行 = 前进（）【2】",
    "条件 = 解析表达式（）",
    "返回 【“while”，行，条件，解析循环体（）】",
    "",
    "",
    "函数 解析对于（）：",
    "行 = 前进（）【2】",
    "来源 = 解析表达式（）",
    "如果 不是 （接受（“KW”，“里的每个”） 或者 接受（“KW”，“中的每个”））：",
    "语法错（“「对于」的写法是：对于 列表 里的每个 名字：”）",
    "名 = 期待名字（“变量名”）",
    "返回 【“foreach”，行，来源，名，解析循环体（）】",
    "",
    "",
    "函数 解析重复（）：",
    "行 = 前进（）【2】",
    "次数 = 解析表达式（）",
    "如果 不是 接受（“KW”，“次”）：",
    "语法错（“「重复」的写法是：重复 3 次：”）",
    "返回 【“repeat”，行，次数，解析循环体（）】",
    "",
    "",
    "函数 解析函数（）：",
    "全局 循环层数，函数层数",
    "行 = 前进（）【2】",
    "名 = 期待名字（“函数名”）",
    "期待（“OP”，“(”）",
    "参数们 = 【】",
    "当 不是 是这个（“OP”，“)”）：",
    "参数 = 期待名字（“参数名”）",
    "如果 查找（参数们，参数） 大于等于 0：",
    "语法错（“参数名「” + 参数 + “」重复了”）",
    "添加（参数们，参数）",
    "如果 不是 接受（“OP”，“,”）：",
    "跳出",
    "期待（“OP”，“)”）",
    "# 函数体是一个全新的环境：外面的循环管不到里面的「跳出」",
    "外面的循环 = 循环层数",
    "循环层数 = 0",
    "函数层数 += 1",
    "体 = 解析块（）",
    "函数层数 -= 1",
    "循环层数 = 外面的循环",
    "返回 【“def”，行，名，参数们，体】",
    "",
    "",
    "# ---------- 表达式：优先级越低的越先被调用 ----------",
    "",
    "函数 解析表达式（）：",
    "返回 解析或（）",
    "",
    "",
    "函数 解析或（）：",
    "左 = 解析且（）",
    "当 是这个（“OP”，“or”）：",
    "行 = 前进（）【2】",
    "左 = 【“or”，行，左，解析且（）】",
    "返回 左",
    "",
    "",
    "函数 解析且（）：",
    "左 = 解析非（）",
    "当 是这个（“OP”，“and”）：",
    "行 = 前进（）【2】",
    "左 = 【“and”，行，左，解析非（）】",
    "返回 左",
    "",
    "",
    "函数 解析非（）：",
    "如果 是这个（“OP”，“not”） 或者 是这个（“OP”，“!”）：",
    "行 = 前进（）【2】                    # 写在最前面的「不是」：取反",
    "返回 【“not”，行，解析非（）】",
    "返回 解析比较（）",
    "",
    "",
    "函数 解析比较（）：",
    "首 = 解析加减（）",
    "行 = 看（）【2】",
    "其余 = 【】",
    "当 真：",
    "这个 = 看（）",
    "如果 这个【0】 是 “OP” 并且 查找（比较符号，这个【1】） 大于等于 0：",
    "符 = 这个【1】",
    "否则如果 这个【0】 是 “OP” 并且 这个【1】 是 “not”：",
    "符 = “!=”                         # 夹在两个值中间的「不是」：不等于",
    "否则：",
    "跳出",
    "前进（）",
    "添加（其余，【符，解析加减（）】）",
    "如果 长度（其余） 是 0：",
    "返回 首",
    "返回 【“compare”，行，首，其余】          # 可以连着比：1 小于 x 小于 10",
    "",
    "",
    "函数 解析加减（）：",
    "左 = 解析乘除（）",
    "当 看（）【0】 是 “OP” 并且 （看（）【1】 是 “+” 或者 看（）【1】 是 “-”）：",
    "这个 = 前进（）",
    "左 = 【“binop”，这个【2】，这个【1】，左，解析乘除（）】",
    "返回 左",
    "",
    "",
    "函数 解析乘除（）：",
    "左 = 解析一元（）",
    "当 看（）【0】 是 “OP” 并且 查找（【“*”，“/”，“%”】，看（）【1】） 大于等于 0：",
    "这个 = 前进（）",
    "左 = 【“binop”，这个【2】，这个【1】，左，解析一元（）】",
    "返回 左",
    "",
    "",
    "函数 解析一元（）：",
    "如果 是这个（“OP”，“-”）：",
    "行 = 前进（）【2】",
    "返回 【“neg”，行，解析一元（）】",
    "返回 解析后缀（）",
    "",
    "",
    "函数 解析后缀（）：",
    "节点 = 解析基本（）",
    "当 真：",
    "如果 是这个（“OP”，“(”）：             # 函数调用：名字（参数，参数）",
    "行 = 前进（）【2】",
    "节点 = 【“call”，行，节点，解析一串（“)”）】",
    "否则如果 是这个（“OP”，“[”）：         # 取下标：列表【0】",
    "行 = 前进（）【2】",
    "下标 = 解析表达式（）",
    "期待（“OP”，“]”）",
    "节点 = 【“index”，行，节点，下标】",
    "否则：",
    "返回 节点",
    "",
    "",
    "函数 解析一串（收尾）：",
    "# 解析用逗号隔开的一串表达式，直到遇见右括号",
    "项们 = 【】",
    "当 不是 是这个（“OP”，收尾）：",
    "添加（项们，解析表达式（））",
    "如果 不是 接受（“OP”，“,”）：",
    "跳出",
    "期待（“OP”，收尾）",
    "返回 项们",
    "",
    "",
    "函数 解析基本（）：",
    "这个 = 看（）",
    "种类 = 这个【0】",
    "如果 种类 是 “NUM”：",
    "前进（）",
    "返回 【“num”，这个【2】，这个【1】】",
    "如果 种类 是 “STR”：",
    "前进（）",
    "返回 【“str”，这个【2】，这个【1】】",
    "如果 种类 是 “NAME”：",
    "前进（）",
    "返回 【“name”，这个【2】，这个【1】】",
    "如果 种类 是 “KW” 并且 （这个【1】 是 “真” 或者 这个【1】 是 “假” 或者 这个【1】 是 “空”）：",
    "前进（）",
    "返回 【“const”，这个【2】，这个【1】】",
    "如果 种类 是 “OP” 并且 这个【1】 是 “(”：",
    "前进（）",
    "式 = 解析表达式（）",
    "期待（“OP”，“)”）",
    "返回 式",
    "如果 种类 是 “OP” 并且 这个【1】 是 “[”：       # 列表：【1，2，3】  字典：【“甲”：1，“乙”：2】  空字典：【：】",
    "前进（）",
    "如果 接受（“OP”，“:”）：",
    "期待（“OP”，“]”）",
    "返回 【“dict”，这个【2】，【】】",
    "如果 接受（“OP”，“]”）：",
    "返回 【“list”，这个【2】，【】】",
    "首 = 解析表达式（）",
    "如果 不是 接受（“OP”，“:”）：",
    "项们 = 【首】",
    "如果 接受（“OP”，“,”）：",
    "并入（项们，解析一串（“]”））",
    "否则：",
    "期待（“OP”，“]”）",
    "返回 【“list”，这个【2】，项们】",
    "对们 = 【【首，解析表达式（）】】",
    "当 接受（“OP”，“,”）：",
    "如果 是这个（“OP”，“]”）：",
    "跳出",
    "键 = 解析表达式（）",
    "期待（“OP”，“:”）",
    "添加（对们，【键，解析表达式（）】）",
    "期待（“OP”，“]”）",
    "返回 【“dict”，这个【2】，对们】",
    "如果 种类 是 “KW”：",
    "语法错（“「” + 这个【3】 + “」不能出现在这里（它是白话语言的关键字，也不能当名字用）”）",
    "语法错（“这里缺了一个值，出现的是” + 说（这个））",
    "",
    "",
    "# ======================================================================",
    "# 三、生成代码：沿着语法树走一遍，写出对应的 C 代码",
    "# ======================================================================",
    "# 每个子表达式的结果都先放进一个临时变量，再拿去用。",
    "# 这样写出来的 C 很啰嗦，但求值的先后顺序和白话程序一模一样。",
    "# 一句话执行完，它用过的临时变量就可以给下一句接着用。",
    "#",
    "# 临时变量有两种：",
    "#     t0、t1……   普通的 C 局部变量，C 编译器可以把它们放进寄存器，最快",
    "#     T[0]……     登记给垃圾回收器的数组里的一格",
    "# 回收只会发生在调用函数和循环转圈的时候。一个值如果在算后面的东西时还要留着，",
    "# 而后面那些东西里有函数调用，就得先把它挪进 T，免得被当成垃圾收走（见「保根」）。",
    "#",
    "# 没有循环、也不调用别的白话函数的函数叫「叶子函数」：执行过程中不可能回收，",
    "# 连变量都不用登记，和普通的 C 函数一样快。",
    "# 调用内置函数（而且这个名字在程序里从来没被赋过值）时，直接调用 C 写的那个函数。",
    "#",
    "# 变量放在哪里：",
    "#     最外层的变量     C 数组 G 里的一格",
    "#     函数里的变量     C 数组 L 里的一格。如果函数里面还定义了函数，L 就放到堆上，",
    "#                      内层函数通过 up 指针找到它",
    "# 规则和解释器完全一样：读变量时从里往外找；在函数里赋值只影响函数自己的变量，",
    "# 想改最外层的变量，要先写「全局 名字」。",
    "",
    "全局名表 = 【】          # 最外层变量的名字，下标就是它在 G 里的位置",
    "常量表 = 【】            # 程序里出现过的文字和大整数，下标就是它在 K 里的位置（开头一个字标明是「文」还是「数」）",
    "函数成品 = 【】          # 每个函数生成好的 C 代码",
    "函数个数 = 0",
    "环境 = 空                # 正在编译的那个函数的信息，是下面这样的一张表",
    "内置名表 = 【】          # 运行时里有哪些内置函数（从运行时的源码里读出来）",
    "内置表 = 【】            # 和内置名表一一对应：【名字，C 函数名，最少几个参数，最多几个参数】",
    "全局被赋值 = 【】        # 程序里哪些最外层的名字被赋过值（这样的名字不能当成内置函数直接调用）",
    "直接调用过 = 【】        # 直接调用过哪些内置函数",
    "",
    "# 环境这张表里各个位置的含义",
    "代码位 = 0               # 已经生成的 C 代码，一行一项",
    "局部位 = 1               # 函数里的变量名，下标就是它在 L 里的位置（参数排在最前面）",
    "参数位 = 2               # 有几个参数",
    "声明位 = 3               # 用「全局」声明过的名字",
    "内层位 = 4               # 函数里面是不是还定义了函数",
    "临时位 = 5               # 已经用掉几个临时变量",
    "行位 = 6                 # C 代码里的 bh_line 现在是几（-1 表示不确定）",
    "外层位 = 7               # 外面一层的环境",
    "模块位 = 8               # 是不是最外层",
    "缩进位 = 9               # 生成的 C 代码缩进几层",
    "最多位 = 10              # 普通临时变量最多同时用到几个",
    "整型位 = 11              # 已经用掉几个 C 的整型变量（循环计数用）",
    "根位 = 12                # 已经用掉几个登记过的临时变量（T 里的格）",
    "最多根位 = 13            # 登记过的临时变量最多同时用到几个（决定数组 T 要多大）",
    "叶子位 = 14              # 是不是叶子函数",
    "",
    "",
    "函数 新环境（外层，局部，参数数，声明，有内层，是模块）：",
    "返回 【【】，局部，参数数，声明，有内层，0，-1，外层，是模块，1，0，0，0，0，假】",
    "",
    "",
    "函数 出（文本）：",
    "添加（环境【代码位】，“    ” * 环境【缩进位】 + 文本）",
    "",
    "",
    "函数 开块（文本）：",
    "出（文本）",
    "环境【缩进位】 += 1",
    "",
    "",
    "函数 关块（）：",
    "环境【缩进位】 -= 1",
    "出（“}”）",
    "环境【行位】 = -1",
    "",
    "",
    "函数 否则块（）：",
    "环境【缩进位】 -= 1",
    "出（“} else {”）",
    "环境【缩进位】 += 1",
    "环境【行位】 = -1",
    "",
    "",
    "函数 新临时（）：",
    "# 普通的临时变量：C 的局部变量 t0、t1……",
    "格 = 环境【临时位】",
    "环境【临时位】 += 1",
    "如果 环境【临时位】 大于 环境【最多位】：",
    "环境【最多位】 = 环境【临时位】",
    "返回 “t” + 格",
    "",
    "",
    "函数 根临时（）：",
    "# 登记过的临时变量：数组 T 里的一格。函数开头会把这个数组登记给垃圾回收器",
    "如果 环境【叶子位】：",
    "报错（“编译器内部错误：叶子函数里不该需要登记临时变量”）",
    "退出（3）",
    "格 = 环境【根位】",
    "环境【根位】 += 1",
    "如果 环境【根位】 大于 环境【最多根位】：",
    "环境【最多根位】 = 环境【根位】",
    "返回 “T[” + 格 + “]”",
    "",
    "",
    "函数 不用保护（式）：",
    "# 常数、已经登记过的格、函数自己的参数：不会被回收器收走",
    "头 = 截取（式，0，2）",
    "如果 头 是 “T[” 或者 头 是 “L[” 或者 头 是 “K[” 或者 头 是 “BH”：",
    "返回 真",
    "返回 截取（式，0，7） 是 “bh_int(” 或者 截取（式，0，9） 是 “bh_float(”",
    "",
    "",
    "函数 保根（式）：",
    "# 这个值在算后面的东西时还要留着，而后面可能发生回收：挪进登记过的格里",
    "如果 不用保护（式）：",
    "返回 式",
    "格 = 根临时（）",
    "出（格 + “ = ” + 式 + “;”）",
    "返回 格",
    "",
    "",
    "函数 新整型（）：",
    "环境【整型位】 += 1",
    "返回 “n” + 环境【整型位】",
    "",
    "",
    "函数 定行（行）：",
    "# 出错时要报行号，所以执行到哪一行，就把行号记到 bh_line 里",
    "如果 环境【行位】 不是 行：",
    "出（“bh_line = ” + 行 + “;”）",
    "环境【行位】 = 行",
    "",
    "",
    "函数 全局槽（名）：",
    "号 = 查找（全局名表，名）",
    "如果 号 小于 0：",
    "添加（全局名表，名）",
    "号 = 长度（全局名表） - 1",
    "返回 号",
    "",
    "",
    "函数 常量槽（文）：",
    "号 = 查找（常量表，文）",
    "如果 号 小于 0：",
    "添加（常量表，文）",
    "号 = 长度（常量表） - 1",
    "返回 号",
    "",
    "",
    "函数 C文字（文）：",
    "# 把一段文字写成 C 的字符串常量",
    "片 = 【“\"”】",
    "对于 文 里的每个 字：",
    "码 = 编码（字）",
    "如果 字 是 “\\\\”：",
    "添加（片，“\\\\\\\\”）",
    "否则如果 字 是 “\"”：",
    "添加（片，“\\\\\"”）",
    "否则如果 字 是 “\?”：",
    "添加（片，“\\\\\?”）",
    "否则如果 字 是 “\\n”：",
    "添加（片，“\\\\n”）",
    "否则如果 字 是 “\\t”：",
    "添加（片，“\\\\t”）",
    "否则如果 码 小于 32 或者 码 是 127：",
    "添加（片，“\\\\” + 整数（码 / 64） + 整数（码 / 8） % 8 + 码 % 8）",
    "否则：",
    "添加（片，字）",
    "添加（片，“\"”）",
    "返回 连接（片）",
    "",
    "",
    "函数 数字常量（文本）：",
    "如果 查找（文本，“.”） 大于等于 0：",
    "返回 “bh_float(” + 文本 + “)”",
    "如果 长度（文本） 大于 18：                # 64 位可能装不下：当作大整数常量，程序启动时算好",
    "返回 “K[” + 常量槽（“数” + 文本） + “]”",
    "返回 “bh_int(” + 文本 + “LL)”",
    "",
    "",
    "函数 至少一（数）：",
    "如果 数 小于 1：",
    "返回 1",
    "返回 数",
    "",
    "",
    "函数 记下（表，名）：",
    "如果 查找（表，名） 小于 0：",
    "添加（表，名）",
    "",
    "",
    "函数 收集（语句表，局部，声明，标记）：",
    "# 编译一个函数之前先扫一遍：哪些名字会被赋值（它们是这个函数自己的变量）、",
    "# 哪些名字声明过「全局」、里面有没有再定义函数（记在 标记【0】）",
    "对于 语句表 里的每个 句：",
    "种类 = 句【0】",
    "如果 种类 是 “assign”：",
    "如果 句【2】【0】 是 “name”：",
    "记下（局部，句【2】【2】）",
    "否则如果 种类 是 “if”：",
    "对于 句【2】 里的每个 分支：",
    "收集（分支【1】，局部，声明，标记）",
    "如果 句【3】 不是 空：",
    "收集（句【3】，局部，声明，标记）",
    "否则如果 种类 是 “while” 或者 种类 是 “repeat”：",
    "收集（句【3】，局部，声明，标记）",
    "否则如果 种类 是 “foreach”：",
    "记下（局部，句【3】）",
    "收集（句【4】，局部，声明，标记）",
    "否则如果 种类 是 “def”：",
    "记下（局部，句【2】）",
    "标记【0】 = 真",
    "否则如果 种类 是 “global”：",
    "对于 句【2】 里的每个 名：",
    "记下（声明，名）",
    "",
    "",
    "函数 读内置表（运行时）：",
    "# 运行时的源码里有一张内置函数表：{\"长度\", bi_len, 1, 1}, ……  把它读出来",
    "全局 内置名表，内置表",
    "内置名表 = 【】",
    "内置表 = 【】",
    "起 = 查找（运行时，“bh_builtins[] = {”）",
    "止 = 查找（运行时，“{NULL, NULL”）",
    "如果 起 小于 0 或者 止 小于 起：",
    "返回",
    "片们 = 分割（截取（运行时，起，止 - 起），“{\"”）",
    "下标 = 1",
    "当 下标 小于 长度（片们）：",
    "两半 = 分割（片们【下标】，“\"”）",
    "项们 = 分割（分割（两半【1】，“}”）【0】，“,”）",
    "添加（内置名表，两半【0】）",
    "添加（内置表，【两半【0】，去空白（项们【1】），整数（项们【2】），整数（项们【3】）】）",
    "下标 += 1",
    "",
    "",
    "函数 找函数（语句表，结果）：",
    "# 找出这些语句里（不算更里面的函数）定义的所有函数",
    "对于 语句表 里的每个 句：",
    "种类 = 句【0】",
    "如果 种类 是 “def”：",
    "添加（结果，句）",
    "否则如果 种类 是 “if”：",
    "对于 句【2】 里的每个 分支：",
    "找函数（分支【1】，结果）",
    "如果 句【3】 不是 空：",
    "找函数（句【3】，结果）",
    "否则如果 种类 是 “while” 或者 种类 是 “repeat”：",
    "找函数（句【3】，结果）",
    "否则如果 种类 是 “foreach”：",
    "找函数（句【4】，结果）",
    "",
    "",
    "函数 找全局赋值（语句表，是模块）：",
    "# 最外层赋值的名字，以及函数里声明了「全局」又赋了值的名字，都会改动最外层的变量",
    "局部 = 【】",
    "声明 = 【】",
    "标记 = 【假】",
    "收集（语句表，局部，声明，标记）",
    "对于 局部 里的每个 名：",
    "如果 是模块 或者 查找（声明，名） 大于等于 0：",
    "记下（全局被赋值，名）",
    "里面的 = 【】",
    "找函数（语句表，里面的）",
    "对于 里面的 里的每个 句：",
    "找全局赋值（句【4】，假）",
    "",
    "",
    "函数 直接内置（节点）：",
    "# 这次调用能不能直接调用 C 写的内置函数？能的话返回内置表里的那一项",
    "被调 = 节点【2】",
    "如果 被调【0】 不是 “name”：",
    "返回 空",
    "名 = 被调【2】",
    "号 = 查找（内置名表，名）",
    "如果 号 小于 0 或者 查找（全局被赋值，名） 大于等于 0：",
    "返回 空",
    "外 = 环境",
    "当 外 不是 空 并且 不是 外【模块位】：      # 函数自己或者外层函数有同名的变量：不行",
    "如果 查找（外【局部位】，名） 大于等于 0：",
    "返回 空",
    "外 = 外【外层位】",
    "项 = 内置表【号】",
    "个数 = 长度（节点【3】）",
    "如果 个数 小于 项【2】 或者 （项【3】 大于等于 0 并且 个数 大于 项【3】）：",
    "返回 空                                 # 个数不对：走普通的路，运行到这里时再报错",
    "返回 项",
    "",
    "",
    "函数 可能回收（节点）：",
    "# 算这个表达式的过程中，会不会发生垃圾回收（也就是会不会调用白话函数）",
    "种类 = 节点【0】",
    "如果 种类 是 “num” 或者 种类 是 “str” 或者 种类 是 “const” 或者 种类 是 “name”：",
    "返回 假",
    "如果 种类 是 “list”：",
    "返回 一串可能回收（节点【2】）",
    "如果 种类 是 “dict”：",
    "返回 一串可能回收（摊平（节点【2】））",
    "如果 种类 是 “binop”：",
    "返回 可能回收（节点【3】） 或者 可能回收（节点【4】）",
    "如果 种类 是 “compare”：",
    "如果 可能回收（节点【2】）：",
    "返回 真",
    "对于 节点【3】 里的每个 对：",
    "如果 可能回收（对【1】）：",
    "返回 真",
    "返回 假",
    "如果 种类 是 “and” 或者 种类 是 “or”：",
    "返回 可能回收（节点【2】） 或者 可能回收（节点【3】）",
    "如果 种类 是 “not” 或者 种类 是 “neg”：",
    "返回 可能回收（节点【2】）",
    "如果 种类 是 “index”：",
    "返回 可能回收（节点【2】） 或者 可能回收（节点【3】）",
    "如果 直接内置（节点） 不是 空：",
    "返回 一串可能回收（节点【3】）",
    "返回 真",
    "",
    "",
    "函数 摊平（对们）：",
    "# 【【键，值】，【键，值】】 -> 【键，值，键，值】",
    "结果 = 【】",
    "对于 对们 里的每个 对：",
    "添加（结果，对【0】）",
    "添加（结果，对【1】）",
    "返回 结果",
    "",
    "",
    "函数 一串可能回收（节点们）：",
    "对于 节点们 里的每个 节点：",
    "如果 可能回收（节点）：",
    "返回 真",
    "返回 假",
    "",
    "",
    "函数 后面可能回收（节点们）：",
    "# 返回一张表：第 i 项表示第 i 个后面的那些里有没有可能回收",
    "结果 = 【假】 * 长度（节点们）",
    "有 = 假",
    "下标 = 长度（节点们） - 1",
    "当 下标 大于等于 0：",
    "结果【下标】 = 有",
    "如果 可能回收（节点们【下标】）：",
    "有 = 真",
    "下标 -= 1",
    "返回 结果",
    "",
    "",
    "函数 是叶子（语句表）：",
    "# 没有循环，没有定义函数，所有表达式都不会引起回收",
    "对于 语句表 里的每个 句：",
    "种类 = 句【0】",
    "如果 种类 是 “while” 或者 种类 是 “foreach” 或者 种类 是 “repeat” 或者 种类 是 “def”：",
    "返回 假",
    "如果 种类 是 “expr”：",
    "如果 可能回收（句【2】）：",
    "返回 假",
    "否则如果 种类 是 “assign”：",
    "如果 可能回收（句【4】） 或者 可能回收（句【2】）：",
    "返回 假",
    "否则如果 种类 是 “return”：",
    "如果 句【2】 不是 空 并且 可能回收（句【2】）：",
    "返回 假",
    "否则如果 种类 是 “if”：",
    "对于 句【2】 里的每个 分支：",
    "如果 可能回收（分支【0】） 或者 不是 是叶子（分支【1】）：",
    "返回 假",
    "如果 句【3】 不是 空 并且 不是 是叶子（句【3】）：",
    "返回 假",
    "返回 真",
    "",
    "",
    "# ---------- 变量的读和写 ----------",
    "",
    "函数 全局取值（名）：",
    "返回 “bh_chk(G[” + 全局槽（名） + “], ” + C文字（名） + “)”",
    "",
    "",
    "函数 外层取值（名）：",
    "# 自己这一层没有这个变量，就一层一层往外找，最后找到最外层",
    "路径 = “up”",
    "外 = 环境【外层位】",
    "候选 = 【】",
    "当 外 不是 空 并且 不是 外【模块位】：",
    "槽 = 查找（外【局部位】，名）",
    "如果 槽 大于等于 0：",
    "添加（候选，路径 + “->v[” + 槽 + “]”）",
    "路径 = 路径 + “->up”",
    "外 = 外【外层位】",
    "结果 = 全局取值（名）",
    "下标 = 长度（候选） - 1",
    "当 下标 大于等于 0：",
    "结果 = “(” + 候选【下标】 + “.tag \? ” + 候选【下标】 + “ : ” + 结果 + “)”",
    "下标 -= 1",
    "返回 结果",
    "",
    "",
    "函数 读变量（名）：",
    "如果 环境【模块位】：",
    "临 = 新临时（）",
    "出（临 + “ = ” + 全局取值（名） + “;”）",
    "返回 临",
    "槽 = 查找（环境【局部位】，名）",
    "声 = 查找（环境【声明位】，名）",
    "如果 槽 大于等于 0 并且 槽 小于 环境【参数位】 并且 声 小于 0：",
    "返回 “L[” + 槽 + “]”                          # 参数一定有值，一个式子里也不会被改掉，直接用",
    "临 = 新临时（）",
    "如果 槽 大于等于 0：",
    "前缀 = “”",
    "如果 声 大于等于 0：",
    "前缀 = “if (gf” + 声 + “) ” + 临 + “ = ” + 全局取值（名） + “; else ”",
    "出（前缀 + “if (L[” + 槽 + “].tag) ” + 临 + “ = L[” + 槽 + “]; else { or” + 槽 + “ = 1; ” + 临 + “ = ” + 外层取值（名） + “; }”）",
    "否则如果 声 大于等于 0：",
    "出（临 + “ = gf” + 声 + “ \? ” + 全局取值（名） + “ : ” + 外层取值（名） + “;”）",
    "否则：",
    "出（临 + “ = ” + 外层取值（名） + “;”）",
    "返回 临",
    "",
    "",
    "函数 写变量（名，值式）：",
    "如果 环境【模块位】：",
    "出（“G[” + 全局槽（名） + “] = ” + 值式 + “;”）",
    "返回",
    "槽 = 查找（环境【局部位】，名）",
    "声 = 查找（环境【声明位】，名）",
    "本地 = “if (!L[” + 槽 + “].tag && or” + 槽 + “) bh_modify_outer(” + C文字（名） + “); L[” + 槽 + “] = ” + 值式 + “;”",
    "如果 槽 小于 环境【参数位】 并且 声 小于 0：",
    "出（“L[” + 槽 + “] = ” + 值式 + “;”）",
    "否则如果 声 大于等于 0：",
    "出（“if (gf” + 声 + “) G[” + 全局槽（名） + “] = ” + 值式 + “; else { ” + 本地 + “ }”）",
    "否则：",
    "出（本地）",
    "",
    "",
    "# ---------- 表达式 ----------",
    "",
    "函数 运算函数（符）：",
    "如果 符 是 “+”：返回 “bh_add”",
    "如果 符 是 “-”：返回 “bh_sub”",
    "如果 符 是 “*”：返回 “bh_mul”",
    "如果 符 是 “/”：返回 “bh_div”",
    "返回 “bh_mod”",
    "",
    "",
    "函数 算一串（节点们）：",
    "# 从左到右算一串值。某个值后面如果还有可能引起回收的，就要保护起来",
    "式们 = 【】",
    "后面 = 后面可能回收（节点们）",
    "下标 = 0",
    "对于 节点们 里的每个 节点：",
    "式 = 算（节点）",
    "如果 后面【下标】：",
    "式 = 保根（式）",
    "添加（式们，式）",
    "下标 += 1",
    "返回 式们",
    "",
    "",
    "函数 数组式（式们）：",
    "如果 长度（式们） 是 0：",
    "返回 “NULL”",
    "返回 “(Value[]){” + 连接（式们，“, ”） + “}”",
    "",
    "",
    "函数 算（节点）：",
    "# 生成计算这个表达式的 C 代码，返回存着结果的那个 C 表达式",
    "种类 = 节点【0】",
    "行 = 节点【1】",
    "如果 种类 是 “num”：",
    "返回 数字常量（节点【2】）",
    "如果 种类 是 “str”：",
    "返回 “K[” + 常量槽（“文” + 节点【2】） + “]”",
    "如果 种类 是 “const”：",
    "如果 节点【2】 是 “真”：返回 “BH_TRUE”",
    "如果 节点【2】 是 “假”：返回 “BH_FALSE”",
    "返回 “BH_NIL”",
    "如果 种类 是 “name”：",
    "定行（行）",
    "返回 读变量（节点【2】）",
    "如果 种类 是 “list”：",
    "项们 = 算一串（节点【2】）",
    "临 = 新临时（）",
    "出（临 + “ = bh_list_make(” + 长度（项们） + “, ” + 数组式（项们） + “);”）",
    "返回 临",
    "如果 种类 是 “dict”：                        # 先从左到右算出所有的键和值，再一起放进字典",
    "项们 = 算一串（摊平（节点【2】））",
    "定行（行）",
    "临 = 新临时（）",
    "出（临 + “ = bh_dict_make(” + 长度（节点【2】） + “, ” + 数组式（项们） + “);”）",
    "返回 临",
    "如果 种类 是 “binop”：",
    "左 = 算（节点【3】）",
    "如果 可能回收（节点【4】）：",
    "左 = 保根（左）",
    "右 = 算（节点【4】）",
    "定行（行）",
    "临 = 新临时（）",
    "出（临 + “ = ” + 运算函数（节点【2】） + “(” + 左 + “, ” + 右 + “);”）",
    "返回 临",
    "如果 种类 是 “compare”：",
    "返回 算比较（节点）",
    "如果 种类 是 “and” 或者 种类 是 “or”：",
    "返回 算逻辑（节点）",
    "如果 种类 是 “not”：",
    "值 = 算（节点【2】）",
    "临 = 新临时（）",
    "出（临 + “ = bh_bool(!bh_truthy(” + 值 + “));”）",
    "返回 临",
    "如果 种类 是 “neg”：",
    "值 = 算（节点【2】）",
    "定行（行）",
    "临 = 新临时（）",
    "出（临 + “ = bh_neg(” + 值 + “);”）",
    "返回 临",
    "如果 种类 是 “index”：",
    "容器 = 算（节点【2】）",
    "如果 可能回收（节点【3】）：",
    "容器 = 保根（容器）",
    "下标 = 算（节点【3】）",
    "定行（行）",
    "临 = 新临时（）",
    "出（临 + “ = bh_index(” + 容器 + “, ” + 下标 + “);”）",
    "返回 临",
    "# 剩下的是函数调用",
    "项 = 直接内置（节点）",
    "如果 项 不是 空：                            # 内置函数：直接调用 C 函数",
    "记下（直接调用过，项【0】）",
    "参数们 = 算一串（节点【3】）",
    "定行（行）",
    "临 = 新临时（）",
    "出（临 + “ = ” + 项【1】 + “(” + 长度（参数们） + “, ” + 数组式（参数们） + “);”）",
    "返回 临",
    "# 一般的调用：先算函数本身，再从左到右算参数",
    "被调 = 算（节点【2】）",
    "如果 一串可能回收（节点【3】）：",
    "被调 = 保根（被调）",
    "参数们 = 算一串（节点【3】）",
    "定行（行）",
    "名 = “NULL”",
    "如果 节点【2】【0】 是 “name”：",
    "名 = C文字（节点【2】【2】）",
    "临 = 新临时（）",
    "出（临 + “ = bh_call(” + 被调 + “, ” + 长度（参数们） + “, ” + 数组式（参数们） + “, ” + 名 + “);”）",
    "返回 临",
    "",
    "",
    "函数 算比较（节点）：",
    "# 连着比（1 小于 x 小于 10）：一个一个比下去，有一个不成立就不用再比了",
    "临 = 新临时（）",
    "出（临 + “ = BH_FALSE;”）",
    "开块（“{”）",
    "左 = 算（节点【2】）",
    "层数 = 0",
    "对于 节点【3】 里的每个 对：",
    "如果 可能回收（对【1】）：",
    "左 = 保根（左）",
    "右 = 算（对【1】）",
    "定行（节点【1】）",
    "开块（“if (bh_cmp(” + 查找（比较符号，对【0】） + “, ” + 左 + “, ” + 右 + “)) {”）",
    "层数 += 1",
    "左 = 右",
    "出（临 + “ = BH_TRUE;”）",
    "重复 层数 + 1 次：",
    "关块（）",
    "返回 临",
    "",
    "",
    "函数 算逻辑（节点）：",
    "# 并且：左边不成立，就不用算右边了。或者：左边成立，就不用算右边了",
    "临 = 新临时（）",
    "开块（“{”）",
    "左 = 算（节点【2】）",
    "开块（“if (bh_truthy(” + 左 + “)) {”）",
    "如果 节点【0】 是 “and”：",
    "右 = 算（节点【3】）",
    "出（临 + “ = ” + 右 + “;”）",
    "否则块（）",
    "出（临 + “ = ” + 左 + “;”）",
    "否则：",
    "出（临 + “ = ” + 左 + “;”）",
    "否则块（）",
    "右 = 算（节点【3】）",
    "出（临 + “ = ” + 右 + “;”）",
    "关块（）",
    "关块（）",
    "返回 临",
    "",
    "",
    "# ---------- 语句 ----------",
    "",
    "函数 编语句表（语句表）：",
    "对于 语句表 里的每个 句：",
    "编语句（句）",
    "",
    "",
    "函数 编语句（句）：",
    "种类 = 句【0】",
    "行 = 句【1】",
    "定行（行）",
    "用到这里 = 环境【临时位】",
    "根用到这里 = 环境【根位】",
    "如果 种类 是 “expr”：",
    "算（句【2】）",
    "否则如果 种类 是 “assign”：",
    "编赋值（句）",
    "否则如果 种类 是 “if”：",
    "编如果（句）",
    "否则如果 种类 是 “while”：",
    "开块（“for (;;) {”）",
    "出（“BH_SAFEPOINT();”）",
    "环境【行位】 = -1",
    "条件 = 算（句【2】）",
    "出（“if (!bh_truthy(” + 条件 + “)) break;”）",
    "编语句表（句【3】）",
    "关块（）",
    "否则如果 种类 是 “foreach” 并且 是范围调用（句【2】）：",
    "# 对于 范围（……）里的每个：参数都是普通整数时不造列表，边数边给（见运行时的 bh_range_begin）",
    "开块（“{”）",
    "参数们 = 算一串（句【2】【3】）",
    "定行（句【2】【1】）",
    "份 = 根临时（）",
    "号 = 新整型（）",
    "区 = “r” + 号",
    "出（“BhRange ” + 区 + “;”）",
    "出（“int64_t ” + 号 + “;”）",
    "出（份 + “ = bh_range_begin(&” + 区 + “, ” + 长度（参数们） + “, ” + 数组式（参数们） + “);”）",
    "开块（“for (” + 号 + “ = 0; ” + 号 + “ < ” + 区 + “.count; ” + 号 + “++) {”）",
    "出（“BH_SAFEPOINT();”）",
    "环境【行位】 = -1",
    "定行（行）",
    "写变量（句【3】，“bh_range_at(&” + 区 + “, ” + 份 + “, ” + 号 + “)”）",
    "编语句表（句【4】）",
    "关块（）",
    "关块（）",
    "否则如果 种类 是 “foreach”：",
    "# 先把列表抄一份再逐个取出，循环里改原来的列表也不受影响",
    "开块（“{”）",
    "来源 = 算（句【2】）",
    "定行（行）",
    "份 = 根临时（）                        # 整个循环期间都要留着",
    "号 = 新整型（）",
    "出（份 + “ = bh_iter(” + 来源 + “);”）",
    "出（“int64_t ” + 号 + “;”）",
    "开块（“for (” + 号 + “ = 0; ” + 号 + “ < bh_list_len(” + 份 + “); ” + 号 + “++) {”）",
    "出（“BH_SAFEPOINT();”）",
    "环境【行位】 = -1",
    "定行（行）",
    "写变量（句【3】，“bh_list_at(” + 份 + “, ” + 号 + “)”）",
    "编语句表（句【4】）",
    "关块（）",
    "关块（）",
    "否则如果 种类 是 “repeat”：",
    "开块（“{”）",
    "次数 = 算（句【2】）",
    "定行（行）",
    "总 = 新整型（）",
    "号 = 新整型（）",
    "出（“int64_t ” + 总 + “ = bh_count(” + 次数 + “), ” + 号 + “;”）",
    "开块（“for (” + 号 + “ = 0; ” + 号 + “ < ” + 总 + “; ” + 号 + “++) {”）",
    "出（“BH_SAFEPOINT();”）",
    "环境【行位】 = -1",
    "编语句表（句【3】）",
    "关块（）",
    "关块（）",
    "否则如果 种类 是 “def”：",
    "式 = 编函数（句）",
    "定行（行）",
    "写变量（句【2】，式）",
    "否则如果 种类 是 “return”：",
    "值 = “BH_NIL”",
    "如果 句【2】 不是 空：",
    "值 = 算（句【2】）",
    "如果 环境【叶子位】：",
    "出（“return ” + 值 + “;”）",
    "否则：",
    "出（“BH_RETURN(fr, ” + 值 + “);”）",
    "否则如果 种类 是 “break”：",
    "出（“break;”）",
    "否则如果 种类 是 “continue”：",
    "出（“continue;”）",
    "否则如果 种类 是 “global”：",
    "如果 不是 环境【模块位】：",
    "对于 句【2】 里的每个 名：",
    "出（“gf” + 查找（环境【声明位】，名） + “ = 1;”）",
    "环境【临时位】 = 用到这里",
    "环境【根位】 = 根用到这里",
    "",
    "",
    "函数 是范围调用（式）：",
    "# 是不是直接调用内置的「范围」",
    "如果 式【0】 不是 “call”：",
    "返回 假",
    "项 = 直接内置（式）",
    "返回 项 不是 空 并且 项【0】 是 “范围”",
    "",
    "",
    "函数 编赋值（句）：",
    "行 = 句【1】",
    "目标 = 句【2】",
    "运算 = 句【3】",
    "值 = 算（句【4】）",
    "如果 运算 不是 “=”：                       # 甲 += 1 就是 甲 = 甲 + 1",
    "如果 可能回收（目标）：",
    "值 = 保根（值）",
    "旧 = 算（目标）",
    "定行（行）",
    "临 = 新临时（）",
    "出（临 + “ = ” + 运算函数（截取（运算，0，1）） + “(” + 旧 + “, ” + 值 + “);”）",
    "值 = 临",
    "如果 目标【0】 是 “name”：",
    "定行（行）",
    "写变量（目标【2】，值）",
    "否则：                                     # 列表【下标】 = 值",
    "如果 可能回收（目标）：",
    "值 = 保根（值）",
    "容器 = 算（目标【2】）",
    "如果 可能回收（目标【3】）：",
    "容器 = 保根（容器）",
    "下标 = 算（目标【3】）",
    "定行（行）",
    "出（“bh_set_index(” + 容器 + “, ” + 下标 + “, ” + 值 + “);”）",
    "",
    "",
    "函数 编如果（句）：",
    "层数 = 0",
    "对于 句【2】 里的每个 分支：",
    "条件 = 算（分支【0】）",
    "开块（“if (bh_truthy(” + 条件 + “)) {”）",
    "编语句表（分支【1】）",
    "否则块（）",
    "层数 += 1",
    "如果 句【3】 不是 空：",
    "编语句表（句【3】）",
    "重复 层数 次：",
    "关块（）",
    "",
    "",
    "函数 编函数（句）：",
    "# 把一个白话函数编译成一个 C 函数，返回「创建这个函数值」的 C 表达式",
    "全局 环境，函数个数",
    "函数个数 += 1",
    "编号 = 函数个数",
    "局部 = 【】",
    "并入（局部，句【3】）",
    "声明 = 【】",
    "标记 = 【假】",
    "收集（句【4】，局部，声明，标记）",
    "外面 = 环境",
    "环境 = 新环境（外面，局部，长度（句【3】），声明，标记【0】，假）",
    "环境【叶子位】 = 不是 标记【0】 并且 是叶子（句【4】）",
    "编语句表（句【4】）",
    "头 = 【“static Value fn” + 编号 + “(Frame *up, Value *a) {    /* 函数 ” + 句【2】 + “，第 ” + 句【1】 + “ 行 */”】",
    "根数 = 环境【最多根位】",
    "如果 环境【叶子位】：",
    "# 叶子函数：执行过程中不会回收，变量放在普通的 C 数组里就行",
    "添加（头，“    Value L[” + 至少一（长度（局部）） + “] = {{0}};”）",
    "否则如果 标记【0】：",
    "# 里面还定义了函数：局部变量放到堆上，这个函数结束以后内层函数还能接着用",
    "添加（头，“    struct { BhRoots h; Value v[” + （根数 + 1） + “]; } fr;”）",
    "添加（头，“    Frame *self; Value *L, *T;”）",
    "添加（头，“    BH_ENTER(fr, ” + （根数 + 1） + “);”）",
    "添加（头，“    self = bh_frame_new(up, ” + 长度（局部） + “); fr.v[0] = bh_obj(T_FRAME, self); L = self->v; T = fr.v + 1;”）",
    "否则：",
    "内层 = 不是 外面【模块位】          # 定义在函数里面的函数：外层的变量（up）也要登记",
    "格数 = 长度（局部） + 根数",
    "如果 内层：",
    "格数 += 1",
    "添加（头，“    struct { BhRoots h; Value v[” + 至少一（格数） + “]; } fr;”）",
    "添加（头，“    Value *L, *T;”）",
    "添加（头，“    BH_ENTER(fr, ” + 至少一（格数） + “);”）",
    "添加（头，“    L = fr.v; T = fr.v + ” + 长度（局部） + “;”）",
    "如果 内层：",
    "添加（头，“    fr.v[” + （格数 - 1） + “] = bh_obj(T_FRAME, up);”）",
    "添加C临时（头）",
    "下标 = 0",
    "当 下标 小于 长度（局部）：",
    "如果 下标 小于 长度（句【3】）：",
    "添加（头，“    L[” + 下标 + “] = a[” + 下标 + “];    /* ” + 局部【下标】 + “ */”）",
    "添加（头，“    char or” + 下标 + “ = 0;”）",
    "下标 += 1",
    "下标 = 0",
    "当 下标 小于 长度（声明）：",
    "添加（头，“    char gf” + 下标 + “ = 0;”）",
    "下标 += 1",
    "结尾 = “    return BH_NIL;”",
    "如果 不是 环境【叶子位】：",
    "添加（头，“    BH_SAFEPOINT();”）",
    "结尾 = “    BH_RETURN(fr, BH_NIL);”",
    "添加（函数成品，连接（头 + 环境【代码位】 + 【结尾，“}”，“”】，“\\n”））",
    "环境 = 外面",
    "框 = “NULL”",
    "如果 不是 外面【模块位】：",
    "框 = “self”",
    "返回 “bh_func(fn” + 编号 + “, ” + 长度（句【3】） + “, ” + C文字（句【2】） + “, ” + 框 + “)”",
    "",
    "",
    "函数 添加C临时（头）：",
    "# 在函数开头声明用到的普通临时变量 t0、t1……",
    "如果 环境【最多位】 大于 0：",
    "名们 = 【】",
    "下标 = 0",
    "当 下标 小于 环境【最多位】：",
    "添加（名们，“t” + 下标）",
    "下标 += 1",
    "添加（头，“    Value ” + 连接（名们，“, ”） + “;”）",
    "",
    "",
    "函数 编译（源码，文件名，运行时）：",
    "# 整个编译过程：源代码进，C 代码出",
    "全局 源码行表，记号，游标，环境",
    "源码 = 连接（分割（源码，字符（13） + “\\n”），“\\n”）",
    "源码 = 连接（分割（源码，字符（13）），“\\n”）",
    "源码行表 = 分割（源码，“\\n”）",
    "记号 = 词法分析（）",
    "游标 = 0",
    "树 = 解析程序（）",
    "读内置表（运行时）",
    "找全局赋值（树，真）",
    "环境 = 新环境（空，【】，0，【】，假，真）",
    "编语句表（树）",
    "主体 = 环境【代码位】",
    "模块头 = 【】",
    "添加C临时（模块头）",
    "",
    "如果 查找（文件名，“*/”） 大于等于 0：",
    "文件名 = “程序”",
    "成品 = 【“/* 由白话编译器生成，源文件：” + 文件名 + “ */”，运行时】",
    "",
    "名字们 = 【】",
    "对于 全局名表 里的每个 名：",
    "添加（名字们，C文字（名））",
    "添加（名字们，“NULL”）",
    "添加（成品，“static Value G[” + 至少一（长度（全局名表）） + “];”）",
    "添加（成品，“static const char *bh_gnames[] = {” + 连接（名字们，“, ”） + “};”）",
    "添加（成品，“static Value K[” + 至少一（长度（常量表）） + “];”）",
    "",
    "添加（成品，“static const char *bh_srclines[] = {”）",
    "对于 源码行表 里的每个 一行：",
    "添加（成品，“    ” + C文字（去空白（一行）） + “,”）",
    "添加（成品，“    NULL};”）",
    "",
    "带源码 = 查找（全局名表，“运行时源码”） 大于等于 0 或者 查找（直接调用过，“运行时源码”） 大于等于 0   # 只有编译器自己才需要带着运行时的源码",
    "如果 带源码：",
    "添加（成品，“static const char *bh_rtsource[] = {”）",
    "片们 = 分割（运行时，“\\n”）",
    "下标 = 0",
    "当 下标 小于 长度（片们）：",
    "一行 = 片们【下标】",
    "如果 下标 小于 长度（片们） - 1：",
    "一行 = 一行 + “\\n”",
    "添加（成品，“    ” + C文字（一行） + “,”）",
    "下标 += 1",
    "添加（成品，“    NULL};”）",
    "",
    "添加（成品，“”）",
    "下标 = 1",
    "当 下标 小于等于 函数个数：",
    "添加（成品，“static Value fn” + 下标 + “(Frame *up, Value *a);”）",
    "下标 += 1",
    "添加（成品，“”）",
    "并入（成品，函数成品）",
    "格数 = 至少一（环境【最多根位】）",
    "添加（成品，“static void bh_module(void) {”）",
    "添加（成品，“    struct { BhRoots h; Value v[” + 格数 + “]; } fr;”）",
    "添加（成品，“    Value *T;”）",
    "并入（成品，模块头）",
    "添加（成品，“    BH_ENTER(fr, ” + 格数 + “);”）",
    "添加（成品，“    T = fr.v;”）",
    "并入（成品，主体）",
    "添加（成品，“    bh_roots = fr.h.prev;”）",
    "添加（成品，“}”）",
    "添加（成品，“”）",
    "添加（成品，“int main(int argc, char **argv) {”）",
    "添加（成品，“    char base;”）",
    "添加（成品，“    bh_init(argc, argv, &base);”）",
    "添加（成品，“    bh_src = bh_srclines;”）",
    "添加（成品，“    bh_src_n = ” + 长度（源码行表） + “;”）",
    "如果 带源码：",
    "添加（成品，“    bh_rt_lines = bh_rtsource;”）",
    "添加（成品，“    bh_rt_lines_n = ” + 长度（分割（运行时，“\\n”）） + “;”）",
    "添加（成品，“    bh_init_globals(G, bh_gnames, ” + 长度（全局名表） + “, K, ” + 长度（常量表） + “);”）",
    "下标 = 0",
    "当 下标 小于 长度（常量表）：",
    "项 = 常量表【下标】",
    "制造 = “bh_str_lit(”",
    "如果 截取（项，0，1） 是 “数”：",
    "制造 = “bh_big_lit(”",
    "添加（成品，“    K[” + 下标 + “] = ” + 制造 + C文字（截取（项，1，长度（项））） + “);”）",
    "下标 += 1",
    "添加（成品，“    bh_module();”）",
    "添加（成品，“    fflush(stdout);”）",
    "添加（成品，“    return 0;”）",
    "添加（成品，“}”）",
    "添加（成品，“”）",
    "返回 连接（成品，“\\n”）",
    "",
    "",
    "# ======================================================================",
    "# 入口：看命令行参数，读文件，编译，调用 C 编译器",
    "# ======================================================================",
    "",
    "函数 引（文）：",
    "# 给命令里的文件名加上引号，这样名字里有空格也不怕",
    "返回 “'” + 连接（分割（文，“'”），“'\\\\''”） + “'”",
    "",
    "",
    "函数 主程序（）：",
    "参数表 = 命令行参数（）",
    "输入文件 = 空",
    "输出名 = 空",
    "只要C = 假",
    "C编译器 = “cc”",
    "运行时文件 = 空",
    "下标 = 0",
    "当 下标 小于 长度（参数表）：",
    "项 = 参数表【下标】",
    "有下一个 = 下标 + 1 小于 长度（参数表）",
    "如果 项 是 “-o” 并且 有下一个：",
    "输出名 = 参数表【下标 + 1】",
    "下标 += 1",
    "否则如果 项 是 “--cc” 并且 有下一个：",
    "C编译器 = 参数表【下标 + 1】",
    "下标 += 1",
    "否则如果 项 是 “--runtime” 并且 有下一个：",
    "运行时文件 = 参数表【下标 + 1】",
    "下标 += 1",
    "否则如果 项 是 “--emit-c”：",
    "只要C = 真",
    "否则如果 输入文件 是 空 并且 截取（项，0，1） 不是 “-”：",
    "输入文件 = 项",
    "否则：",
    "报错（“看不懂的参数：” + 项）",
    "退出（2）",
    "下标 += 1",
    "如果 输入文件 是 空：",
    "报错（“用法：baihuac 程序.bh [-o 输出名] [--emit-c] [--cc C编译器]”）",
    "退出（2）",
    "",
    "源码 = 读文件（输入文件）",
    "如果 源码 是 空：",
    "报错（“打不开文件：” + 输入文件）",
    "退出（1）",
    "如果 运行时文件 是 空：",
    "运行时 = 运行时源码（）",
    "否则：",
    "运行时 = 读文件（运行时文件）",
    "如果 运行时 是 空：",
    "报错（“打不开文件：” + 运行时文件）",
    "退出（1）",
    "",
    "基名 = 输入文件 + “.out”",
    "如果 长度（输入文件） 大于 3 并且 截取（输入文件，长度（输入文件） - 3，3） 是 “.bh”：",
    "基名 = 截取（输入文件，0，长度（输入文件） - 3）",
    "",
    "C代码 = 编译（源码，输入文件，运行时）",
    "",
    "如果 只要C：",
    "如果 输出名 是 空：",
    "输出名 = 基名 + “.c”",
    "如果 不是 写文件（输出名，C代码）：",
    "报错（“写不了文件：” + 输出名）",
    "退出（1）",
    "返回",
    "",
    "如果 输出名 是 空：",
    "输出名 = 基名",
    "C文件 = 输出名 + “.c”",
    "如果 不是 写文件（C文件，C代码）：",
    "报错（“写不了文件：” + C文件）",
    "退出（1）",
    "状态 = 运行命令（C编译器 + “ -O2 -w -o ” + 引（输出名） + “ ” + 引（C文件） + “ -lm”）",
    "如果 状态 不是 0：",
    "报错（“调用 C 编译器（” + C编译器 + “）没有成功。生成的 C 代码留在 ” + C文件 + “ 里。”）",
    "退出（1）",
    "运行命令（“rm -f ” + 引（C文件））",
    "打印（“编译好了：” + 输出名）",
    "",
    "",
    "主程序（）",
    "",
    NULL};
static const char *bh_rtsource[] = {
    "/* ======================================================================\n",
    " * 白话运行时\n",
    " *\n",
    " * 白话编译器生成的每个 C 程序，开头都带着这一份代码。它负责：\n",
    " * 值的表示、垃圾回收、加减乘除和比较、文字、列表和字典、内置函数、中文报错。\n",
    " * 整数没有上限：64 位装不下时自动换成大整数。\n",
    " * 用不到的内存由垃圾回收器收回（标记清除）。\n",
    " * 出错时列出调用过程：是从哪里一层一层调用到出错的地方的。\n",
    " * ====================================================================== */\n",
    "#include <stdio.h>\n",
    "#include <stdlib.h>\n",
    "#include <string.h>\n",
    "#include <stdint.h>\n",
    "#include <stdarg.h>\n",
    "#include <math.h>\n",
    "#include <time.h>\n",
    "#ifdef _WIN32\n",
    "#include <windows.h>\n",
    "#else\n",
    "#include <sys/resource.h>\n",
    "#include <sys/wait.h>\n",
    "#endif\n",
    "\n",
    "/* ---------- 让 C 编译器生成更快的代码 ---------- */\n",
    "\n",
    "#if defined(__GNUC__) || defined(__clang__)\n",
    "#define BH_LIKELY(x) __builtin_expect(!!(x), 1)\n",
    "#define BH_UNLIKELY(x) __builtin_expect(!!(x), 0)\n",
    "#define BH_INLINE static inline __attribute__((always_inline))\n",
    "#define BH_NOINLINE __attribute__((noinline))\n",
    "#else\n",
    "#define BH_LIKELY(x) (x)\n",
    "#define BH_UNLIKELY(x) (x)\n",
    "#define BH_INLINE static inline\n",
    "#define BH_NOINLINE\n",
    "#endif\n",
    "\n",
    "#if defined(__has_builtin)\n",
    "#if __has_builtin(__builtin_add_overflow) && __has_builtin(__builtin_mul_overflow)\n",
    "#define BH_HAVE_OVERFLOW_BUILTINS 1\n",
    "#endif\n",
    "#elif defined(__GNUC__) && __GNUC__ >= 5\n",
    "#define BH_HAVE_OVERFLOW_BUILTINS 1\n",
    "#endif\n",
    "\n",
    "/* ---------- 值的表示 ---------- */\n",
    "\n",
    "enum { T_UNDEF, T_NIL, T_BOOL, T_INT, T_FLOAT, T_STR, T_LIST, T_FUNC, T_BUILTIN, T_BIG, T_FRAME, T_DICT };\n",
    "\n",
    "typedef struct { int tag; union { int64_t i; double f; void *p; } u; } Value;\n",
    "\n",
    "/* 放在堆上的东西，开头都有这么一段，垃圾回收器靠它把所有东西串起来 */\n",
    "typedef struct Obj { struct Obj *next; size_t size; unsigned char kind, mark; } Obj;\n",
    "enum { K_STR, K_BUF, K_LIST, K_FRAME, K_FUNC, K_BIG, K_DICT };\n",
    "\n",
    "typedef struct { Obj h; int64_t cap, used; uint32_t cp[1]; } Buf;        /* 可以往后接的文字缓冲区 */\n",
    "typedef struct { Obj h; int64_t len; uint32_t *cp; Buf *buf; } Str;      /* 文字：一串 Unicode 编码 */\n",
    "typedef struct { Obj h; int64_t len, cap; Value *items; } List;\n",
    "typedef struct Frame { Obj h; struct Frame *up; int64_t n; Value v[1]; } Frame;   /* 会被内层函数用到的局部变量 */\n",
    "typedef struct { Obj h; Value (*code)(Frame *, Value *); int arity; const char *name; Frame *up; } Func;\n",
    "typedef struct { Obj h; int neg; int64_t n; uint32_t d[1]; } Big;        /* 大整数：绝对值每 32 位一段，低位在前 */\n",
    "/* 字典：条目按放进去的先后排在 e 里（删掉的条目键的 tag 是 T_UNDEF），\n",
    " * slots 是散列表，存条目的位置：-1 空着，-2 原来的条目删掉了 */\n",
    "typedef struct { uint64_t hash; Value key, val; } DictEntry;\n",
    "typedef struct { Obj h; int64_t len, used, cap, nslots; DictEntry *e; int64_t *slots; } Dict;\n",
    "typedef struct { const char *name; Value (*fn)(int, Value *); int min, max; } Builtin;  /* max < 0：个数不限 */\n",
    "typedef struct BhRoots { struct BhRoots *prev; int64_t n; } BhRoots;     /* 后面紧跟着 n 个 Value */\n",
    "\n",
    "static const Value BH_NIL = {T_NIL, {0}}, BH_TRUE = {T_BOOL, {1}}, BH_FALSE = {T_BOOL, {0}};\n",
    "\n",
    "static int bh_line = 0;                          /* 正在执行的白话代码是第几行 */\n",
    "static const char **bh_src = NULL;               /* 源代码的每一行，报错时显示 */\n",
    "static int bh_src_n = 0;\n",
    "static const char **bh_rt_lines = NULL;          /* 运行时自己的源码（只有编译器带着） */\n",
    "static int bh_rt_lines_n = 0;\n",
    "static int bh_argc = 0;\n",
    "static char **bh_argv = NULL;\n",
    "static uintptr_t bh_stack_base = 0;\n",
    "static size_t bh_stack_limit = 768 * 1024;\n",
    "\n",
    "#define BH_TOO_DEEP \"嵌套得太深了。如果是函数自己调用自己，检查一下是不是缺少出口\"\n",
    "#define BH_TOO_BIG \"数字太大了，算不了\"\n",
    "\n",
    "static Value bh_int(int64_t i) { Value v; v.tag = T_INT; v.u.i = i; return v; }\n",
    "static Value bh_float(double f) { Value v; v.tag = T_FLOAT; v.u.f = f; return v; }\n",
    "static Value bh_bool(int b) { return b \? BH_TRUE : BH_FALSE; }\n",
    "static Value bh_obj(int tag, void *p) { Value v; v.tag = tag; v.u.p = p; return v; }\n",
    "\n",
    "/* ---------- 报错 ---------- */\n",
    "\n",
    "/* 正在进行的函数调用，一层套一层地串起来。出错时用它显示「调用过程」。\n",
    " * line 是调用发生的那一行（在调用者里），name 是被调用的函数 */\n",
    "typedef struct BhCall { struct BhCall *prev; const char *name; int line; } BhCall;\n",
    "static BhCall *bh_calls = NULL;\n",
    "\n",
    "/* 调用过程里的一层：在哪个函数里（NULL 是最外层）、执行到第几行 */\n",
    "typedef struct { const char *name; int line; } BhFrameInfo;\n",
    "typedef struct { int64_t first, size, times; } BhStackUnit;    /* 一段：从第 first 层开始，size 层一组，重复 times 遍 */\n",
    "\n",
    "static int bh_frame_same(const BhFrameInfo *a, const BhFrameInfo *b) {\n",
    "    if (a->line != b->line) return 0;\n",
    "    if (!a->name || !b->name) return a->name == b->name;\n",
    "    return strcmp(a->name, b->name) == 0;\n",
    "}\n",
    "\n",
    "static void bh_stack_unit(const BhFrameInfo *f, const BhStackUnit *u) {\n",
    "    int64_t k;\n",
    "    for (k = u->first; k < u->first + u->size; k++) {\n",
    "        if (f[k].name) fprintf(stderr, \"  函数「%s」，第 %d 行\", f[k].name, f[k].line);\n",
    "        else fprintf(stderr, \"  最外层，第 %d 行\", f[k].line);\n",
    "        if (f[k].line >= 1 && f[k].line <= bh_src_n && bh_src[f[k].line - 1][0]) fprintf(stderr, \"：%s\", bh_src[f[k].line - 1]);\n",
    "        fputc('\\n', stderr);\n",
    "    }\n",
    "    if (u->times > 1 && u->size == 1) fprintf(stderr, \"  （上面这一行又重复了 %lld 次）\\n\", (long long)(u->times - 1));\n",
    "    else if (u->times > 1) fprintf(stderr, \"  （上面这 %lld 行又重复了 %lld 次）\\n\", (long long)u->size, (long long)(u->times - 1));\n",
    "}\n",
    "\n",
    "/* 从最外层往里，一层一层列出来。连着重复的几层（递归）并在一起；太多的话只列开头和结尾。\n",
    " * 解释器 baihua.py 的 print_stack 做同样的事，两边的输出一字不差 */\n",
    "static void bh_print_stack(void) {\n",
    "    BhCall *r;\n",
    "    BhFrameInfo *f;\n",
    "    BhStackUnit *units;\n",
    "    int64_t depth = 0, n, nu = 0, i, k, p, reps, omitted;\n",
    "    if (!bh_calls) return;\n",
    "    for (r = bh_calls; r; r = r->prev) depth++;\n",
    "    n = depth + 1;\n",
    "    f = (BhFrameInfo *)malloc((size_t)n * sizeof(BhFrameInfo));\n",
    "    units = (BhStackUnit *)malloc((size_t)n * sizeof(BhStackUnit));\n",
    "    if (!f || !units) return;\n",
    "    f[depth].name = bh_calls->name;\n",
    "    f[depth].line = bh_line;\n",
    "    for (r = bh_calls, k = depth; r; r = r->prev, k--) {\n",
    "        f[k - 1].line = r->line;\n",
    "        f[k - 1].name = r->prev \? r->prev->name : NULL;\n",
    "    }\n",
    "    i = 0;\n",
    "    while (i < n) {\n",
    "        units[nu].first = i;\n",
    "        units[nu].size = 1;\n",
    "        units[nu].times = 1;\n",
    "        for (p = 1; p <= 3; p++) {\n",
    "            reps = 1;\n",
    "            while (i + (reps + 1) * p <= n) {\n",
    "                for (k = 0; k < p; k++)\n",
    "                    if (!bh_frame_same(&f[i + reps * p + k], &f[i + k])) break;\n",
    "                if (k < p) break;\n",
    "                reps++;\n",
    "            }\n",
    "            if (reps >= 2) { units[nu].size = p; units[nu].times = reps; break; }\n",
    "        }\n",
    "        i += units[nu].size * units[nu].times;\n",
    "        nu++;\n",
    "    }\n",
    "    fputs(\"调用过程（从最外层到出错的地方）：\\n\", stderr);\n",
    "    if (nu <= 24) {\n",
    "        for (i = 0; i < nu; i++) bh_stack_unit(f, &units[i]);\n",
    "    } else {\n",
    "        for (i = 0; i < 8; i++) bh_stack_unit(f, &units[i]);\n",
    "        omitted = 0;\n",
    "        for (i = 8; i < nu - 16; i++) omitted += units[i].size * units[i].times;\n",
    "        fprintf(stderr, \"  ……（中间省略了 %lld 层）……\\n\", (long long)omitted);\n",
    "        for (i = nu - 16; i < nu; i++) bh_stack_unit(f, &units[i]);\n",
    "    }\n",
    "    free(f);\n",
    "    free(units);\n",
    "}\n",
    "\n",
    "static void bh_fail(const char *fmt, ...) {\n",
    "    va_list ap;\n",
    "    fflush(stdout);\n",
    "    fputs(\"运行错误\", stderr);\n",
    "    if (bh_line > 0) fprintf(stderr, \"（第 %d 行）\", bh_line);\n",
    "    fputs(\"：\", stderr);\n",
    "    va_start(ap, fmt);\n",
    "    vfprintf(stderr, fmt, ap);\n",
    "    va_end(ap);\n",
    "    fputc('\\n', stderr);\n",
    "    if (bh_line >= 1 && bh_line <= bh_src_n && bh_src[bh_line - 1][0])\n",
    "        fprintf(stderr, \"  >  %s\\n\", bh_src[bh_line - 1]);\n",
    "    bh_print_stack();\n",
    "    exit(1);\n",
    "}\n",
    "\n",
    "static void *bh_xmalloc(size_t n) {\n",
    "    void *p = malloc(n \? n : 1);\n",
    "    if (!p) bh_fail(\"内存不够了\");\n",
    "    return p;\n",
    "}\n",
    "\n",
    "/* ======================================================================\n",
    " * 垃圾回收：标记清除\n",
    " *\n",
    " * 1. 所有放在堆上的东西都串在 bh_all 这条链上。\n",
    " * 2. 回收时从「根」出发：最外层的变量、文字常量、所有正在运行的函数的变量。\n",
    " *    顺着它们能找到的东西都做上标记。\n",
    " * 3. 把链走一遍，没有标记的就是垃圾，释放掉。\n",
    " *\n",
    " * 正在运行的函数的变量怎么找？编译器让每个函数把自己的变量放进一个数组，\n",
    " * 并挂到 bh_roots 这条链上（「影子栈」），回收器照着这条链找。\n",
    " *\n",
    " * 回收只在「安全点」发生：函数刚开始时、循环每转一圈时。运行时内部的 C 代码\n",
    " * 执行到一半时绝不会回收，所以这些代码可以放心地把值拿在手里。\n",
    " * ====================================================================== */\n",
    "\n",
    "static Obj *bh_all = NULL;\n",
    "static size_t bh_since_gc = 0;                           /* 上次回收之后又申请了多少内存 */\n",
    "static size_t bh_gc_threshold = (size_t)8 << 20;         /* 申请超过这么多就该回收了 */\n",
    "static int bh_gc_wanted = 0;\n",
    "static long bh_gc_stress = 0, bh_gc_countdown = 0;       /* 测试用：环境变量 BH_GC_STRESS=N，每 N 个安全点强制回收一次 */\n",
    "static BhRoots *bh_roots = NULL;\n",
    "static Value *bh_root_arrays[2];\n",
    "static int64_t bh_root_counts[2];\n",
    "static Obj **bh_gray = NULL;                             /* 已经标记、但还没检查它里面装了什么的东西 */\n",
    "static size_t bh_gray_n = 0, bh_gray_cap = 0;\n",
    "\n",
    "/* ---------- 小块内存池 ----------\n",
    " * 回收器管的东西（短文字、短列表、函数……）大多很小，申请和释放又特别频繁。\n",
    " * 按 16 字节分档：释放掉的块串起来留着，下次要同样大小的直接拿来用，比每次都找 malloc 快得多。\n",
    " * 用 AddressSanitizer 检查内存时，空闲的块会被标成「不许碰」，用了已经释放的东西照样能查出来。 */\n",
    "#define BH_POOL_MAX 256\n",
    "#define BH_POOL_CHUNK 65536\n",
    "static void *bh_pool[BH_POOL_MAX / 16 + 1];\n",
    "\n",
    "#if defined(__SANITIZE_ADDRESS__)\n",
    "#define BH_ASAN 1\n",
    "#elif defined(__has_feature)\n",
    "#if __has_feature(address_sanitizer)\n",
    "#define BH_ASAN 1\n",
    "#endif\n",
    "#endif\n",
    "#ifdef BH_ASAN\n",
    "void __asan_poison_memory_region(void const volatile *addr, size_t size);\n",
    "void __asan_unpoison_memory_region(void const volatile *addr, size_t size);\n",
    "#define BH_POISON(p, n) __asan_poison_memory_region((p), (n))\n",
    "#define BH_UNPOISON(p, n) __asan_unpoison_memory_region((p), (n))\n",
    "#else\n",
    "#define BH_POISON(p, n) ((void)0)\n",
    "#define BH_UNPOISON(p, n) ((void)0)\n",
    "#endif\n",
    "\n",
    "static void *bh_alloc(size_t n) {\n",
    "    size_t k, size, i;\n",
    "    void *p;\n",
    "    if (n > BH_POOL_MAX) return bh_xmalloc(n);\n",
    "    k = n \? (n + 15) >> 4 : 1;\n",
    "    if (!bh_pool[k]) {                       /* 这一档没有空闲的了：一次要一大块，切成小块 */\n",
    "        char *chunk = (char *)bh_xmalloc(BH_POOL_CHUNK);\n",
    "        size = k << 4;\n",
    "        for (i = 0; i + size <= BH_POOL_CHUNK; i += size) {\n",
    "            *(void **)(chunk + i) = bh_pool[k];\n",
    "            bh_pool[k] = chunk + i;\n",
    "            BH_POISON(chunk + i, size);\n",
    "        }\n",
    "    }\n",
    "    p = bh_pool[k];\n",
    "    BH_UNPOISON(p, k << 4);\n",
    "    bh_pool[k] = *(void **)p;\n",
    "    return p;\n",
    "}\n",
    "\n",
    "static void bh_release(void *p, size_t n) {\n",
    "    size_t k;\n",
    "    if (!p) return;\n",
    "    if (n > BH_POOL_MAX) { free(p); return; }\n",
    "    k = n \? (n + 15) >> 4 : 1;\n",
    "    *(void **)p = bh_pool[k];\n",
    "    bh_pool[k] = p;\n",
    "    BH_POISON(p, k << 4);\n",
    "}\n",
    "\n",
    "static void *bh_new(int kind, size_t size) {\n",
    "    Obj *o = (Obj *)bh_alloc(size);\n",
    "    o->next = bh_all;\n",
    "    bh_all = o;\n",
    "    o->size = size;\n",
    "    o->kind = (unsigned char)kind;\n",
    "    o->mark = 0;\n",
    "    bh_since_gc += size;\n",
    "    if (bh_since_gc > bh_gc_threshold) bh_gc_wanted = 1;\n",
    "    return o;\n",
    "}\n",
    "\n",
    "static void bh_mark_obj(void *p) {\n",
    "    Obj *o = (Obj *)p;\n",
    "    if (!o || o->mark) return;\n",
    "    o->mark = 1;\n",
    "    if (bh_gray_n == bh_gray_cap) {\n",
    "        bh_gray_cap = bh_gray_cap \? bh_gray_cap * 2 : 1024;\n",
    "        bh_gray = (Obj **)realloc(bh_gray, bh_gray_cap * sizeof(Obj *));\n",
    "        if (!bh_gray) bh_fail(\"内存不够了\");\n",
    "    }\n",
    "    bh_gray[bh_gray_n++] = o;\n",
    "}\n",
    "\n",
    "static void bh_mark_value(Value v) {\n",
    "    if (v.tag == T_STR || v.tag == T_LIST || v.tag == T_FUNC || v.tag == T_BIG || v.tag == T_FRAME || v.tag == T_DICT) bh_mark_obj(v.u.p);\n",
    "}\n",
    "\n",
    "static void bh_collect(void) {\n",
    "    Obj **link, *o;\n",
    "    BhRoots *r;\n",
    "    size_t live = 0;\n",
    "    int64_t i;\n",
    "    int k;\n",
    "    if (bh_gc_stress > 1 && bh_since_gc <= bh_gc_threshold) {\n",
    "        if (--bh_gc_countdown > 0) return;\n",
    "        bh_gc_countdown = bh_gc_stress;\n",
    "    }\n",
    "    /* 标记：从根出发 */\n",
    "    for (k = 0; k < 2; k++)\n",
    "        for (i = 0; i < bh_root_counts[k]; i++) bh_mark_value(bh_root_arrays[k][i]);\n",
    "    for (r = bh_roots; r; r = r->prev) {\n",
    "        Value *v = (Value *)(r + 1);\n",
    "        for (i = 0; i < r->n; i++) bh_mark_value(v[i]);\n",
    "    }\n",
    "    while (bh_gray_n) {\n",
    "        o = bh_gray[--bh_gray_n];\n",
    "        if (o->kind == K_STR) {\n",
    "            bh_mark_obj(((Str *)o)->buf);\n",
    "        } else if (o->kind == K_LIST) {\n",
    "            List *l = (List *)o;\n",
    "            for (i = 0; i < l->len; i++) bh_mark_value(l->items[i]);\n",
    "        } else if (o->kind == K_FRAME) {\n",
    "            Frame *f = (Frame *)o;\n",
    "            bh_mark_obj(f->up);\n",
    "            for (i = 0; i < f->n; i++) bh_mark_value(f->v[i]);\n",
    "        } else if (o->kind == K_FUNC) {\n",
    "            bh_mark_obj(((Func *)o)->up);\n",
    "        } else if (o->kind == K_DICT) {\n",
    "            Dict *d = (Dict *)o;\n",
    "            for (i = 0; i < d->used; i++)\n",
    "                if (d->e[i].key.tag != T_UNDEF) { bh_mark_value(d->e[i].key); bh_mark_value(d->e[i].val); }\n",
    "        }\n",
    "    }\n",
    "    /* 清除：没被标记的都释放 */\n",
    "    link = &bh_all;\n",
    "    while ((o = *link) != NULL) {\n",
    "        if (o->mark) {\n",
    "            o->mark = 0;\n",
    "            live += o->size;\n",
    "            if (o->kind == K_LIST) live += (size_t)((List *)o)->cap * sizeof(Value);\n",
    "            if (o->kind == K_DICT) live += (size_t)((Dict *)o)->cap * sizeof(DictEntry) + (size_t)((Dict *)o)->nslots * sizeof(int64_t);\n",
    "            link = &o->next;\n",
    "        } else {\n",
    "            *link = o->next;\n",
    "            if (o->kind == K_LIST) bh_release(((List *)o)->items, (size_t)((List *)o)->cap * sizeof(Value));\n",
    "            if (o->kind == K_DICT) { free(((Dict *)o)->e); free(((Dict *)o)->slots); }\n",
    "            bh_release(o, o->size);\n",
    "        }\n",
    "    }\n",
    "    bh_since_gc = 0;\n",
    "    bh_gc_threshold = live > ((size_t)8 << 20) \? live : (size_t)8 << 20;   /* 活着的东西越多，隔得越久再回收 */\n",
    "    bh_gc_wanted = bh_gc_stress != 0;\n",
    "}\n",
    "\n",
    "/* 下面三个是给编译器生成的代码用的 */\n",
    "#define BH_SAFEPOINT() do { if (bh_gc_wanted) bh_collect(); } while (0)\n",
    "#define BH_ENTER(fr, count) do { memset(&(fr), 0, sizeof(fr)); (fr).h.n = (count); (fr).h.prev = bh_roots; bh_roots = &(fr).h; } while (0)\n",
    "#define BH_RETURN(fr, x) do { Value bh_result = (x); bh_roots = (fr).h.prev; return bh_result; } while (0)\n",
    "\n",
    "/* ---------- 文字 ---------- */\n",
    "\n",
    "static Str *bh_str_alloc(int64_t len) {\n",
    "    Str *s = (Str *)bh_new(K_STR, sizeof(Str) + (size_t)(len > 0 \? len : 1) * sizeof(uint32_t));\n",
    "    s->len = len;\n",
    "    s->cp = (uint32_t *)(s + 1);\n",
    "    s->buf = NULL;\n",
    "    return s;\n",
    "}\n",
    "\n",
    "/* UTF-8 字节 -> 文字。不合法的字节换成 U+FFFD */\n",
    "static Str *bh_decode(const char *bytes, size_t n) {\n",
    "    const unsigned char *s = (const unsigned char *)bytes;\n",
    "    Str *out = bh_str_alloc((int64_t)n);\n",
    "    size_t i = 0;\n",
    "    int64_t k = 0;\n",
    "    while (i < n) {\n",
    "        uint32_t c = s[i];\n",
    "        int extra = 0, j, ok;\n",
    "        if (c < 0x80) extra = 0;\n",
    "        else if (c >= 0xC2 && c <= 0xDF) { extra = 1; c &= 0x1F; }\n",
    "        else if (c >= 0xE0 && c <= 0xEF) { extra = 2; c &= 0x0F; }\n",
    "        else if (c >= 0xF0 && c <= 0xF4) { extra = 3; c &= 0x07; }\n",
    "        else { out->cp[k++] = 0xFFFD; i++; continue; }\n",
    "        ok = i + (size_t)extra < n;\n",
    "        for (j = 1; ok && j <= extra; j++) {\n",
    "            if ((s[i + j] & 0xC0) != 0x80) ok = 0;\n",
    "            else c = (c << 6) | (s[i + j] & 0x3F);\n",
    "        }\n",
    "        if (!ok || (extra == 2 && c < 0x800) || (extra == 3 && (c < 0x10000 || c > 0x10FFFF))\n",
    "                || (c >= 0xD800 && c <= 0xDFFF)) {\n",
    "            out->cp[k++] = 0xFFFD;\n",
    "            i++;\n",
    "            continue;\n",
    "        }\n",
    "        out->cp[k++] = c;\n",
    "        i += (size_t)extra + 1;\n",
    "    }\n",
    "    out->len = k;\n",
    "    return out;\n",
    "}\n",
    "\n",
    "static size_t bh_encode1(uint32_t c, char *o) {\n",
    "    if (c < 0x80) { o[0] = (char)c; return 1; }\n",
    "    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }\n",
    "    if (c < 0x10000) {\n",
    "        o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3F));\n",
    "        o[2] = (char)(0x80 | (c & 0x3F));\n",
    "        return 3;\n",
    "    }\n",
    "    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 0x3F));\n",
    "    o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F));\n",
    "    return 4;\n",
    "}\n",
    "\n",
    "/* 文字 -> 以 0 结尾的 UTF-8，给 C 的库函数用。用完要 free */\n",
    "static char *bh_cstr(const Str *s) {\n",
    "    char *o = (char *)bh_xmalloc((size_t)s->len * 4 + 1);\n",
    "    size_t k = 0;\n",
    "    int64_t i;\n",
    "    for (i = 0; i < s->len; i++) k += bh_encode1(s->cp[i], o + k);\n",
    "    o[k] = 0;\n",
    "    return o;\n",
    "}\n",
    "\n",
    "static void bh_fwrite(FILE *f, const uint32_t *cp, int64_t len) {\n",
    "    char buf[4096];\n",
    "    size_t k = 0;\n",
    "    int64_t i;\n",
    "    for (i = 0; i < len; i++) {\n",
    "        if (k > sizeof buf - 8) { fwrite(buf, 1, k, f); k = 0; }\n",
    "        k += bh_encode1(cp[i], buf + k);\n",
    "    }\n",
    "    fwrite(buf, 1, k, f);\n",
    "}\n",
    "\n",
    "static Value bh_str_lit(const char *z) { return bh_obj(T_STR, bh_decode(z, strlen(z))); }\n",
    "\n",
    "/* 单个字符的文字。英文字符很常用，各存一份永远不回收的 */\n",
    "static Value bh_char(uint32_t c) {\n",
    "    static Str *cache[128];\n",
    "    Str *s;\n",
    "    if (c >= 128) {\n",
    "        s = bh_str_alloc(1);\n",
    "        s->cp[0] = c;\n",
    "        return bh_obj(T_STR, s);\n",
    "    }\n",
    "    if (!cache[c]) {\n",
    "        s = (Str *)bh_xmalloc(sizeof(Str) + sizeof(uint32_t));\n",
    "        memset(s, 0, sizeof(Str));\n",
    "        s->h.mark = 1;                           /* 不在 bh_all 链上，标记永远是 1，回收器不会碰它 */\n",
    "        s->len = 1;\n",
    "        s->cp = (uint32_t *)(s + 1);\n",
    "        s->cp[0] = c;\n",
    "        cache[c] = s;\n",
    "    }\n",
    "    return bh_obj(T_STR, cache[c]);\n",
    "}\n",
    "\n",
    "static int bh_strcmp(const Str *a, const Str *b) {\n",
    "    int64_t i, n = a->len < b->len \? a->len : b->len;\n",
    "    for (i = 0; i < n; i++)\n",
    "        if (a->cp[i] != b->cp[i]) return a->cp[i] < b->cp[i] \? -1 : 1;\n",
    "    return (a->len > b->len) - (a->len < b->len);\n",
    "}\n",
    "\n",
    "/* 一点一点拼出一段文字 */\n",
    "typedef struct { uint32_t *cp; int64_t len, cap; } SB;\n",
    "\n",
    "static void sb_reserve(SB *b, int64_t more) {\n",
    "    if (b->len + more > b->cap) {\n",
    "        int64_t cap = b->cap \? b->cap * 2 : 32;\n",
    "        while (cap < b->len + more) cap *= 2;\n",
    "        b->cp = (uint32_t *)realloc(b->cp, (size_t)cap * sizeof(uint32_t));\n",
    "        if (!b->cp) bh_fail(\"内存不够了\");\n",
    "        b->cap = cap;\n",
    "    }\n",
    "}\n",
    "static void sb_put(SB *b, uint32_t c) { sb_reserve(b, 1); b->cp[b->len++] = c; }\n",
    "static void sb_cp(SB *b, const uint32_t *cp, int64_t len) {\n",
    "    if (!len) return;\n",
    "    sb_reserve(b, len);\n",
    "    memcpy(b->cp + b->len, cp, (size_t)len * sizeof(uint32_t));\n",
    "    b->len += len;\n",
    "}\n",
    "static void sb_str(SB *b, const Str *s) { sb_cp(b, s->cp, s->len); }\n",
    "static void sb_ascii(SB *b, const char *z) { while (*z) sb_put(b, (unsigned char)*z++); }\n",
    "static void sb_utf8(SB *b, const char *z) { sb_str(b, bh_decode(z, strlen(z))); }\n",
    "static Value sb_done(SB *b) {\n",
    "    Str *s = bh_str_alloc(b->len);\n",
    "    if (b->len) memcpy(s->cp, b->cp, (size_t)b->len * sizeof(uint32_t));\n",
    "    free(b->cp);\n",
    "    return bh_obj(T_STR, s);\n",
    "}\n",
    "\n",
    "/* 把两段文字接起来。\n",
    " * 循环里常见的写法是 甲 = 甲 + 乙，如果每次都把整段重抄一遍，接一万次就要抄五千万个字。\n",
    " * 所以第一次接的时候多申请一倍的空间，之后只要前一段正好是这块空间里最长的那段文字，\n",
    " * 就直接把后一段写在它后面。前一段文字自己的长度不变，所以它的内容没有被改动。 */\n",
    "static Value bh_str_concat(const Str *a, const Str *b) {\n",
    "    int64_t total = a->len + b->len;\n",
    "    Str *r = (Str *)bh_new(K_STR, sizeof(Str));\n",
    "    r->len = total;\n",
    "    if (a->buf && a->cp == a->buf->cp && a->buf->used == a->len && a->buf->cap - a->len >= b->len) {\n",
    "        if (b->len) memcpy(a->buf->cp + a->len, b->cp, (size_t)b->len * sizeof(uint32_t));\n",
    "        r->buf = a->buf;\n",
    "    } else {\n",
    "        int64_t cap = total < 16 \? 32 : total * 2;\n",
    "        Buf *nb = (Buf *)bh_new(K_BUF, sizeof(Buf) + (size_t)cap * sizeof(uint32_t));\n",
    "        nb->cap = cap;\n",
    "        if (a->len) memcpy(nb->cp, a->cp, (size_t)a->len * sizeof(uint32_t));\n",
    "        if (b->len) memcpy(nb->cp + a->len, b->cp, (size_t)b->len * sizeof(uint32_t));\n",
    "        r->buf = nb;\n",
    "    }\n",
    "    r->buf->used = total;\n",
    "    r->cp = r->buf->cp;\n",
    "    return bh_obj(T_STR, r);\n",
    "}\n",
    "\n",
    "/* ---------- 列表 ---------- */\n",
    "\n",
    "static List *bh_list_alloc(int64_t cap) {\n",
    "    List *l = (List *)bh_new(K_LIST, sizeof(List));\n",
    "    l->len = 0;\n",
    "    l->cap = cap > 4 \? cap : 4;\n",
    "    l->items = (Value *)bh_alloc((size_t)l->cap * sizeof(Value));\n",
    "    bh_since_gc += (size_t)l->cap * sizeof(Value);\n",
    "    return l;\n",
    "}\n",
    "static void bh_push(List *l, Value v) {\n",
    "    if (l->len == l->cap) {\n",
    "        Value *items = (Value *)bh_alloc((size_t)l->cap * 2 * sizeof(Value));\n",
    "        bh_since_gc += (size_t)l->cap * sizeof(Value);\n",
    "        memcpy(items, l->items, (size_t)l->len * sizeof(Value));\n",
    "        bh_release(l->items, (size_t)l->cap * sizeof(Value));\n",
    "        l->items = items;\n",
    "        l->cap *= 2;\n",
    "    }\n",
    "    l->items[l->len++] = v;\n",
    "}\n",
    "static Value bh_list_make(int64_t n, const Value *items) {\n",
    "    List *l = bh_list_alloc(n);\n",
    "    if (n) memcpy(l->items, items, (size_t)n * sizeof(Value));\n",
    "    l->len = n;\n",
    "    return bh_obj(T_LIST, l);\n",
    "}\n",
    "static Value bh_list_at(Value list, int64_t i) { return ((List *)list.u.p)->items[i]; }\n",
    "static int64_t bh_list_len(Value list) { return ((List *)list.u.p)->len; }\n",
    "\n",
    "/* ======================================================================\n",
    " * 大整数\n",
    " *\n",
    " * 平时整数就是一个 64 位的数。只有装不下的时候才换成 Big：把绝对值切成\n",
    " * 每 32 位一段，放进数组，低位在前；正负号单独记。加减乘除就是小学竖式，\n",
    " * 只不过每一「位」是 0 到 42 亿多，而不是 0 到 9。\n",
    " * 算完之后如果又能装进 64 位，就换回普通整数，所以同一个数只有一种表示。\n",
    " * ====================================================================== */\n",
    "\n",
    "/* 参加运算的整数，不管原来是普通整数还是大整数，都先看成「正负号 + 若干段」 */\n",
    "typedef struct { int neg; int64_t n; const uint32_t *d; uint32_t small[2]; } BigRef;\n",
    "\n",
    "static void big_ref(BigRef *r, Value v) {\n",
    "    if (v.tag == T_BIG) {\n",
    "        Big *b = (Big *)v.u.p;\n",
    "        r->neg = b->neg; r->n = b->n; r->d = b->d;\n",
    "    } else {\n",
    "        uint64_t m = v.u.i < 0 \? (uint64_t)0 - (uint64_t)v.u.i : (uint64_t)v.u.i;\n",
    "        r->neg = v.u.i < 0;\n",
    "        r->small[0] = (uint32_t)m;\n",
    "        r->small[1] = (uint32_t)(m >> 32);\n",
    "        r->n = r->small[1] \? 2 : r->small[0] \? 1 : 0;\n",
    "        r->d = r->small;\n",
    "    }\n",
    "}\n",
    "\n",
    "/* 把算出来的结果变成值：去掉高位的 0，能装进 64 位就用普通整数 */\n",
    "static Value big_make(int neg, const uint32_t *d, int64_t n) {\n",
    "    Big *b;\n",
    "    while (n > 0 && d[n - 1] == 0) n--;\n",
    "    if (n <= 2) {\n",
    "        uint64_t m = n == 0 \? 0 : n == 1 \? d[0] : ((uint64_t)d[1] << 32) | d[0];\n",
    "        if (!neg && m <= (uint64_t)INT64_MAX) return bh_int((int64_t)m);\n",
    "        if (neg && m <= (uint64_t)INT64_MAX + 1) return bh_int(m == (uint64_t)INT64_MAX + 1 \? INT64_MIN : -(int64_t)m);\n",
    "    }\n",
    "    b = (Big *)bh_new(K_BIG, sizeof(Big) + (size_t)n * sizeof(uint32_t));\n",
    "    b->neg = neg;\n",
    "    b->n = n;\n",
    "    memcpy(b->d, d, (size_t)n * sizeof(uint32_t));\n",
    "    return bh_obj(T_BIG, b);\n",
    "}\n",
    "\n",
    "static int mag_cmp(const uint32_t *a, int64_t na, const uint32_t *b, int64_t nb) {\n",
    "    int64_t i;\n",
    "    if (na != nb) return na > nb \? 1 : -1;\n",
    "    for (i = na - 1; i >= 0; i--)\n",
    "        if (a[i] != b[i]) return a[i] > b[i] \? 1 : -1;\n",
    "    return 0;\n",
    "}\n",
    "\n",
    "static int64_t mag_add(uint32_t *r, const uint32_t *a, int64_t na, const uint32_t *b, int64_t nb) {\n",
    "    uint64_t carry = 0;\n",
    "    int64_t i, n = na > nb \? na : nb;\n",
    "    for (i = 0; i < n; i++) {\n",
    "        carry += (uint64_t)(i < na \? a[i] : 0) + (i < nb \? b[i] : 0);\n",
    "        r[i] = (uint32_t)carry;\n",
    "        carry >>= 32;\n",
    "    }\n",
    "    r[n] = (uint32_t)carry;\n",
    "    return n + 1;\n",
    "}\n",
    "\n",
    "static int64_t mag_sub(uint32_t *r, const uint32_t *a, int64_t na, const uint32_t *b, int64_t nb) {   /* 要求 a >= b */\n",
    "    int64_t borrow = 0, i;\n",
    "    for (i = 0; i < na; i++) {\n",
    "        int64_t t = (int64_t)a[i] - (i < nb \? (int64_t)b[i] : 0) - borrow;\n",
    "        borrow = t < 0;\n",
    "        r[i] = (uint32_t)t;\n",
    "    }\n",
    "    return na;\n",
    "}\n",
    "\n",
    "static uint32_t mag_divmod1(uint32_t *q, const uint32_t *a, int64_t na, uint32_t b) {   /* 除数只有一段 */\n",
    "    uint64_t rem = 0;\n",
    "    int64_t i;\n",
    "    for (i = na - 1; i >= 0; i--) {\n",
    "        uint64_t cur = (rem << 32) | a[i];\n",
    "        q[i] = (uint32_t)(cur / b);\n",
    "        rem = cur % b;\n",
    "    }\n",
    "    return (uint32_t)rem;\n",
    "}\n",
    "\n",
    "/* 多段除以多段（nb >= 2，na >= nb）。经典的「算法 D」：先估一位商，再乘回去减，估大了就加回来 */\n",
    "static void mag_divmod(uint32_t *q, uint32_t *r, const uint32_t *u, int64_t na, const uint32_t *v, int64_t nb) {\n",
    "    uint32_t *un = (uint32_t *)bh_xmalloc((size_t)(na + 1) * sizeof(uint32_t));\n",
    "    uint32_t *vn = (uint32_t *)bh_xmalloc((size_t)nb * sizeof(uint32_t));\n",
    "    int64_t i, j;\n",
    "    int s = 0;\n",
    "    while (!((v[nb - 1] << s) & 0x80000000u)) s++;          /* 先把除数往左挪，让最高位是 1 */\n",
    "    for (i = nb - 1; i > 0; i--) vn[i] = (v[i] << s) | (s \? v[i - 1] >> (32 - s) : 0);\n",
    "    vn[0] = v[0] << s;\n",
    "    un[na] = s \? u[na - 1] >> (32 - s) : 0;\n",
    "    for (i = na - 1; i > 0; i--) un[i] = (u[i] << s) | (s \? u[i - 1] >> (32 - s) : 0);\n",
    "    un[0] = u[0] << s;\n",
    "    for (j = na - nb; j >= 0; j--) {\n",
    "        uint64_t num = ((uint64_t)un[j + nb] << 32) | un[j + nb - 1];\n",
    "        uint64_t qhat = num / vn[nb - 1], rhat = num % vn[nb - 1], p;\n",
    "        int64_t k = 0, t;\n",
    "        while (qhat >= 0x100000000ULL || qhat * vn[nb - 2] > ((rhat << 32) | un[j + nb - 2])) {\n",
    "            qhat--;\n",
    "            rhat += vn[nb - 1];\n",
    "            if (rhat >= 0x100000000ULL) break;\n",
    "        }\n",
    "        for (i = 0; i < nb; i++) {\n",
    "            p = qhat * vn[i];\n",
    "            t = (int64_t)un[i + j] - k - (int64_t)(p & 0xFFFFFFFFu);\n",
    "            un[i + j] = (uint32_t)t;\n",
    "            k = (int64_t)(p >> 32) - (t >> 32);\n",
    "        }\n",
    "        t = (int64_t)un[j + nb] - k;\n",
    "        un[j + nb] = (uint32_t)t;\n",
    "        q[j] = (uint32_t)qhat;\n",
    "        if (t < 0) {                                          /* 估大了一：加回来 */\n",
    "            uint64_t carry = 0;\n",
    "            q[j]--;\n",
    "            for (i = 0; i < nb; i++) {\n",
    "                carry += (uint64_t)un[i + j] + vn[i];\n",
    "                un[i + j] = (uint32_t)carry;\n",
    "                carry >>= 32;\n",
    "            }\n",
    "            un[j + nb] += (uint32_t)carry;\n",
    "        }\n",
    "    }\n",
    "    for (i = 0; i < nb; i++) r[i] = (un[i] >> s) | (s \? un[i + 1] << (32 - s) : 0);\n",
    "    free(un);\n",
    "    free(vn);\n",
    "}\n",
    "\n",
    "/* 取最高的三段，算出近似的小数值。lo 是被略去的低位段数 */\n",
    "static double mag_top(const uint32_t *d, int64_t n, int64_t *lo) {\n",
    "    double x = 0;\n",
    "    int64_t i;\n",
    "    *lo = n > 3 \? n - 3 : 0;\n",
    "    for (i = n - 1; i >= *lo; i--) x = x * 4294967296.0 + (double)d[i];\n",
    "    return x;\n",
    "}\n",
    "\n",
    "static double mag_to_double(const uint32_t *d, int64_t n) {       /* 太大就得到无穷大 */\n",
    "    int64_t lo;\n",
    "    double x = mag_top(d, n, &lo);\n",
    "    return ldexp(x, lo > 40 \? 1280 : (int)(lo * 32));\n",
    "}\n",
    "\n",
    "static double mag_ratio(const uint32_t *a, int64_t na, const uint32_t *b, int64_t nb) {   /* a / b，要求 a < b */\n",
    "    int64_t la, lb;\n",
    "    double x = mag_top(a, na, &la), y = mag_top(b, nb, &lb);\n",
    "    return lb - la > 40 \? 0.0 : ldexp(x / y, (int)((la - lb) * 32));\n",
    "}\n",
    "\n",
    "static int big_cmp(const BigRef *x, const BigRef *y) {\n",
    "    int c;\n",
    "    if (x->n == 0 && y->n == 0) return 0;\n",
    "    if (x->neg != y->neg) return x->neg \? -1 : 1;\n",
    "    c = mag_cmp(x->d, x->n, y->d, y->n);\n",
    "    return x->neg \? -c : c;\n",
    "}\n",
    "\n",
    "static Value big_addsub(const BigRef *x, const BigRef *y, int subtract) {\n",
    "    int yneg = y->neg ^ subtract, c;\n",
    "    uint32_t *r = (uint32_t *)bh_xmalloc((size_t)((x->n > y->n \? x->n : y->n) + 1) * sizeof(uint32_t));\n",
    "    Value v;\n",
    "    if (x->neg == yneg) {\n",
    "        v = big_make(x->neg, r, mag_add(r, x->d, x->n, y->d, y->n));\n",
    "    } else {\n",
    "        c = mag_cmp(x->d, x->n, y->d, y->n);\n",
    "        if (c >= 0) v = big_make(x->neg, r, mag_sub(r, x->d, x->n, y->d, y->n));\n",
    "        else v = big_make(yneg, r, mag_sub(r, y->d, y->n, x->d, x->n));\n",
    "    }\n",
    "    free(r);\n",
    "    return v;\n",
    "}\n",
    "\n",
    "static Value big_mul(const BigRef *x, const BigRef *y) {\n",
    "    uint32_t *r = (uint32_t *)bh_xmalloc((size_t)(x->n + y->n + 1) * sizeof(uint32_t));\n",
    "    int64_t i, j;\n",
    "    Value v;\n",
    "    memset(r, 0, (size_t)(x->n + y->n + 1) * sizeof(uint32_t));\n",
    "    for (i = 0; i < x->n; i++) {\n",
    "        uint64_t carry = 0;\n",
    "        for (j = 0; j < y->n; j++) {\n",
    "            carry += (uint64_t)x->d[i] * y->d[j] + r[i + j];\n",
    "            r[i + j] = (uint32_t)carry;\n",
    "            carry >>= 32;\n",
    "        }\n",
    "        r[i + y->n] = (uint32_t)carry;\n",
    "    }\n",
    "    v = big_make(x->neg ^ y->neg, r, x->n + y->n);\n",
    "    free(r);\n",
    "    return v;\n",
    "}\n",
    "\n",
    "/* 除法和取余数。op 是 '/' 或 '%'。规则和普通整数一样：\n",
    " * 能整除就得到整数，否则得到小数；余数的正负跟着除数走 */\n",
    "static Value big_divmod(int op, const BigRef *x, const BigRef *y) {\n",
    "    uint32_t *q, *r;\n",
    "    int64_t qn, rn;\n",
    "    Value v;\n",
    "    if (y->n == 0) bh_fail(\"除数不能是 0\");\n",
    "    q = (uint32_t *)bh_xmalloc((size_t)(x->n + 1) * sizeof(uint32_t));\n",
    "    r = (uint32_t *)bh_xmalloc((size_t)(y->n + x->n + 1) * sizeof(uint32_t));\n",
    "    if (mag_cmp(x->d, x->n, y->d, y->n) < 0) {\n",
    "        qn = 0;\n",
    "        rn = x->n;\n",
    "        if (rn) memcpy(r, x->d, (size_t)rn * sizeof(uint32_t));\n",
    "    } else if (y->n == 1) {\n",
    "        r[0] = mag_divmod1(q, x->d, x->n, y->d[0]);\n",
    "        qn = x->n;\n",
    "        rn = 1;\n",
    "    } else {\n",
    "        mag_divmod(q, r, x->d, x->n, y->d, y->n);\n",
    "        qn = x->n - y->n + 1;\n",
    "        rn = y->n;\n",
    "    }\n",
    "    while (qn > 0 && q[qn - 1] == 0) qn--;\n",
    "    while (rn > 0 && r[rn - 1] == 0) rn--;\n",
    "    if (op == '/') {\n",
    "        if (rn == 0) {\n",
    "            v = big_make(x->neg ^ y->neg, q, qn);\n",
    "        } else {\n",
    "            double result = mag_to_double(q, qn) + mag_ratio(r, rn, y->d, y->n);\n",
    "            if (isinf(result)) bh_fail(BH_TOO_BIG);\n",
    "            v = bh_float(x->neg ^ y->neg \? -result : result);\n",
    "        }\n",
    "    } else if (rn == 0) {\n",
    "        v = bh_int(0);\n",
    "    } else if (x->neg == y->neg) {\n",
    "        v = big_make(x->neg, r, rn);\n",
    "    } else {\n",
    "        uint32_t *t = (uint32_t *)bh_xmalloc((size_t)y->n * sizeof(uint32_t));\n",
    "        v = big_make(y->neg, t, mag_sub(t, y->d, y->n, r, rn));\n",
    "        free(t);\n",
    "    }\n",
    "    free(q);\n",
    "    free(r);\n",
    "    return v;\n",
    "}\n",
    "\n",
    "/* 把一个没有小数部分的小数，原原本本地变成「正负号 + 若干段」。buf 要有 36 段 */\n",
    "static void big_from_whole(BigRef *r, uint32_t *buf, double whole) {\n",
    "    int e, shift;\n",
    "    double m = frexp(fabs(whole), &e);\n",
    "    uint64_t mant;\n",
    "    int64_t n = 36;\n",
    "    memset(buf, 0, 36 * sizeof(uint32_t));\n",
    "    r->neg = whole < 0;\n",
    "    r->d = buf;\n",
    "    r->n = 0;\n",
    "    if (whole == 0) return;\n",
    "    mant = (uint64_t)ldexp(m, 53);              /* 小数的 53 位有效数字 */\n",
    "    shift = e - 53;                             /* whole = mant * 2^shift */\n",
    "    if (shift <= 0) {\n",
    "        mant >>= -shift;\n",
    "        buf[0] = (uint32_t)mant;\n",
    "        buf[1] = (uint32_t)(mant >> 32);\n",
    "    } else {\n",
    "        int limb = shift / 32, bit = shift % 32;\n",
    "        buf[limb] = (uint32_t)(mant << bit);\n",
    "        buf[limb + 1] = (uint32_t)(bit \? mant >> (32 - bit) : mant >> 32);\n",
    "        buf[limb + 2] = bit \? (uint32_t)(mant >> (64 - bit)) : 0;\n",
    "    }\n",
    "    while (n > 0 && buf[n - 1] == 0) n--;\n",
    "    r->n = n;\n",
    "}\n",
    "\n",
    "static Value bh_from_double(double d) {         /* 小数 -> 整数，去掉小数部分。d 不能是无穷大 */\n",
    "    BigRef r;\n",
    "    uint32_t buf[36];\n",
    "    big_from_whole(&r, buf, trunc(d));\n",
    "    return big_make(r.neg, buf, r.n);\n",
    "}\n",
    "\n",
    "/* 整数和小数比大小，不丢精度。返回 -1、0、1。d 不能是无穷大 */\n",
    "static int bh_cmp_int_double(Value iv, double d) {\n",
    "    BigRef x, y;\n",
    "    uint32_t buf[36];\n",
    "    double whole = floor(d);\n",
    "    int c;\n",
    "    big_ref(&x, iv);\n",
    "    big_from_whole(&y, buf, whole);\n",
    "    c = big_cmp(&x, &y);\n",
    "    if (c) return c;\n",
    "    return d > whole \? -1 : 0;\n",
    "}\n",
    "\n",
    "static void big_show(SB *b, const Big *x) {\n",
    "    int64_t n = x->n, k = 0, i;\n",
    "    uint32_t *tmp = (uint32_t *)bh_xmalloc((size_t)n * sizeof(uint32_t));\n",
    "    uint32_t *chunks = (uint32_t *)bh_xmalloc((size_t)(n * 32 / 29 + 2) * sizeof(uint32_t));\n",
    "    char buf[16];\n",
    "    memcpy(tmp, x->d, (size_t)n * sizeof(uint32_t));\n",
    "    while (n > 0) {                             /* 每次除以十亿，余数就是最低的九位十进制数字 */\n",
    "        chunks[k++] = mag_divmod1(tmp, tmp, n, 1000000000u);\n",
    "        while (n > 0 && tmp[n - 1] == 0) n--;\n",
    "    }\n",
    "    if (x->neg) sb_put(b, '-');\n",
    "    for (i = k - 1; i >= 0; i--) {\n",
    "        snprintf(buf, sizeof buf, i == k - 1 \? \"%u\" : \"%09u\", (unsigned)chunks[i]);\n",
    "        sb_ascii(b, buf);\n",
    "    }\n",
    "    free(tmp);\n",
    "    free(chunks);\n",
    "}\n",
    "\n",
    "/* 十进制数字（可以带正负号）-> 整数，多长都行 */\n",
    "static Value bh_big_lit(const char *z) {\n",
    "    int neg = 0;\n",
    "    uint32_t *d;\n",
    "    int64_t n = 0, i;\n",
    "    Value v;\n",
    "    if (*z == '+' || *z == '-') { neg = *z == '-'; z++; }\n",
    "    d = (uint32_t *)bh_xmalloc((strlen(z) / 9 + 2) * sizeof(uint32_t));\n",
    "    for (; *z; z++) {\n",
    "        uint64_t carry = (uint64_t)(*z - '0');\n",
    "        for (i = 0; i < n; i++) {\n",
    "            carry += (uint64_t)d[i] * 10;\n",
    "            d[i] = (uint32_t)carry;\n",
    "            carry >>= 32;\n",
    "        }\n",
    "        if (carry) d[n++] = (uint32_t)carry;\n",
    "    }\n",
    "    v = big_make(neg, d, n);\n",
    "    free(d);\n",
    "    return v;\n",
    "}\n",
    "\n",
    "static Value bh_big_arith(int op, Value a, Value b) {\n",
    "    BigRef x, y;\n",
    "    big_ref(&x, a);\n",
    "    big_ref(&y, b);\n",
    "    if (op == '+') return big_addsub(&x, &y, 0);\n",
    "    if (op == '-') return big_addsub(&x, &y, 1);\n",
    "    if (op == '*') return big_mul(&x, &y);\n",
    "    return big_divmod(op, &x, &y);\n",
    "}\n",
    "\n",
    "/* ---------- 把值变成给人看的文字 ---------- */\n",
    "\n",
    "static const char *bh_tn(Value v) {\n",
    "    switch (v.tag) {\n",
    "    case T_NIL: return \"空\";\n",
    "    case T_BOOL: return \"真假\";\n",
    "    case T_INT: case T_BIG: return \"整数\";\n",
    "    case T_FLOAT: return \"小数\";\n",
    "    case T_STR: return \"文字\";\n",
    "    case T_LIST: return \"列表\";\n",
    "    case T_DICT: return \"字典\";\n",
    "    default: return \"函数\";\n",
    "    }\n",
    "}\n",
    "\n",
    "/* 整数写成十进制，返回写了几个字符。比 printf 快得多 */\n",
    "static int bh_itoa(int64_t v, char *out) {\n",
    "    char tmp[24];\n",
    "    int n = 0, k = 0;\n",
    "    uint64_t u = v < 0 \? (uint64_t)0 - (uint64_t)v : (uint64_t)v;\n",
    "    do { tmp[n++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);\n",
    "    if (v < 0) out[k++] = '-';\n",
    "    while (n) out[k++] = tmp[--n];\n",
    "    out[k] = 0;\n",
    "    return k;\n",
    "}\n",
    "\n",
    "/* 一串英文字符做成文字。只有一个字符时用现成的那份 */\n",
    "static Value bh_ascii_str(const char *z, size_t n) {\n",
    "    Str *s;\n",
    "    size_t i;\n",
    "    if (n == 1) return bh_char((unsigned char)z[0]);\n",
    "    s = bh_str_alloc((int64_t)n);\n",
    "    for (i = 0; i < n; i++) s->cp[i] = (unsigned char)z[i];\n",
    "    return bh_obj(T_STR, s);\n",
    "}\n",
    "\n",
    "static void bh_show_into(SB *b, Value v, int quote, int depth) {\n",
    "    char buf[64];\n",
    "    switch (v.tag) {\n",
    "    case T_NIL: sb_put(b, 0x7A7A); return;                       /* 空 */\n",
    "    case T_BOOL: sb_put(b, v.u.i \? 0x771F : 0x5047); return;     /* 真 假 */\n",
    "    case T_INT: bh_itoa(v.u.i, buf); sb_ascii(b, buf); return;\n",
    "    case T_BIG: big_show(b, (Big *)v.u.p); return;\n",
    "    case T_FLOAT: snprintf(buf, sizeof buf, \"%.12g\", v.u.f); sb_ascii(b, buf); return;\n",
    "    case T_STR:\n",
    "        if (quote) sb_put(b, 0x201C);\n",
    "        sb_str(b, (Str *)v.u.p);\n",
    "        if (quote) sb_put(b, 0x201D);\n",
    "        return;\n",
    "    case T_LIST: {\n",
    "        List *l = (List *)v.u.p;\n",
    "        int64_t i;\n",
    "        if (depth > 500) bh_fail(BH_TOO_DEEP);\n",
    "        sb_put(b, 0x3010);                                        /* 【 */\n",
    "        for (i = 0; i < l->len; i++) {\n",
    "            if (i) sb_put(b, 0xFF0C);                             /* ， */\n",
    "            bh_show_into(b, l->items[i], 1, depth + 1);\n",
    "        }\n",
    "        sb_put(b, 0x3011);                                        /* 】 */\n",
    "        return;\n",
    "    }\n",
    "    case T_DICT: {                                                /* 【“甲”：1，“乙”：2】，空字典是【：】 */\n",
    "        Dict *d = (Dict *)v.u.p;\n",
    "        int64_t i, k = 0;\n",
    "        if (depth > 500) bh_fail(BH_TOO_DEEP);\n",
    "        sb_put(b, 0x3010);\n",
    "        if (d->len == 0) sb_put(b, 0xFF1A);\n",
    "        for (i = 0; i < d->used; i++) {\n",
    "            if (d->e[i].key.tag == T_UNDEF) continue;\n",
    "            if (k++) sb_put(b, 0xFF0C);\n",
    "            bh_show_into(b, d->e[i].key, 1, depth + 1);\n",
    "            sb_put(b, 0xFF1A);\n",
    "            bh_show_into(b, d->e[i].val, 1, depth + 1);\n",
    "        }\n",
    "        sb_put(b, 0x3011);\n",
    "        return;\n",
    "    }\n",
    "    case T_FUNC: sb_utf8(b, \"<函数 \"); sb_utf8(b, ((Func *)v.u.p)->name); sb_put(b, '>'); return;\n",
    "    case T_BUILTIN: sb_utf8(b, \"<内置函数 \"); sb_utf8(b, ((Builtin *)v.u.p)->name); sb_put(b, '>'); return;\n",
    "    default: sb_put(b, '\?');\n",
    "    }\n",
    "}\n",
    "\n",
    "static Value bh_show(Value v, int quote) {\n",
    "    SB b = {NULL, 0, 0};\n",
    "    char buf[64];\n",
    "    if (v.tag == T_STR && !quote) return v;                     /* 文字不会被改动，不用抄一份 */\n",
    "    if (v.tag == T_INT) return bh_ascii_str(buf, (size_t)bh_itoa(v.u.i, buf));\n",
    "    if (v.tag == T_FLOAT) {\n",
    "        snprintf(buf, sizeof buf, \"%.12g\", v.u.f);\n",
    "        return bh_ascii_str(buf, strlen(buf));\n",
    "    }\n",
    "    bh_show_into(&b, v, quote, 0);\n",
    "    return sb_done(&b);\n",
    "}\n",
    "\n",
    "static const char *bh_shown(Value v) { return bh_cstr((Str *)bh_show(v, 1).u.p); }   /* 只在报错信息里用 */\n",
    "\n",
    "/* ---------- 运算 ---------- */\n",
    "\n",
    "static int bh_isint(Value v) { return v.tag == T_INT || v.tag == T_BIG; }\n",
    "static int bh_isnum(Value v) { return v.tag == T_INT || v.tag == T_BIG || v.tag == T_FLOAT; }\n",
    "\n",
    "static double bh_tof(Value v) {\n",
    "    double d;\n",
    "    if (v.tag == T_FLOAT) return v.u.f;\n",
    "    if (v.tag != T_BIG) return (double)v.u.i;\n",
    "    d = mag_to_double(((Big *)v.u.p)->d, ((Big *)v.u.p)->n);\n",
    "    if (isinf(d)) bh_fail(BH_TOO_BIG);\n",
    "    return ((Big *)v.u.p)->neg \? -d : d;\n",
    "}\n",
    "\n",
    "/* 大整数当成普通整数用（下标、次数等）：太大的按 64 位的最大、最小值算 */\n",
    "static int64_t bh_small(Value v) {\n",
    "    if (v.tag == T_BIG) return ((Big *)v.u.p)->neg \? INT64_MIN : INT64_MAX;\n",
    "    return v.u.i;\n",
    "}\n",
    "\n",
    "static BH_NOINLINE int bh_truthy_slow(Value v) {\n",
    "    switch (v.tag) {\n",
    "    case T_NIL: return 0;\n",
    "    case T_BOOL: case T_INT: return v.u.i != 0;\n",
    "    case T_FLOAT: return v.u.f != 0.0;\n",
    "    case T_STR: return ((Str *)v.u.p)->len != 0;\n",
    "    case T_LIST: return ((List *)v.u.p)->len != 0;\n",
    "    case T_DICT: return ((Dict *)v.u.p)->len != 0;\n",
    "    default: return 1;\n",
    "    }\n",
    "}\n",
    "\n",
    "\n",
    "BH_INLINE int bh_truthy(Value v) {\n",
    "    if (BH_LIKELY(v.tag == T_BOOL)) return v.u.i != 0;\n",
    "    return bh_truthy_slow(v);\n",
    "}\n",
    "\n",
    "static BH_NOINLINE Value bh_arith(int op, Value a, Value b) {\n",
    "    if (!(bh_isnum(a) && bh_isnum(b))) {\n",
    "        const char *name = op == '+' \? \"加\" : op == '-' \? \"减\" : op == '*' \? \"乘\" : op == '/' \? \"除\" : \"取余数\";\n",
    "        bh_fail(\"%s和%s之间不能%s\", bh_tn(a), bh_tn(b), name);\n",
    "    }\n",
    "    if (a.tag == T_INT && b.tag == T_INT) {       /* 最常见的情况：两个普通整数。装不下再换成大整数 */\n",
    "        int64_t x = a.u.i, y = b.u.i, r;\n",
    "        switch (op) {\n",
    "        case '+':\n",
    "            if (!((y > 0 && x > INT64_MAX - y) || (y < 0 && x < INT64_MIN - y))) return bh_int(x + y);\n",
    "            break;\n",
    "        case '-':\n",
    "            if (!((y < 0 && x > INT64_MAX + y) || (y > 0 && x < INT64_MIN + y))) return bh_int(x - y);\n",
    "            break;\n",
    "        case '*':\n",
    "            if (x > 0 \? (y > 0 \? x <= INT64_MAX / y : y >= INT64_MIN / x)\n",
    "                      : x < 0 \? (y > 0 \? x >= INT64_MIN / y : y >= INT64_MAX / x) : 1) return bh_int(x * y);\n",
    "            break;\n",
    "        case '/':\n",
    "            if (y == 0) bh_fail(\"除数不能是 0\");\n",
    "            if (x == INT64_MIN && y == -1) break;\n",
    "            if (x % y == 0) return bh_int(x / y);         /* 能整除就保持整数 */\n",
    "            return bh_float((double)x / (double)y);\n",
    "        default:\n",
    "            if (y == 0) bh_fail(\"除数不能是 0\");\n",
    "            if (y == -1) return bh_int(0);\n",
    "            r = x % y;\n",
    "            if (r != 0 && ((r < 0) != (y < 0))) r += y;   /* 余数的正负跟着除数走 */\n",
    "            return bh_int(r);\n",
    "        }\n",
    "        return bh_big_arith(op, a, b);\n",
    "    }\n",
    "    if (a.tag != T_FLOAT && b.tag != T_FLOAT) return bh_big_arith(op, a, b);\n",
    "    {\n",
    "        double x = bh_tof(a), y = bh_tof(b), r;\n",
    "        switch (op) {\n",
    "        case '+': return bh_float(x + y);\n",
    "        case '-': return bh_float(x - y);\n",
    "        case '*': return bh_float(x * y);\n",
    "        case '/':\n",
    "            if (y == 0) bh_fail(\"除数不能是 0\");\n",
    "            return bh_float(x / y);\n",
    "        default:\n",
    "            if (y == 0) bh_fail(\"除数不能是 0\");\n",
    "            r = fmod(x, y);\n",
    "            if (r != 0) { if ((r < 0) != (y < 0)) r += y; }\n",
    "            else r = copysign(0.0, y);\n",
    "            return bh_float(r);\n",
    "        }\n",
    "    }\n",
    "}\n",
    "\n",
    "static Value bh_repeat(Value seq, int64_t times) {\n",
    "    int64_t i;\n",
    "    if (times < 0) times = 0;\n",
    "    if (seq.tag == T_STR) {\n",
    "        SB b = {NULL, 0, 0};\n",
    "        for (i = 0; i < times; i++) sb_str(&b, (Str *)seq.u.p);\n",
    "        return sb_done(&b);\n",
    "    } else {\n",
    "        List *src = (List *)seq.u.p, *out = bh_list_alloc(src->len * times);\n",
    "        int64_t j;\n",
    "        for (i = 0; i < times; i++)\n",
    "            for (j = 0; j < src->len; j++) bh_push(out, src->items[j]);\n",
    "        return bh_obj(T_LIST, out);\n",
    "    }\n",
    "}\n",
    "\n",
    "static BH_NOINLINE Value bh_add_slow(Value a, Value b) {\n",
    "    if (a.tag == T_STR || b.tag == T_STR) {               /* 文字 + 任何东西 = 拼在一起 */\n",
    "        if (a.tag != T_STR) a = bh_show(a, 0);\n",
    "        if (b.tag != T_STR) b = bh_show(b, 0);\n",
    "        return bh_str_concat((Str *)a.u.p, (Str *)b.u.p);\n",
    "    }\n",
    "    if (a.tag == T_LIST && b.tag == T_LIST) {\n",
    "        List *x = (List *)a.u.p, *y = (List *)b.u.p, *out = bh_list_alloc(x->len + y->len);\n",
    "        int64_t i;\n",
    "        for (i = 0; i < x->len; i++) bh_push(out, x->items[i]);\n",
    "        for (i = 0; i < y->len; i++) bh_push(out, y->items[i]);\n",
    "        return bh_obj(T_LIST, out);\n",
    "    }\n",
    "    return bh_arith('+', a, b);\n",
    "}\n",
    "static BH_NOINLINE Value bh_mul_slow(Value a, Value b) {     /* 文字或列表 乘以 整数 = 重复几遍 */\n",
    "    if ((a.tag == T_STR || a.tag == T_LIST) && b.tag == T_INT) return bh_repeat(a, b.u.i);\n",
    "    if ((b.tag == T_STR || b.tag == T_LIST) && a.tag == T_INT) return bh_repeat(b, a.u.i);\n",
    "    return bh_arith('*', a, b);\n",
    "}\n",
    "\n",
    "\n",
    "/* ---------- 快速路径 ----------\n",
    " * 下面这些是生成的代码最常调用的操作。最常见的情况（两个普通整数、列表配整数下标……）\n",
    " * 直接在调用的地方算完；别的情况交给上面那些慢一点、但什么都能处理的函数。 */\n",
    "\n",
    "BH_INLINE int bh_add_ok(int64_t x, int64_t y, int64_t *r) {\n",
    "#ifdef BH_HAVE_OVERFLOW_BUILTINS\n",
    "    return !__builtin_add_overflow(x, y, r);\n",
    "#else\n",
    "    if ((y > 0 && x > INT64_MAX - y) || (y < 0 && x < INT64_MIN - y)) return 0;\n",
    "    *r = x + y;\n",
    "    return 1;\n",
    "#endif\n",
    "}\n",
    "BH_INLINE int bh_sub_ok(int64_t x, int64_t y, int64_t *r) {\n",
    "#ifdef BH_HAVE_OVERFLOW_BUILTINS\n",
    "    return !__builtin_sub_overflow(x, y, r);\n",
    "#else\n",
    "    if ((y < 0 && x > INT64_MAX + y) || (y > 0 && x < INT64_MIN + y)) return 0;\n",
    "    *r = x - y;\n",
    "    return 1;\n",
    "#endif\n",
    "}\n",
    "BH_INLINE int bh_mul_ok(int64_t x, int64_t y, int64_t *r) {\n",
    "#ifdef BH_HAVE_OVERFLOW_BUILTINS\n",
    "    return !__builtin_mul_overflow(x, y, r);\n",
    "#else\n",
    "    if (!(x > 0 \? (y > 0 \? x <= INT64_MAX / y : y >= INT64_MIN / x)\n",
    "                : x < 0 \? (y > 0 \? x >= INT64_MIN / y : y >= INT64_MAX / x) : 1)) return 0;\n",
    "    *r = x * y;\n",
    "    return 1;\n",
    "#endif\n",
    "}\n",
    "\n",
    "BH_INLINE Value bh_add(Value a, Value b) {\n",
    "    int64_t r;\n",
    "    if (BH_LIKELY(a.tag == T_INT && b.tag == T_INT) && BH_LIKELY(bh_add_ok(a.u.i, b.u.i, &r))) return bh_int(r);\n",
    "    return bh_add_slow(a, b);\n",
    "}\n",
    "BH_INLINE Value bh_sub(Value a, Value b) {\n",
    "    int64_t r;\n",
    "    if (BH_LIKELY(a.tag == T_INT && b.tag == T_INT) && BH_LIKELY(bh_sub_ok(a.u.i, b.u.i, &r))) return bh_int(r);\n",
    "    return bh_arith('-', a, b);\n",
    "}\n",
    "BH_INLINE Value bh_mul(Value a, Value b) {\n",
    "    int64_t r;\n",
    "    if (BH_LIKELY(a.tag == T_INT && b.tag == T_INT) && BH_LIKELY(bh_mul_ok(a.u.i, b.u.i, &r))) return bh_int(r);\n",
    "    return bh_mul_slow(a, b);\n",
    "}\n",
    "BH_INLINE Value bh_div(Value a, Value b) {\n",
    "    if (BH_LIKELY(a.tag == T_INT && b.tag == T_INT && b.u.i > 0)) {\n",
    "        if (a.u.i % b.u.i == 0) return bh_int(a.u.i / b.u.i);       /* 能整除就保持整数 */\n",
    "        return bh_float((double)a.u.i / (double)b.u.i);\n",
    "    }\n",
    "    return bh_arith('/', a, b);\n",
    "}\n",
    "BH_INLINE Value bh_mod(Value a, Value b) {\n",
    "    if (BH_LIKELY(a.tag == T_INT && b.tag == T_INT && b.u.i > 0)) {\n",
    "        int64_t r = a.u.i % b.u.i;\n",
    "        if (r < 0) r += b.u.i;                                   /* 余数的正负跟着除数走 */\n",
    "        return bh_int(r);\n",
    "    }\n",
    "    return bh_arith('%', a, b);\n",
    "}\n",
    "\n",
    "static Value bh_neg(Value v) {\n",
    "    BigRef r;\n",
    "    if (!bh_isnum(v)) bh_fail(\"负号只能用在数字前面，这里是%s\", bh_tn(v));\n",
    "    if (v.tag == T_FLOAT) return bh_float(-v.u.f);\n",
    "    if (v.tag == T_INT && v.u.i != INT64_MIN) return bh_int(-v.u.i);\n",
    "    big_ref(&r, v);\n",
    "    return big_make(!r.neg, r.d, r.n);\n",
    "}\n",
    "\n",
    "/* 两个数比大小，返回 -1、0、1。真假在这里当作 1 和 0。比不了（有一边不是数）时 *bad 置 1 */\n",
    "static int bh_num_cmp(Value a, Value b, int *bad) {\n",
    "    BigRef x, y;\n",
    "    if (a.tag == T_FLOAT || b.tag == T_FLOAT) {\n",
    "        double d;\n",
    "        if (a.tag == T_FLOAT && b.tag == T_FLOAT) {\n",
    "            if (a.u.f < b.u.f) return -1;\n",
    "            if (a.u.f > b.u.f) return 1;\n",
    "            if (a.u.f != b.u.f) *bad = 1;\n",
    "            return 0;\n",
    "        }\n",
    "        if (a.tag == T_FLOAT) return -bh_num_cmp(b, a, bad);\n",
    "        d = b.u.f;\n",
    "        if (d != d) { *bad = 1; return 0; }\n",
    "        if (isinf(d)) return d > 0 \? -1 : 1;\n",
    "        return bh_cmp_int_double(a, d);\n",
    "    }\n",
    "    if (a.tag != T_BIG && b.tag != T_BIG) return (a.u.i > b.u.i) - (a.u.i < b.u.i);\n",
    "    big_ref(&x, a);\n",
    "    big_ref(&y, b);\n",
    "    return big_cmp(&x, &y);\n",
    "}\n",
    "\n",
    "/* ======================================================================\n",
    " * 字典\n",
    " *\n",
    " * 和 Python 一样：条目保持放进去的先后顺序；1、1.0、真 算同一个键（它们相等）。\n",
    " * 所以散列值也要让相等的数一样：整数（不管多大）、值是整数的小数，都按\n",
    " * 「绝对值除以 2^61-1 的余数」来算。\n",
    " * ====================================================================== */\n",
    "\n",
    "static int bh_same(Value a, Value b, int depth);\n",
    "\n",
    "#define BH_HASH_P ((((uint64_t)1) << 61) - 1)\n",
    "\n",
    "static uint64_t bh_rot61(uint64_t x, int k) {        /* x 乘以 2 的 k 次方，除以 2^61-1 的余数（x 比 2^61-1 小） */\n",
    "    k %= 61;\n",
    "    if (k == 0) return x;\n",
    "    return ((x << k) & BH_HASH_P) | (x >> (61 - k));\n",
    "}\n",
    "\n",
    "static uint64_t bh_hash_mix(uint64_t x) {\n",
    "    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;\n",
    "    x ^= x >> 27; x *= 0x94d049bb133111ebULL;\n",
    "    x ^= x >> 31;\n",
    "    return x;\n",
    "}\n",
    "\n",
    "static uint64_t bh_hash_num(int neg, uint64_t m) {     /* m 是绝对值除以 2^61-1 的余数 */\n",
    "    if (neg && m) m = BH_HASH_P - m;\n",
    "    return bh_hash_mix(m ^ 0x2545F4914F6CDD1DULL);\n",
    "}\n",
    "\n",
    "static uint64_t bh_hash_i64(int64_t v) {\n",
    "    uint64_t u = v < 0 \? (uint64_t)0 - (uint64_t)v : (uint64_t)v;\n",
    "    return bh_hash_num(v < 0, u % BH_HASH_P);\n",
    "}\n",
    "\n",
    "static uint64_t bh_hash(Value v) {\n",
    "    switch (v.tag) {\n",
    "    case T_NIL: return 0x9E3779B97F4A7C15ULL;\n",
    "    case T_BOOL: case T_INT: return bh_hash_i64(v.u.i);\n",
    "    case T_BIG: {\n",
    "        Big *b = (Big *)v.u.p;\n",
    "        uint64_t h = 0;\n",
    "        int64_t i;\n",
    "        for (i = b->n - 1; i >= 0; i--) {\n",
    "            h = bh_rot61(h, 32) + b->d[i];\n",
    "            if (h >= BH_HASH_P) h -= BH_HASH_P;\n",
    "        }\n",
    "        return bh_hash_num(b->neg, h);\n",
    "    }\n",
    "    case T_FLOAT: {\n",
    "        double d = v.u.f, f;\n",
    "        uint64_t bits;\n",
    "        int ex;\n",
    "        if (isinf(d)) return d > 0 \? 0x7FF0000000000001ULL : 0xFFF0000000000001ULL;\n",
    "        if (d == floor(d)) {                                   /* 值是整数的小数：和同样大的整数一样 */\n",
    "            if (d >= -9223372036854775808.0 && d < 9223372036854775808.0) return bh_hash_i64((int64_t)d);\n",
    "            f = frexp(fabs(d), &ex);                           /* |d| = m * 2^(ex-53)，m 是 53 位的整数 */\n",
    "            return bh_hash_num(d < 0, bh_rot61((uint64_t)ldexp(f, 53), ex - 53));\n",
    "        }\n",
    "        memcpy(&bits, &d, sizeof bits);\n",
    "        return bh_hash_mix(bits ^ 0x9E3779B97F4A7C15ULL);\n",
    "    }\n",
    "    case T_STR: {\n",
    "        Str *s = (Str *)v.u.p;\n",
    "        uint64_t h = 1469598103934665603ULL;\n",
    "        int64_t i;\n",
    "        for (i = 0; i < s->len; i++) { h ^= s->cp[i]; h *= 1099511628211ULL; }\n",
    "        return bh_hash_mix(h);\n",
    "    }\n",
    "    default: return 0;\n",
    "    }\n",
    "}\n",
    "\n",
    "static void bh_check_key(Value k) {\n",
    "    if (k.tag == T_FLOAT && k.u.f != k.u.f) bh_fail(\"nan 不能当字典的键\");\n",
    "    if (!(k.tag == T_NIL || k.tag == T_BOOL || k.tag == T_INT || k.tag == T_BIG || k.tag == T_FLOAT || k.tag == T_STR))\n",
    "        bh_fail(\"%s不能当字典的键（只有数字、文字、真假和空可以）\", bh_tn(k));\n",
    "}\n",
    "\n",
    "static Dict *bh_dict_alloc(void) {\n",
    "    Dict *d = (Dict *)bh_new(K_DICT, sizeof(Dict));\n",
    "    d->len = d->used = d->cap = d->nslots = 0;\n",
    "    d->e = NULL;\n",
    "    d->slots = NULL;\n",
    "    return d;\n",
    "}\n",
    "\n",
    "/* 找键：返回它在散列表里的位置，找不到返回 -1 */\n",
    "static int64_t bh_dict_find(const Dict *d, Value k, uint64_t h) {\n",
    "    uint64_t mask, i;\n",
    "    if (!d->nslots) return -1;\n",
    "    mask = (uint64_t)d->nslots - 1;\n",
    "    for (i = h & mask;; i = (i + 1) & mask) {\n",
    "        int64_t at = d->slots[i];\n",
    "        if (at == -1) return -1;\n",
    "        if (at >= 0 && d->e[at].hash == h && bh_same(d->e[at].key, k, 0)) return (int64_t)i;\n",
    "    }\n",
    "}\n",
    "\n",
    "/* 重新整理：去掉删除过的条目，空间至少能放 want 个。散列表的格数是条目空间的两倍 */\n",
    "static void bh_dict_rebuild(Dict *d, int64_t want) {\n",
    "    int64_t cap = 8, ns, i, j = 0;\n",
    "    DictEntry *e;\n",
    "    int64_t *slots;\n",
    "    while (cap < want) cap *= 2;\n",
    "    ns = cap * 2;\n",
    "    e = (DictEntry *)bh_xmalloc((size_t)cap * sizeof(DictEntry));\n",
    "    slots = (int64_t *)bh_xmalloc((size_t)ns * sizeof(int64_t));\n",
    "    for (i = 0; i < ns; i++) slots[i] = -1;\n",
    "    for (i = 0; i < d->used; i++) {\n",
    "        uint64_t p;\n",
    "        if (d->e[i].key.tag == T_UNDEF) continue;\n",
    "        e[j] = d->e[i];\n",
    "        for (p = e[j].hash & (uint64_t)(ns - 1); slots[p] != -1; p = (p + 1) & (uint64_t)(ns - 1)) {}\n",
    "        slots[p] = j++;\n",
    "    }\n",
    "    free(d->e);\n",
    "    free(d->slots);\n",
    "    d->e = e;\n",
    "    d->slots = slots;\n",
    "    d->cap = cap;\n",
    "    d->nslots = ns;\n",
    "    d->used = j;\n",
    "    bh_since_gc += (size_t)cap * sizeof(DictEntry) + (size_t)ns * sizeof(int64_t);\n",
    "    if (bh_since_gc > bh_gc_threshold) bh_gc_wanted = 1;\n",
    "}\n",
    "\n",
    "static void bh_dict_set(Dict *d, Value k, Value v) {\n",
    "    uint64_t h, p;\n",
    "    int64_t at;\n",
    "    bh_check_key(k);\n",
    "    h = bh_hash(k);\n",
    "    at = bh_dict_find(d, k, h);\n",
    "    if (at >= 0) { d->e[d->slots[at]].val = v; return; }     /* 已经有这个键：只换值，键保持原样 */\n",
    "    if (d->used == d->cap) bh_dict_rebuild(d, d->len * 2 + 1);\n",
    "    for (p = h & (uint64_t)(d->nslots - 1); d->slots[p] != -1; p = (p + 1) & (uint64_t)(d->nslots - 1)) {}\n",
    "    d->slots[p] = d->used;\n",
    "    d->e[d->used].hash = h;\n",
    "    d->e[d->used].key = k;\n",
    "    d->e[d->used].val = v;\n",
    "    d->used++;\n",
    "    d->len++;\n",
    "}\n",
    "\n",
    "/* 取值。没有这个键时：*found 置 0 */\n",
    "static Value bh_dict_get(const Dict *d, Value k, int *found) {\n",
    "    int64_t at;\n",
    "    bh_check_key(k);\n",
    "    at = bh_dict_find(d, k, bh_hash(k));\n",
    "    *found = at >= 0;\n",
    "    return at >= 0 \? d->e[d->slots[at]].val : BH_NIL;\n",
    "}\n",
    "\n",
    "static Value bh_dict_need(const Dict *d, Value k) {\n",
    "    int found;\n",
    "    Value v = bh_dict_get(d, k, &found);\n",
    "    if (!found) bh_fail(\"字典里没有 %s 这个键\", bh_shown(k));\n",
    "    return v;\n",
    "}\n",
    "\n",
    "static Value bh_dict_remove(Dict *d, Value k) {\n",
    "    int64_t at, entry;\n",
    "    Value v;\n",
    "    bh_check_key(k);\n",
    "    at = bh_dict_find(d, k, bh_hash(k));\n",
    "    if (at < 0) bh_fail(\"字典里没有 %s 这个键\", bh_shown(k));\n",
    "    entry = d->slots[at];\n",
    "    v = d->e[entry].val;\n",
    "    d->e[entry].key.tag = T_UNDEF;\n",
    "    d->e[entry].val = BH_NIL;\n",
    "    d->slots[at] = -2;\n",
    "    d->len--;\n",
    "    return v;\n",
    "}\n",
    "\n",
    "/* 字典的字面量：【键：值，键：值】，kv 里键和值交替排着 */\n",
    "static Value bh_dict_make(int64_t pairs, const Value *kv) {\n",
    "    Dict *d = bh_dict_alloc();\n",
    "    int64_t i;\n",
    "    for (i = 0; i < pairs; i++) bh_dict_set(d, kv[2 * i], kv[2 * i + 1]);\n",
    "    return bh_obj(T_DICT, d);\n",
    "}\n",
    "\n",
    "static int bh_numlike(Value v) { return v.tag == T_BOOL || bh_isnum(v); }\n",
    "\n",
    "/* 列表内部的相等：真假在这里当作 1 和 0 */\n",
    "static int bh_same(Value a, Value b, int depth) {\n",
    "    if (bh_numlike(a) && bh_numlike(b)) {\n",
    "        int bad = 0, c = bh_num_cmp(a, b, &bad);\n",
    "        return !bad && c == 0;\n",
    "    }\n",
    "    if (a.tag != b.tag) return 0;\n",
    "    switch (a.tag) {\n",
    "    case T_NIL: return 1;\n",
    "    case T_STR: return bh_strcmp((Str *)a.u.p, (Str *)b.u.p) == 0;\n",
    "    case T_LIST: {\n",
    "        List *x = (List *)a.u.p, *y = (List *)b.u.p;\n",
    "        int64_t i;\n",
    "        if (x == y) return 1;\n",
    "        if (x->len != y->len) return 0;\n",
    "        if (depth > 500) bh_fail(BH_TOO_DEEP);\n",
    "        for (i = 0; i < x->len; i++)\n",
    "            if (!bh_same(x->items[i], y->items[i], depth + 1)) return 0;\n",
    "        return 1;\n",
    "    }\n",
    "    case T_DICT: {                                  /* 键一样、对应的值也相等，先后顺序不管 */\n",
    "        Dict *x = (Dict *)a.u.p, *y = (Dict *)b.u.p;\n",
    "        int64_t i, at;\n",
    "        if (x == y) return 1;\n",
    "        if (x->len != y->len) return 0;\n",
    "        if (depth > 500) bh_fail(BH_TOO_DEEP);\n",
    "        for (i = 0; i < x->used; i++) {\n",
    "            if (x->e[i].key.tag == T_UNDEF) continue;\n",
    "            at = bh_dict_find(y, x->e[i].key, x->e[i].hash);\n",
    "            if (at < 0 || !bh_same(x->e[i].val, y->e[y->slots[at]].val, depth + 1)) return 0;\n",
    "        }\n",
    "        return 1;\n",
    "    }\n",
    "    default: return a.u.p == b.u.p;\n",
    "    }\n",
    "}\n",
    "\n",
    "static int bh_equal(Value a, Value b) {\n",
    "    if ((a.tag == T_BOOL) != (b.tag == T_BOOL)) return 0;   /* 真 和 1 不算相等 */\n",
    "    return bh_same(a, b, 0);\n",
    "}\n",
    "\n",
    "/* op：0 等于，1 不等于，2 小于，3 大于，4 小于等于，5 大于等于 */\n",
    "static BH_NOINLINE int bh_cmp_slow(int op, Value a, Value b) {\n",
    "    int c, bad = 0;\n",
    "    if (op == 0) return bh_equal(a, b);\n",
    "    if (op == 1) return !bh_equal(a, b);\n",
    "    if (bh_isnum(a) && bh_isnum(b)) {\n",
    "        c = bh_num_cmp(a, b, &bad);\n",
    "        if (bad) return 0;\n",
    "    } else if (a.tag == T_STR && b.tag == T_STR) {\n",
    "        c = bh_strcmp((Str *)a.u.p, (Str *)b.u.p);\n",
    "    } else {\n",
    "        bh_fail(\"%s和%s没法比大小\", bh_tn(a), bh_tn(b));\n",
    "        return 0;\n",
    "    }\n",
    "    return op == 2 \? c < 0 : op == 3 \? c > 0 : op == 4 \? c <= 0 : c >= 0;\n",
    "}\n",
    "\n",
    "\n",
    "/* op 是常数，内联以后 switch 会被 C 编译器直接算掉 */\n",
    "BH_INLINE int bh_cmp(int op, Value a, Value b) {\n",
    "    if (BH_LIKELY(a.tag == T_INT && b.tag == T_INT)) {\n",
    "        int64_t x = a.u.i, y = b.u.i;\n",
    "        switch (op) {\n",
    "        case 0: return x == y;\n",
    "        case 1: return x != y;\n",
    "        case 2: return x < y;\n",
    "        case 3: return x > y;\n",
    "        case 4: return x <= y;\n",
    "        default: return x >= y;\n",
    "        }\n",
    "    }\n",
    "    return bh_cmp_slow(op, a, b);\n",
    "}\n",
    "\n",
    "static int64_t bh_check_index(Value i, int64_t len) {\n",
    "    if (!bh_isint(i)) bh_fail(\"下标必须是整数，这里是%s\", bh_tn(i));\n",
    "    if (i.tag == T_BIG || i.u.i < -len || i.u.i >= len)\n",
    "        bh_fail(\"下标 %s 超出了范围（一共 %lld 个，下标从 0 开始数）\", bh_shown(i), (long long)len);\n",
    "    return i.u.i < 0 \? i.u.i + len : i.u.i;\n",
    "}\n",
    "\n",
    "static BH_NOINLINE Value bh_index_slow(Value c, Value i) {\n",
    "    if (c.tag == T_LIST) {\n",
    "        List *l = (List *)c.u.p;\n",
    "        return l->items[bh_check_index(i, l->len)];\n",
    "    }\n",
    "    if (c.tag == T_STR) {\n",
    "        Str *s = (Str *)c.u.p;\n",
    "        return bh_char(s->cp[bh_check_index(i, s->len)]);\n",
    "    }\n",
    "    if (c.tag == T_DICT) return bh_dict_need((Dict *)c.u.p, i);\n",
    "    bh_fail(\"只有列表、文字和字典可以用【下标】取值，这里是%s\", bh_tn(c));\n",
    "    return BH_NIL;\n",
    "}\n",
    "\n",
    "static BH_NOINLINE void bh_set_index_slow(Value c, Value i, Value v) {\n",
    "    List *l;\n",
    "    if (c.tag == T_DICT) { bh_dict_set((Dict *)c.u.p, i, v); return; }\n",
    "    if (c.tag != T_LIST) bh_fail(\"只有列表和字典里的东西可以这样修改，这里是%s\", bh_tn(c));\n",
    "    l = (List *)c.u.p;\n",
    "    l->items[bh_check_index(i, l->len)] = v;\n",
    "}\n",
    "\n",
    "\n",
    "BH_INLINE Value bh_index(Value c, Value i) {\n",
    "    if (BH_LIKELY(c.tag == T_LIST && i.tag == T_INT)) {\n",
    "        List *l = (List *)c.u.p;\n",
    "        if (BH_LIKELY((uint64_t)i.u.i < (uint64_t)l->len)) return l->items[i.u.i];\n",
    "    }\n",
    "    return bh_index_slow(c, i);\n",
    "}\n",
    "BH_INLINE void bh_set_index(Value c, Value i, Value v) {\n",
    "    if (BH_LIKELY(c.tag == T_LIST && i.tag == T_INT)) {\n",
    "        List *l = (List *)c.u.p;\n",
    "        if (BH_LIKELY((uint64_t)i.u.i < (uint64_t)l->len)) { l->items[i.u.i] = v; return; }\n",
    "    }\n",
    "    bh_set_index_slow(c, i, v);\n",
    "}\n",
    "\n",
    "/* 「对于」用：先把要逐个取出的东西抄一份，循环里改原来的列表也不受影响 */\n",
    "static Value bh_iter(Value v) {\n",
    "    if (v.tag == T_LIST) return bh_list_make(((List *)v.u.p)->len, ((List *)v.u.p)->items);\n",
    "    if (v.tag == T_STR) {\n",
    "        Str *s = (Str *)v.u.p;\n",
    "        List *l = bh_list_alloc(s->len);\n",
    "        int64_t i;\n",
    "        for (i = 0; i < s->len; i++) bh_push(l, bh_char(s->cp[i]));\n",
    "        return bh_obj(T_LIST, l);\n",
    "    }\n",
    "    if (v.tag == T_DICT) {                          /* 字典：逐个取出键 */\n",
    "        Dict *d = (Dict *)v.u.p;\n",
    "        List *l = bh_list_alloc(d->len);\n",
    "        int64_t i;\n",
    "        for (i = 0; i < d->used; i++)\n",
    "            if (d->e[i].key.tag != T_UNDEF) bh_push(l, d->e[i].key);\n",
    "        return bh_obj(T_LIST, l);\n",
    "    }\n",
    "    bh_fail(\"「对于」只能逐个取出列表、文字或字典里的东西，这里是%s\", bh_tn(v));\n",
    "    return BH_NIL;\n",
    "}\n",
    "\n",
    "static int64_t bh_count(Value v) {\n",
    "    if (!bh_isint(v)) bh_fail(\"「重复」和「次」中间要放一个整数，这里是%s\", bh_tn(v));\n",
    "    return bh_small(v);\n",
    "}\n",
    "\n",
    "/* ---------- 变量 ---------- */\n",
    "\n",
    "/* 和编译器里的「长关键字」表一致：两个字以上的关键字，长的在前 */\n",
    "static const char *bh_reserved[] = {\n",
    "    \"否则如果\", \"里的每个\", \"中的每个\", \"大于等于\", \"小于等于\", \"不等于\",\n",
    "    \"函数\", \"返回\", \"全局\", \"如果\", \"否则\", \"对于\", \"重复\", \"跳出\", \"继续\",\n",
    "    \"除以\", \"大于\", \"小于\", \"等于\", \"并且\", \"或者\", \"不是\", NULL\n",
    "};\n",
    "\n",
    "static BH_NOINLINE Value bh_chk_fail(const char *name) {\n",
    "    int i;\n",
    "    for (i = 0; bh_reserved[i]; i++)\n",
    "        if (strstr(name, bh_reserved[i]) && strcmp(name, bh_reserved[i]) != 0)\n",
    "            bh_fail(\"找不到名字「%s」，是不是还没有赋值，或者打错字了？\\n\"\n",
    "                    \"提示：「%s」被当成了一个名字。如果其中的「%s」是单独的一个词，要在它前后加上空格。\",\n",
    "                    name, name, bh_reserved[i]);\n",
    "    bh_fail(\"找不到名字「%s」，是不是还没有赋值，或者打错字了？\", name);\n",
    "    return BH_NIL;\n",
    "}\n",
    "\n",
    "\n",
    "BH_INLINE Value bh_chk(Value v, const char *name) {\n",
    "    if (BH_LIKELY(v.tag != T_UNDEF)) return v;\n",
    "    return bh_chk_fail(name);\n",
    "}\n",
    "\n",
    "static void bh_modify_outer(const char *name) {\n",
    "    bh_fail(\"函数里面不能直接修改外面的变量「%s」。如果它是最外层的变量，先在函数里写一行：全局 %s\", name, name);\n",
    "}\n",
    "\n",
    "/* ---------- 函数 ---------- */\n",
    "\n",
    "static Frame *bh_frame_new(Frame *up, int64_t n) {\n",
    "    size_t size = sizeof(Frame) + (size_t)n * sizeof(Value);\n",
    "    Frame *f = (Frame *)bh_new(K_FRAME, size);\n",
    "    memset(f->v, 0, (size_t)(n + 1) * sizeof(Value));\n",
    "    f->up = up;\n",
    "    f->n = n;\n",
    "    return f;\n",
    "}\n",
    "\n",
    "static Value bh_func(Value (*code)(Frame *, Value *), int arity, const char *name, Frame *up) {\n",
    "    Func *f = (Func *)bh_new(K_FUNC, sizeof(Func));\n",
    "    f->code = code;\n",
    "    f->arity = arity;\n",
    "    f->name = name;\n",
    "    f->up = up;\n",
    "    return bh_obj(T_FUNC, f);\n",
    "}\n",
    "\n",
    "static BH_NOINLINE Value bh_call_slow(Value f, int n, Value *args, const char *callee) {\n",
    "    if (f.tag == T_BUILTIN) {\n",
    "        const Builtin *b = (const Builtin *)f.u.p;\n",
    "        if (n < b->min || (b->max >= 0 && n > b->max)) {\n",
    "            if (b->max < 0) bh_fail(\"「%s」至少需要 %d 个参数，这里给了 %d 个\", b->name, b->min, n);\n",
    "            if (b->min == b->max) bh_fail(\"「%s」需要 %d 个参数，这里给了 %d 个\", b->name, b->min, n);\n",
    "            bh_fail(\"「%s」需要 %d 到 %d 个参数，这里给了 %d 个\", b->name, b->min, b->max, n);\n",
    "        }\n",
    "        return b->fn(n, args);\n",
    "    }\n",
    "    if (f.tag == T_FUNC) {       /* 个数对的情况在 bh_call 里就处理完了，到这里一定是个数不对 */\n",
    "        Func *fn = (Func *)f.u.p;\n",
    "        bh_fail(\"函数「%s」需要 %d 个参数，这里给了 %d 个\", fn->name, fn->arity, n);\n",
    "    }\n",
    "    if (callee) bh_fail(\"「%s」是%s，不是函数，不能加括号调用\", callee, bh_tn(f));\n",
    "    bh_fail(\"这个值是%s，不是函数，不能加括号调用\", bh_tn(f));\n",
    "    return BH_NIL;\n",
    "}\n",
    "\n",
    "/* 调用函数。最常见的情况是调用白话函数、参数个数也对：\n",
    " * 检查栈还够不够用，记下这次调用（出错时显示调用过程用），然后跳过去 */\n",
    "BH_INLINE Value bh_call(Value f, int n, Value *args, const char *callee) {\n",
    "    if (BH_LIKELY(f.tag == T_FUNC) && BH_LIKELY(n == ((Func *)f.u.p)->arity)) {\n",
    "        Func *fn = (Func *)f.u.p;\n",
    "        BhCall rec;\n",
    "        Value result;\n",
    "        uintptr_t at = (uintptr_t)&rec;\n",
    "        if (BH_UNLIKELY((at < bh_stack_base \? bh_stack_base - at : at - bh_stack_base) > bh_stack_limit)) bh_fail(BH_TOO_DEEP);\n",
    "        rec.prev = bh_calls;\n",
    "        rec.name = fn->name;\n",
    "        rec.line = bh_line;\n",
    "        bh_calls = &rec;\n",
    "        result = fn->code(fn->up, args);\n",
    "        bh_calls = rec.prev;\n",
    "        bh_line = rec.line;\n",
    "        return result;\n",
    "    }\n",
    "    return bh_call_slow(f, n, args, callee);\n",
    "}\n",
    "\n",
    "/* ---------- 内置函数 ---------- */\n",
    "\n",
    "static void bh_print_to(FILE *f, int n, Value *a) {\n",
    "    SB b = {NULL, 0, 0};\n",
    "    int i;\n",
    "    for (i = 0; i < n; i++) {\n",
    "        if (i) sb_put(&b, ' ');\n",
    "        bh_show_into(&b, a[i], 0, 0);\n",
    "    }\n",
    "    sb_put(&b, '\\n');\n",
    "    bh_fwrite(f, b.cp, b.len);\n",
    "    free(b.cp);\n",
    "}\n",
    "static Value bi_print(int n, Value *a) { bh_print_to(stdout, n, a); return BH_NIL; }\n",
    "static Value bi_eprint(int n, Value *a) { fflush(stdout); bh_print_to(stderr, n, a); return BH_NIL; }\n",
    "\n",
    "static Value bi_input(int n, Value *a) {\n",
    "    size_t cap = 256, len = 0;\n",
    "    char *buf = (char *)bh_xmalloc(cap);\n",
    "    int c;\n",
    "    Value result;\n",
    "    if (n) {\n",
    "        Str *prompt = (Str *)bh_show(a[0], 0).u.p;\n",
    "        bh_fwrite(stdout, prompt->cp, prompt->len);\n",
    "    }\n",
    "    fflush(stdout);\n",
    "    while ((c = fgetc(stdin)) != EOF && c != '\\n') {\n",
    "        if (len + 1 >= cap) {\n",
    "            cap *= 2;\n",
    "            buf = (char *)realloc(buf, cap);\n",
    "            if (!buf) bh_fail(\"内存不够了\");\n",
    "        }\n",
    "        buf[len++] = (char)c;\n",
    "    }\n",
    "    if (len == 0 && feof(stdin)) bh_fail(\"没有可以读取的输入了\");\n",
    "    if (len && buf[len - 1] == '\\r') len--;\n",
    "    result = bh_obj(T_STR, bh_decode(buf, len));\n",
    "    free(buf);\n",
    "    return result;\n",
    "}\n",
    "\n",
    "static Value bi_len(int n, Value *a) {\n",
    "    (void)n;\n",
    "    if (a[0].tag == T_STR) return bh_int(((Str *)a[0].u.p)->len);\n",
    "    if (a[0].tag == T_LIST) return bh_int(((List *)a[0].u.p)->len);\n",
    "    if (a[0].tag == T_DICT) return bh_int(((Dict *)a[0].u.p)->len);\n",
    "    bh_fail(\"只有文字、列表和字典有长度，这里是%s\", bh_tn(a[0]));\n",
    "    return BH_NIL;\n",
    "}\n",
    "\n",
    "/* 范围（1，5）是【1，2，3，4，5】，两头都算 */\n",
    "static Value bi_range(int n, Value *a) {\n",
    "    int64_t start = 1, stop, step = 1, i;\n",
    "    List *l;\n",
    "    int k;\n",
    "    for (k = 0; k < n; k++) {\n",
    "        if (!bh_isint(a[k])) bh_fail(\"「范围」的参数必须是整数\");\n",
    "        if (a[k].tag == T_BIG) bh_fail(BH_TOO_BIG);\n",
    "    }\n",
    "    if (n == 1) stop = a[0].u.i;\n",
    "    else { start = a[0].u.i; stop = a[1].u.i; if (n == 3) step = a[2].u.i; }\n",
    "    if (step == 0) bh_fail(\"「范围」的步长不能是 0\");\n",
    "    l = bh_list_alloc(16);\n",
    "    if (step > 0) {\n",
    "        for (i = start; i <= stop; i += step) { bh_push(l, bh_int(i)); if (i > INT64_MAX - step) break; }\n",
    "    } else {\n",
    "        for (i = start; i >= stop; i += step) { bh_push(l, bh_int(i)); if (i < INT64_MIN - step) break; }\n",
    "    }\n",
    "    return bh_obj(T_LIST, l);\n",
    "}\n",
    "\n",
    "/* 「对于 范围（……）里的每个」：参数都是普通整数时，不用真的造出这个列表，边数边给。\n",
    " * 别的情况（参数不对要报错、大整数）交给「范围」本身，造出列表来逐个取 */\n",
    "typedef struct { int64_t start, step, count; int is_list; } BhRange;\n",
    "\n",
    "static Value bh_range_begin(BhRange *r, int n, Value *a) {\n",
    "    int64_t start = 1, stop, step = 1;\n",
    "    uint64_t span, q;\n",
    "    int k;\n",
    "    Value l;\n",
    "    for (k = 0; k < n; k++)\n",
    "        if (a[k].tag != T_INT) goto slow;\n",
    "    if (n == 1) stop = a[0].u.i;\n",
    "    else { start = a[0].u.i; stop = a[1].u.i; if (n == 3) step = a[2].u.i; }\n",
    "    if (step == 0) goto slow;\n",
    "    r->is_list = 0;\n",
    "    r->start = start;\n",
    "    r->step = step;\n",
    "    if (step > 0 \? stop < start : stop > start) { r->count = 0; return BH_NIL; }\n",
    "    if (step > 0) { span = (uint64_t)stop - (uint64_t)start; q = span / (uint64_t)step; }\n",
    "    else { span = (uint64_t)start - (uint64_t)stop; q = span / ((uint64_t)0 - (uint64_t)step); }\n",
    "    if (q >= (uint64_t)INT64_MAX) goto slow;\n",
    "    r->count = (int64_t)q + 1;\n",
    "    return BH_NIL;\n",
    "slow:\n",
    "    l = bi_range(n, a);\n",
    "    r->is_list = 1;\n",
    "    r->count = ((List *)l.u.p)->len;\n",
    "    return l;\n",
    "}\n",
    "\n",
    "BH_INLINE Value bh_range_at(const BhRange *r, Value list, int64_t i) {\n",
    "    if (r->is_list) return ((List *)list.u.p)->items[i];\n",
    "    return bh_int((int64_t)((uint64_t)r->start + (uint64_t)i * (uint64_t)r->step));\n",
    "}\n",
    "\n",
    "static int bh_isspace(uint32_t c) {\n",
    "    return c == ' ' || (c >= 9 && c <= 13) || (c >= 0x1C && c <= 0x1F) || c == 0x85 || c == 0xA0\n",
    "        || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029\n",
    "        || c == 0x202F || c == 0x205F || c == 0x3000;\n",
    "}\n",
    "\n",
    "/* 去掉两头的空白，全角数字换成半角，得到一段可以交给 C 解析的文本。用完要 free */\n",
    "static char *bh_number_text(const Str *s) {\n",
    "    int64_t i = 0, j = s->len, k;\n",
    "    char *o;\n",
    "    while (i < j && bh_isspace(s->cp[i])) i++;\n",
    "    while (j > i && bh_isspace(s->cp[j - 1])) j--;\n",
    "    o = (char *)bh_xmalloc((size_t)(j - i) + 1);\n",
    "    for (k = i; k < j; k++) {\n",
    "        uint32_t c = s->cp[k];\n",
    "        if (c >= 0xFF10 && c <= 0xFF19) c -= 0xFEE0;\n",
    "        o[k - i] = c < 128 \? (char)c : '\?';\n",
    "    }\n",
    "    o[j - i] = 0;\n",
    "    return o;\n",
    "}\n",
    "\n",
    "static int bh_digit(char c) { return c >= '0' && c <= '9'; }\n",
    "\n",
    "static int bh_int_syntax(const char *p) {       /* 是不是「正负号 + 一串数字」 */\n",
    "    if (*p == '+' || *p == '-') p++;\n",
    "    if (!bh_digit(*p)) return 0;\n",
    "    while (bh_digit(*p)) p++;\n",
    "    return *p == 0;\n",
    "}\n",
    "\n",
    "static int bh_word_is(const char *p, const char *word) {\n",
    "    for (; *word; p++, word++)\n",
    "        if ((*p | 32) != *word) return 0;\n",
    "    return *p == 0;\n",
    "}\n",
    "\n",
    "static int bh_parse_float(const char *z, double *out) {\n",
    "    const char *p = z;\n",
    "    int digits = 0;\n",
    "    if (*p == '+' || *p == '-') p++;\n",
    "    if (bh_word_is(p, \"inf\") || bh_word_is(p, \"infinity\") || bh_word_is(p, \"nan\")) {\n",
    "        *out = strtod(z, NULL);\n",
    "        return 1;\n",
    "    }\n",
    "    while (bh_digit(*p)) { p++; digits++; }\n",
    "    if (*p == '.') { p++; while (bh_digit(*p)) { p++; digits++; } }\n",
    "    if (!digits) return 0;\n",
    "    if (*p == 'e' || *p == 'E') {\n",
    "        p++;\n",
    "        if (*p == '+' || *p == '-') p++;\n",
    "        if (!bh_digit(*p)) return 0;\n",
    "        while (bh_digit(*p)) p++;\n",
    "    }\n",
    "    if (*p) return 0;\n",
    "    *out = strtod(z, NULL);\n",
    "    return 1;\n",
    "}\n",
    "\n",
    "static Value bi_int(int n, Value *a) {\n",
    "    Value v = a[0], result;\n",
    "    double d;\n",
    "    (void)n;\n",
    "    if (bh_isint(v)) return v;\n",
    "    if (v.tag == T_FLOAT && !isinf(v.u.f) && v.u.f == v.u.f) return bh_from_double(v.u.f);\n",
    "    if (v.tag == T_STR) {\n",
    "        char *z = bh_number_text((Str *)v.u.p);\n",
    "        int ok = 1;\n",
    "        if (bh_int_syntax(z)) result = bh_big_lit(z);\n",
    "        else if (bh_parse_float(z, &d) && !isinf(d) && d == d) result = bh_from_double(d);\n",
    "        else ok = 0;\n",
    "        free(z);\n",
    "        if (ok) return result;\n",
    "    }\n",
    "    bh_fail(\"没法把 %s 变成整数\", bh_shown(v));\n",
    "    return BH_NIL;\n",
    "}\n",
    "\n",
    "static Value bi_float(int n, Value *a) {\n",
    "    Value v = a[0];\n",
    "    double d;\n",
    "    (void)n;\n",
    "    if (v.tag == T_FLOAT) return v;\n",
    "    if (v.tag == T_INT) return bh_float((double)v.u.i);\n",
    "    if (v.tag == T_BIG) {\n",
    "        d = mag_to_double(((Big *)v.u.p)->d, ((Big *)v.u.p)->n);\n",
    "        if (!isinf(d)) return bh_float(((Big *)v.u.p)->neg \? -d : d);\n",
    "    } else if (v.tag == T_STR) {\n",
    "        char *z = bh_number_text((Str *)v.u.p);\n",
    "        int ok = bh_parse_float(z, &d);\n",
    "        free(z);\n",
    "        if (ok) return bh_float(d);\n",
    "    }\n",
    "    bh_fail(\"没法把 %s 变成小数\", bh_shown(v));\n",
    "    return BH_NIL;\n",
    "}\n",
    "\n",
    "static Value bi_str(int n, Value *a) { (void)n; return bh_show(a[0], 0); }\n",
    "static Value bi_type(int n, Value *a) { (void)n; return bh_str_lit(bh_tn(a[0])); }\n",
    "\n",
    "static Value bi_append(int n, Value *a) {\n",
    "    (void)n;\n",
    "    if (a[0].tag != T_LIST) bh_fail(\"「添加」的第一个参数必须是列表，这里是%s\", bh_tn(a[0]));\n",
    "    bh_push((List *)a[0].u.p, a[1]);\n",
    "    return BH_NIL;\n",
    "}\n",
    "\n",
    "static Value bi_remove(int n, Value *a) {\n",
    "    List *l;\n",
    "    int64_t i;\n",
    "    Value removed;\n",
    "    (void)n;\n",
    "    if (a[0].tag == T_DICT) return bh_dict_remove((Dict *)a[0].u.p, a[1]);\n",
    "    if (a[0].tag != T_LIST) bh_fail(\"「删除」的第一个参数必须是列表或字典，这里是%s\", bh_tn(a[0]));\n",
    "    l = (List *)a[0].u.p;\n",
    "    if (a[1].tag != T_INT || a[1].u.i < -l->len || a[1].u.i >= l->len)\n",
    "        bh_fail(\"「删除」的第二个参数是下标，%s 不在范围里（一共 %lld 个，下标从 0 开始数）\",\n",
    "                bh_shown(a[1]), (long long)l->len);\n",
    "    i = a[1].u.i < 0 \? a[1].u.i + l->len : a[1].u.i;\n",
    "    removed = l->items[i];\n",
    "    memmove(l->items + i, l->items + i + 1, (size_t)(l->len - i - 1) * sizeof(Value));\n",
    "    l->len--;\n",
    "    return removed;\n",
    "}\n",
    "\n",
    "/* 排序时的「小于」。类型对不上就报错 */\n",
    "static int bh_sort_less(Value a, Value b, int depth) {\n",
    "    if (bh_numlike(a) && bh_numlike(b)) {\n",
    "        int bad = 0, c = bh_num_cmp(a, b, &bad);\n",
    "        return !bad && c < 0;\n",
    "    }\n",
    "    if (a.tag == T_STR && b.tag == T_STR) return bh_strcmp((Str *)a.u.p, (Str *)b.u.p) < 0;\n",
    "    if (a.tag == T_LIST && b.tag == T_LIST && depth < 500) {\n",
    "        List *x = (List *)a.u.p, *y = (List *)b.u.p;\n",
    "        int64_t i, m = x->len < y->len \? x->len : y->len;\n",
    "        for (i = 0; i < m; i++)\n",
    "            if (!bh_same(x->items[i], y->items[i], 0)) return bh_sort_less(x->items[i], y->items[i], depth + 1);\n",
    "        return x->len < y->len;\n",
    "    }\n",
    "    bh_fail(\"列表里的东西不是同一类，没法排序\");\n",
    "    return 0;\n",
    "}\n",
    "\n",
    "static void bh_merge_sort(Value *v, Value *tmp, int64_t n) {\n",
    "    int64_t mid = n / 2, i = 0, j = mid, k = 0;\n",
    "    if (n < 2) return;\n",
    "    bh_merge_sort(v, tmp, mid);\n",
    "    bh_merge_sort(v + mid, tmp, n - mid);\n",
    "    while (i < mid && j < n) {\n",
    "        if (bh_sort_less(v[j], v[i], 0)) tmp[k++] = v[j++];   /* 相等时左边的在前：稳定排序 */\n",
    "        else tmp[k++] = v[i++];\n",
    "    }\n",
    "    while (i < mid) tmp[k++] = v[i++];\n",
    "    while (j < n) tmp[k++] = v[j++];\n",
    "    memcpy(v, tmp, (size_t)n * sizeof(Value));\n",
    "}\n",
    "\n",
    "static Value bi_sorted(int n, Value *a) {\n",
    "    Value out;\n",
    "    List *l;\n",
    "    (void)n;\n",
    "    if (a[0].tag != T_LIST) bh_fail(\"「排序」的参数必须是列表，这里是%s\", bh_tn(a[0]));\n",
    "    out = bh_list_make(((List *)a[0].u.p)->len, ((List *)a[0].u.p)->items);\n",
    "    l = (List *)out.u.p;\n",
    "    if (l->len > 1) {\n",
    "        Value *tmp = (Value *)bh_xmalloc((size_t)l->len * sizeof(Value));\n",
    "        bh_merge_sort(l->items, tmp, l->len);\n",
    "        free(tmp);\n",
    "    }\n",
    "    return out;\n",
    "}\n",
    "\n",
    "static uint64_t bh_random(void) {\n",
    "    static uint64_t state = 0;\n",
    "    if (!state) state = ((uint64_t)time(NULL) << 20) ^ (uint64_t)clock() ^ (uint64_t)(uintptr_t)&state ^ 0x9E3779B97F4A7C15ULL;\n",
    "    state ^= state >> 12;\n",
    "    state ^= state << 25;\n",
    "    state ^= state >> 27;\n",
    "    return state * 0x2545F4914F6CDD1DULL;\n",
    "}\n",
    "\n",
    "static Value bi_randint(int n, Value *a) {\n",
    "    uint64_t span, r;\n",
    "    (void)n;\n",
    "    if (a[0].tag == T_BIG || a[1].tag == T_BIG) bh_fail(BH_TOO_BIG);\n",
    "    if (a[0].tag != T_INT || a[1].tag != T_INT || a[0].u.i > a[1].u.i)\n",
    "        bh_fail(\"「随机整数」需要两个整数，而且前一个不能比后一个大\");\n",
    "    span = (uint64_t)a[1].u.i - (uint64_t)a[0].u.i + 1;\n",
    "    r = bh_random() >> 11;\n",
    "    if (span) r %= span;\n",
    "    return bh_int((int64_t)((uint64_t)a[0].u.i + r));\n",
    "}\n",
    "\n",
    "static Value bi_slice(int n, Value *a) {\n",
    "    int64_t total, start, len;\n",
    "    (void)n;\n",
    "    if (a[0].tag != T_STR && a[0].tag != T_LIST)\n",
    "        bh_fail(\"「截取」的第一个参数必须是文字或列表，这里是%s\", bh_tn(a[0]));\n",
    "    if (!bh_isint(a[1]) || !bh_isint(a[2]) || bh_small(a[1]) < 0 || bh_small(a[2]) < 0)\n",
    "        bh_fail(\"「截取」的开始位置和长度必须是不小于 0 的整数\");\n",
    "    total = a[0].tag == T_STR \? ((Str *)a[0].u.p)->len : ((List *)a[0].u.p)->len;\n",
    "    start = bh_small(a[1]) < total \? bh_small(a[1]) : total;\n",
    "    len = bh_small(a[2]) < total - start \? bh_small(a[2]) : total - start;\n",
    "    if (a[0].tag == T_STR) {\n",
    "        Str *s = bh_str_alloc(len);\n",
    "        if (len) memcpy(s->cp, ((Str *)a[0].u.p)->cp + start, (size_t)len * sizeof(uint32_t));\n",
    "        return bh_obj(T_STR, s);\n",
    "    }\n",
    "    return bh_list_make(len, ((List *)a[0].u.p)->items + start);\n",
    "}\n",
    "\n",
    "static Value bi_ord(int n, Value *a) {\n",
    "    (void)n;\n",
    "    if (a[0].tag != T_STR || ((Str *)a[0].u.p)->len != 1) bh_fail(\"「编码」需要一个单独的字，这里是 %s\", bh_shown(a[0]));\n",
    "    return bh_int(((Str *)a[0].u.p)->cp[0]);\n",
    "}\n",
    "\n",
    "static Value bi_chr(int n, Value *a) {\n",
    "    (void)n;\n",
    "    if (a[0].tag != T_INT || a[0].u.i < 0 || a[0].u.i > 0x10FFFF || (a[0].u.i >= 0xD800 && a[0].u.i <= 0xDFFF))\n",
    "        bh_fail(\"「字符」需要一个有效的文字编码，这里是 %s\", bh_shown(a[0]));\n",
    "    return bh_char((uint32_t)a[0].u.i);\n",
    "}\n",
    "\n",
    "static Value bi_join(int n, Value *a) {\n",
    "    SB b = {NULL, 0, 0};\n",
    "    List *l;\n",
    "    int64_t i;\n",
    "    if (a[0].tag != T_LIST) bh_fail(\"「连接」的第一个参数必须是列表，这里是%s\", bh_tn(a[0]));\n",
    "    if (n == 2 && a[1].tag != T_STR) bh_fail(\"「连接」的分隔符必须是文字，这里是%s\", bh_tn(a[1]));\n",
    "    l = (List *)a[0].u.p;\n",
    "    for (i = 0; i < l->len; i++) {\n",
    "        if (i && n == 2) sb_str(&b, (Str *)a[1].u.p);\n",
    "        bh_show_into(&b, l->items[i], 0, 0);\n",
    "    }\n",
    "    return sb_done(&b);\n",
    "}\n",
    "\n",
    "static int64_t bh_str_find(const Str *text, const Str *part, int64_t from) {\n",
    "    int64_t i;\n",
    "    for (i = from; i + part->len <= text->len; i++)\n",
    "        if (part->len == 0 || memcmp(text->cp + i, part->cp, (size_t)part->len * sizeof(uint32_t)) == 0) return i;\n",
    "    return -1;\n",
    "}\n",
    "\n",
    "static Value bi_find(int n, Value *a) {\n",
    "    (void)n;\n",
    "    if (a[0].tag == T_STR) {\n",
    "        if (a[1].tag != T_STR) bh_fail(\"在文字里只能查找文字，这里是%s\", bh_tn(a[1]));\n",
    "        return bh_int(bh_str_find((Str *)a[0].u.p, (Str *)a[1].u.p, 0));\n",
    "    }\n",
    "    if (a[0].tag == T_LIST) {\n",
    "        List *l = (List *)a[0].u.p;\n",
    "        int64_t i;\n",
    "        for (i = 0; i < l->len; i++)\n",
    "            if (bh_equal(l->items[i], a[1])) return bh_int(i);\n",
    "        return bh_int(-1);\n",
    "    }\n",
    "    bh_fail(\"「查找」的第一个参数必须是文字或列表，这里是%s\", bh_tn(a[0]));\n",
    "    return BH_NIL;\n",
    "}\n",
    "\n",
    "static Value bi_split(int n, Value *a) {\n",
    "    Str *text, *sep, *piece;\n",
    "    List *out;\n",
    "    int64_t from = 0, at;\n",
    "    (void)n;\n",
    "    if (a[0].tag != T_STR || a[1].tag != T_STR) bh_fail(\"「分割」的两个参数都必须是文字\");\n",
    "    text = (Str *)a[0].u.p;\n",
    "    sep = (Str *)a[1].u.p;\n",
    "    if (sep->len == 0) bh_fail(\"「分割」的分隔符不能是空的\");\n",
    "    out = bh_list_alloc(8);\n",
    "    for (;;) {\n",
    "        int found = (at = bh_str_find(text, sep, from)) >= 0;\n",
    "        if (!found) at = text->len;\n",
    "        piece = bh_str_alloc(at - from);\n",
    "        if (at > from) memcpy(piece->cp, text->cp + from, (size_t)(at - from) * sizeof(uint32_t));\n",
    "        bh_push(out, bh_obj(T_STR, piece));\n",
    "        if (!found) break;\n",
    "        from = at + sep->len;\n",
    "    }\n",
    "    return bh_obj(T_LIST, out);\n",
    "}\n",
    "\n",
    "static char *bh_path(Value v) {                 /* 用完要 free */\n",
    "    if (v.tag != T_STR) bh_fail(\"文件路径必须是文字，这里是%s\", bh_tn(v));\n",
    "    return bh_cstr((Str *)v.u.p);\n",
    "}\n",
    "\n",
    "static Value bi_read_file(int n, Value *a) {\n",
    "    char *path = bh_path(a[0]);\n",
    "    FILE *f = fopen(path, \"rb\");\n",
    "    size_t cap = 65536, len = 0, got;\n",
    "    char *buf;\n",
    "    Value result;\n",
    "    (void)n;\n",
    "    free(path);\n",
    "    if (!f) return BH_NIL;                    /* 打不开就返回 空，由程序自己决定怎么办 */\n",
    "    buf = (char *)bh_xmalloc(cap);\n",
    "    while ((got = fread(buf + len, 1, cap - len, f)) > 0) {\n",
    "        len += got;\n",
    "        if (len == cap) {\n",
    "            cap *= 2;\n",
    "            buf = (char *)realloc(buf, cap);\n",
    "            if (!buf) bh_fail(\"内存不够了\");\n",
    "        }\n",
    "    }\n",
    "    fclose(f);\n",
    "    result = bh_obj(T_STR, bh_decode(buf, len));\n",
    "    free(buf);\n",
    "    return result;\n",
    "}\n",
    "\n",
    "static Value bi_write_file(int n, Value *a) {\n",
    "    char *path = bh_path(a[0]);\n",
    "    FILE *f;\n",
    "    (void)n;\n",
    "    if (a[1].tag != T_STR) bh_fail(\"「写文件」的内容必须是文字，这里是%s\", bh_tn(a[1]));\n",
    "    f = fopen(path, \"wb\");\n",
    "    free(path);\n",
    "    if (!f) return BH_FALSE;                  /* 写成功返回 真，失败返回 假 */\n",
    "    bh_fwrite(f, ((Str *)a[1].u.p)->cp, ((Str *)a[1].u.p)->len);\n",
    "    return bh_bool(fclose(f) == 0);\n",
    "}\n",
    "\n",
    "static Value bi_args(int n, Value *a) {\n",
    "    List *l = bh_list_alloc(bh_argc);\n",
    "    int i;\n",
    "    (void)n; (void)a;\n",
    "    for (i = 1; i < bh_argc; i++) bh_push(l, bh_str_lit(bh_argv[i]));\n",
    "    return bh_obj(T_LIST, l);\n",
    "}\n",
    "\n",
    "static Value bi_exit(int n, Value *a) {\n",
    "    if (n && a[0].tag != T_INT) bh_fail(\"「退出」的参数必须是整数，这里是%s\", bh_tn(a[0]));\n",
    "    fflush(stdout);\n",
    "    exit(n \? (int)a[0].u.i : 0);\n",
    "    return BH_NIL;\n",
    "}\n",
    "\n",
    "static Value bi_system(int n, Value *a) {\n",
    "    int status;\n",
    "    char *command;\n",
    "    (void)n;\n",
    "    if (a[0].tag != T_STR) bh_fail(\"「运行命令」的参数必须是文字，这里是%s\", bh_tn(a[0]));\n",
    "    fflush(stdout);\n",
    "    fflush(stderr);\n",
    "    command = bh_cstr((Str *)a[0].u.p);\n",
    "    status = system(command);\n",
    "    free(command);\n",
    "#ifndef _WIN32\n",
    "    if (status != -1 && WIFEXITED(status)) status = WEXITSTATUS(status);\n",
    "    else if (status != 0) status = 1;\n",
    "#endif\n",
    "    return bh_int(status);\n",
    "}\n",
    "\n",
    "static Value bi_runtime_source(int n, Value *a) {\n",
    "    SB b = {NULL, 0, 0};\n",
    "    int i;\n",
    "    (void)n; (void)a;\n",
    "    if (!bh_rt_lines) bh_fail(\"这个程序里没有带运行时源码\");\n",
    "    for (i = 0; i < bh_rt_lines_n; i++) sb_utf8(&b, bh_rt_lines[i]);\n",
    "    return sb_done(&b);\n",
    "}\n",
    "\n",
    "/* ---------- 0.3 版新增：字典、按行读文件、格式化、文字处理 ---------- */\n",
    "\n",
    "static Value bi_contains(int n, Value *a) {\n",
    "    (void)n;\n",
    "    if (a[0].tag == T_DICT) {\n",
    "        bh_check_key(a[1]);\n",
    "        return bh_bool(bh_dict_find((Dict *)a[0].u.p, a[1], bh_hash(a[1])) >= 0);\n",
    "    }\n",
    "    if (a[0].tag == T_LIST) {\n",
    "        List *l = (List *)a[0].u.p;\n",
    "        int64_t i;\n",
    "        for (i = 0; i < l->len; i++)\n",
    "            if (bh_equal(l->items[i], a[1])) return BH_TRUE;\n",
    "        return BH_FALSE;\n",
    "    }\n",
    "    if (a[0].tag == T_STR) {\n",
    "        if (a[1].tag != T_STR) bh_fail(\"在文字里只能查找文字，这里是%s\", bh_tn(a[1]));\n",
    "        return bh_bool(bh_str_find((Str *)a[0].u.p, (Str *)a[1].u.p, 0) >= 0);\n",
    "    }\n",
    "    bh_fail(\"「包含」的第一个参数必须是列表、文字或字典，这里是%s\", bh_tn(a[0]));\n",
    "    return BH_NIL;\n",
    "}\n",
    "\n",
    "static Dict *bh_need_dict(Value v, const char *who) {\n",
    "    if (v.tag != T_DICT) bh_fail(\"「%s」的参数必须是字典，这里是%s\", who, bh_tn(v));\n",
    "    return (Dict *)v.u.p;\n",
    "}\n",
    "\n",
    "static Value bi_keys(int n, Value *a) {\n",
    "    Dict *d = bh_need_dict(a[0], \"所有键\");\n",
    "    List *l = bh_list_alloc(d->len);\n",
    "    int64_t i;\n",
    "    (void)n;\n",
    "    for (i = 0; i < d->used; i++)\n",
    "        if (d->e[i].key.tag != T_UNDEF) bh_push(l, d->e[i].key);\n",
    "    return bh_obj(T_LIST, l);\n",
    "}\n",
    "\n",
    "static Value bi_values(int n, Value *a) {\n",
    "    Dict *d = bh_need_dict(a[0], \"所有值\");\n",
    "    List *l = bh_list_alloc(d->len);\n",
    "    int64_t i;\n",
    "    (void)n;\n",
    "    for (i = 0; i < d->used; i++)\n",
    "        if (d->e[i].key.tag != T_UNDEF) bh_push(l, d->e[i].val);\n",
    "    return bh_obj(T_LIST, l);\n",
    "}\n",
    "\n",
    "static Value bi_items(int n, Value *a) {\n",
    "    Dict *d = bh_need_dict(a[0], \"所有项\");\n",
    "    List *l = bh_list_alloc(d->len);\n",
    "    int64_t i;\n",
    "    (void)n;\n",
    "    for (i = 0; i < d->used; i++) {\n",
    "        Value pair[2];\n",
    "        if (d->e[i].key.tag == T_UNDEF) continue;\n",
    "        pair[0] = d->e[i].key;\n",
    "        pair[1] = d->e[i].val;\n",
    "        bh_push(l, bh_list_make(2, pair));\n",
    "    }\n",
    "    return bh_obj(T_LIST, l);\n",
    "}\n",
    "\n",
    "static Value bi_get(int n, Value *a) {\n",
    "    int found;\n",
    "    Value v;\n",
    "    if (a[0].tag != T_DICT) bh_fail(\"「取」的第一个参数必须是字典，这里是%s\", bh_tn(a[0]));\n",
    "    v = bh_dict_get((Dict *)a[0].u.p, a[1], &found);\n",
    "    if (found) return v;\n",
    "    return n == 3 \? a[2] : BH_NIL;\n",
    "}\n",
    "\n",
    "/* 格式化（“{}今年{}岁”，名字，年龄）：模板里的 {} 依次换成后面的值。\n",
    " * 全角的 ｛｝ 也行；想要大括号本身就连写两个：{{ }} */\n",
    "static Value bi_format(int n, Value *a) {\n",
    "    SB b = {NULL, 0, 0};\n",
    "    Str *t;\n",
    "    int64_t i = 0, count = 0;\n",
    "    if (a[0].tag != T_STR) bh_fail(\"「格式化」的第一个参数必须是文字，这里是%s\", bh_tn(a[0]));\n",
    "    t = (Str *)a[0].u.p;\n",
    "    while (i < t->len) {\n",
    "        uint32_t c = t->cp[i];\n",
    "        if (c == '{' || c == 0xFF5B) {\n",
    "            uint32_t close = c == '{' \? '}' : 0xFF5D;\n",
    "            if (i + 1 < t->len && t->cp[i + 1] == c) { sb_put(&b, c); i += 2; continue; }\n",
    "            if (i + 1 < t->len && t->cp[i + 1] == close) {\n",
    "                if (count < n - 1) bh_show_into(&b, a[1 + count], 0, 0);\n",
    "                count++;\n",
    "                i += 2;\n",
    "                continue;\n",
    "            }\n",
    "        } else if ((c == '}' || c == 0xFF5D) && i + 1 < t->len && t->cp[i + 1] == c) {\n",
    "            sb_put(&b, c);\n",
    "            i += 2;\n",
    "            continue;\n",
    "        }\n",
    "        sb_put(&b, c);\n",
    "        i++;\n",
    "    }\n",
    "    if (count != n - 1) {\n",
    "        free(b.cp);\n",
    "        bh_fail(\"「格式化」的模板里有 %lld 个 {}，后面却给了 %d 个值\", (long long)count, n - 1);\n",
    "    }\n",
    "    return sb_done(&b);\n",
    "}\n",
    "\n",
    "static Value bi_fixed(int n, Value *a) {\n",
    "    char buf[512];\n",
    "    double x;\n",
    "    (void)n;\n",
    "    if (!bh_isnum(a[0])) bh_fail(\"「保留小数」的第一个参数必须是数字，这里是%s\", bh_tn(a[0]));\n",
    "    if (a[1].tag != T_INT || a[1].u.i < 0 || a[1].u.i > 100) bh_fail(\"「保留小数」的位数必须是 0 到 100 之间的整数\");\n",
    "    x = bh_tof(a[0]);\n",
    "    snprintf(buf, sizeof buf, \"%.*f\", (int)a[1].u.i, x);\n",
    "    return bh_ascii_str(buf, strlen(buf));\n",
    "}\n",
    "\n",
    "/* 读行：读出整个文件，按行分开。每行末尾的换行不要；文件最后的换行不会多出一个空行 */\n",
    "static Value bi_read_lines(int n, Value *a) {\n",
    "    Value text = bi_read_file(n, a);\n",
    "    Str *s, *piece;\n",
    "    List *out;\n",
    "    int64_t from = 0, i, end;\n",
    "    if (text.tag == T_NIL) return text;\n",
    "    s = (Str *)text.u.p;\n",
    "    out = bh_list_alloc(16);\n",
    "    for (i = 0; i <= s->len; i++) {\n",
    "        if (i < s->len && s->cp[i] != '\\n') continue;\n",
    "        if (i == s->len && from == s->len) break;\n",
    "        end = i;\n",
    "        if (end > from && s->cp[end - 1] == '\\r') end--;\n",
    "        piece = bh_str_alloc(end - from);\n",
    "        if (end > from) memcpy(piece->cp, s->cp + from, (size_t)(end - from) * sizeof(uint32_t));\n",
    "        bh_push(out, bh_obj(T_STR, piece));\n",
    "        from = i + 1;\n",
    "    }\n",
    "    return bh_obj(T_LIST, out);\n",
    "}\n",
    "\n",
    "static Value bi_append_file(int n, Value *a) {\n",
    "    char *path = bh_path(a[0]);\n",
    "    FILE *f;\n",
    "    (void)n;\n",
    "    if (a[1].tag != T_STR) { free(path); bh_fail(\"「追加文件」的内容必须是文字，这里是%s\", bh_tn(a[1])); }\n",
    "    f = fopen(path, \"ab\");\n",
    "    free(path);\n",
    "    if (!f) return BH_FALSE;\n",
    "    bh_fwrite(f, ((Str *)a[1].u.p)->cp, ((Str *)a[1].u.p)->len);\n",
    "    return bh_bool(fclose(f) == 0);\n",
    "}\n",
    "\n",
    "static Value bi_replace(int n, Value *a) {\n",
    "    SB b = {NULL, 0, 0};\n",
    "    Str *t, *old, *rep;\n",
    "    int64_t from = 0, at;\n",
    "    (void)n;\n",
    "    if (a[0].tag != T_STR || a[1].tag != T_STR || a[2].tag != T_STR) bh_fail(\"「替换」的三个参数都必须是文字\");\n",
    "    t = (Str *)a[0].u.p;\n",
    "    old = (Str *)a[1].u.p;\n",
    "    rep = (Str *)a[2].u.p;\n",
    "    if (old->len == 0) bh_fail(\"「替换」要换掉的文字不能是空的\");\n",
    "    while ((at = bh_str_find(t, old, from)) >= 0) {\n",
    "        sb_cp(&b, t->cp + from, at - from);\n",
    "        sb_str(&b, rep);\n",
    "        from = at + old->len;\n",
    "    }\n",
    "    sb_cp(&b, t->cp + from, t->len - from);\n",
    "    return sb_done(&b);\n",
    "}\n",
    "\n",
    "static Value bi_strip(int n, Value *a) {\n",
    "    Str *s, *r;\n",
    "    int64_t lo = 0, hi;\n",
    "    (void)n;\n",
    "    if (a[0].tag != T_STR) bh_fail(\"「去空白」的参数必须是文字，这里是%s\", bh_tn(a[0]));\n",
    "    s = (Str *)a[0].u.p;\n",
    "    hi = s->len;\n",
    "    while (lo < hi && bh_isspace(s->cp[lo])) lo++;\n",
    "    while (hi > lo && bh_isspace(s->cp[hi - 1])) hi--;\n",
    "    r = bh_str_alloc(hi - lo);\n",
    "    if (hi > lo) memcpy(r->cp, s->cp + lo, (size_t)(hi - lo) * sizeof(uint32_t));\n",
    "    return bh_obj(T_STR, r);\n",
    "}\n",
    "\n",
    "static const Builtin bh_builtins[] = {\n",
    "    {\"打印\", bi_print, 0, -1}, {\"输入\", bi_input, 0, 1}, {\"长度\", bi_len, 1, 1}, {\"范围\", bi_range, 1, 3},\n",
    "    {\"整数\", bi_int, 1, 1}, {\"小数\", bi_float, 1, 1}, {\"文字\", bi_str, 1, 1}, {\"类型\", bi_type, 1, 1},\n",
    "    {\"添加\", bi_append, 2, 2}, {\"删除\", bi_remove, 2, 2}, {\"排序\", bi_sorted, 1, 1}, {\"随机整数\", bi_randint, 2, 2},\n",
    "    {\"截取\", bi_slice, 3, 3}, {\"编码\", bi_ord, 1, 1}, {\"字符\", bi_chr, 1, 1}, {\"连接\", bi_join, 1, 2},\n",
    "    {\"查找\", bi_find, 2, 2}, {\"分割\", bi_split, 2, 2}, {\"读文件\", bi_read_file, 1, 1}, {\"写文件\", bi_write_file, 2, 2},\n",
    "    {\"命令行参数\", bi_args, 0, 0}, {\"退出\", bi_exit, 0, 1}, {\"运行命令\", bi_system, 1, 1}, {\"报错\", bi_eprint, 0, -1},\n",
    "    {\"运行时源码\", bi_runtime_source, 0, 0},\n",
    "    {\"包含\", bi_contains, 2, 2}, {\"所有键\", bi_keys, 1, 1}, {\"所有值\", bi_values, 1, 1}, {\"所有项\", bi_items, 1, 1},\n",
    "    {\"取\", bi_get, 2, 3}, {\"格式化\", bi_format, 1, -1}, {\"保留小数\", bi_fixed, 2, 2}, {\"读行\", bi_read_lines, 1, 1},\n",
    "    {\"追加文件\", bi_append_file, 2, 2}, {\"替换\", bi_replace, 3, 3}, {\"去空白\", bi_strip, 1, 1},\n",
    "    {NULL, NULL, 0, 0}\n",
    "};\n",
    "\n",
    "/* ---------- 程序启动 ---------- */\n",
    "\n",
    "/* 告诉回收器两个「根」数组在哪：最外层的变量 G 和文字常量 K。\n",
    " * 全局变量里和内置函数同名的，先填上内置函数 */\n",
    "static void bh_init_globals(Value *globals, const char **names, int n, Value *consts, int n_consts) {\n",
    "    int i, k;\n",
    "    bh_root_arrays[0] = globals; bh_root_counts[0] = n;\n",
    "    bh_root_arrays[1] = consts; bh_root_counts[1] = n_consts;\n",
    "    for (i = 0; i < n; i++)\n",
    "        for (k = 0; bh_builtins[k].name; k++)\n",
    "            if (strcmp(names[i], bh_builtins[k].name) == 0) globals[i] = bh_obj(T_BUILTIN, (void *)&bh_builtins[k]);\n",
    "}\n",
    "\n",
    "static void bh_init(int argc, char **argv, char *stack_base) {\n",
    "    const char *stress = getenv(\"BH_GC_STRESS\");\n",
    "    bh_argc = argc;\n",
    "    bh_argv = argv;\n",
    "    bh_stack_base = (uintptr_t)stack_base;\n",
    "    if (stress && atol(stress) > 0) {\n",
    "        bh_gc_stress = bh_gc_countdown = atol(stress);\n",
    "        bh_gc_wanted = 1;\n",
    "    }\n",
    "#ifdef _WIN32\n",
    "    SetConsoleOutputCP(65001);\n",
    "    SetConsoleCP(65001);\n",
    "#else\n",
    "    {   /* 递归太深时要赶在栈用完之前报错，所以先查一下栈有多大 */\n",
    "        struct rlimit rl;\n",
    "        size_t size = (size_t)8 << 20, margin;\n",
    "        if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur > 0)\n",
    "            size = (size_t)rl.rlim_cur;\n",
    "        if (size > ((size_t)64 << 20)) size = (size_t)64 << 20;\n",
    "        /* 留出一截余量：栈顶还放着环境变量和命令行参数，最后一层函数也要有地方用 */\n",
    "        margin = size / 8 > 131072 \? size / 8 : 131072;\n",
    "        bh_stack_limit = size > 2 * margin \? size - margin : size / 2;\n",
    "    }\n",
    "#endif\n",
    "}\n",
    "\n",
    "/* ======================================================================\n",
    " * 运行时到此结束，下面是编译器根据白话程序生成的代码\n",
    " * ====================================================================== */\n",
    "",
    NULL};

static Value fn1(Frame *up, Value *a);
static Value fn2(Frame *up, Value *a);
static Value fn3(Frame *up, Value *a);
static Value fn4(Frame *up, Value *a);
static Value fn5(Frame *up, Value *a);
static Value fn6(Frame *up, Value *a);
static Value fn7(Frame *up, Value *a);
static Value fn8(Frame *up, Value *a);
static Value fn9(Frame *up, Value *a);
static Value fn10(Frame *up, Value *a);
static Value fn11(Frame *up, Value *a);
static Value fn12(Frame *up, Value *a);
static Value fn13(Frame *up, Value *a);
static Value fn14(Frame *up, Value *a);
static Value fn15(Frame *up, Value *a);
static Value fn16(Frame *up, Value *a);
static Value fn17(Frame *up, Value *a);
static Value fn18(Frame *up, Value *a);
static Value fn19(Frame *up, Value *a);
static Value fn20(Frame *up, Value *a);
static Value fn21(Frame *up, Value *a);
static Value fn22(Frame *up, Value *a);
static Value fn23(Frame *up, Value *a);
static Value fn24(Frame *up, Value *a);
static Value fn25(Frame *up, Value *a);
static Value fn26(Frame *up, Value *a);
static Value fn27(Frame *up, Value *a);
static Value fn28(Frame *up, Value *a);
static Value fn29(Frame *up, Value *a);
static Value fn30(Frame *up, Value *a);
static Value fn31(Frame *up, Value *a);
static Value fn32(Frame *up, Value *a);
static Value fn33(Frame *up, Value *a);
static Value fn34(Frame *up, Value *a);
static Value fn35(Frame *up, Value *a);
static Value fn36(Frame *up, Value *a);
static Value fn37(Frame *up, Value *a);
static Value fn38(Frame *up, Value *a);
static Value fn39(Frame *up, Value *a);
static Value fn40(Frame *up, Value *a);
static Value fn41(Frame *up, Value *a);
static Value fn42(Frame *up, Value *a);
static Value fn43(Frame *up, Value *a);
static Value fn44(Frame *up, Value *a);
static Value fn45(Frame *up, Value *a);
static Value fn46(Frame *up, Value *a);
static Value fn47(Frame *up, Value *a);
static Value fn48(Frame *up, Value *a);
static Value fn49(Frame *up, Value *a);
static Value fn50(Frame *up, Value *a);
static Value fn51(Frame *up, Value *a);
static Value fn52(Frame *up, Value *a);
static Value fn53(Frame *up, Value *a);
static Value fn54(Frame *up, Value *a);
static Value fn55(Frame *up, Value *a);
static Value fn56(Frame *up, Value *a);
static Value fn57(Frame *up, Value *a);
static Value fn58(Frame *up, Value *a);
static Value fn59(Frame *up, Value *a);
static Value fn60(Frame *up, Value *a);
static Value fn61(Frame *up, Value *a);
static Value fn62(Frame *up, Value *a);
static Value fn63(Frame *up, Value *a);
static Value fn64(Frame *up, Value *a);
static Value fn65(Frame *up, Value *a);
static Value fn66(Frame *up, Value *a);
static Value fn67(Frame *up, Value *a);
static Value fn68(Frame *up, Value *a);
static Value fn69(Frame *up, Value *a);
static Value fn70(Frame *up, Value *a);
static Value fn71(Frame *up, Value *a);
static Value fn72(Frame *up, Value *a);
static Value fn73(Frame *up, Value *a);
static Value fn74(Frame *up, Value *a);
static Value fn75(Frame *up, Value *a);
static Value fn76(Frame *up, Value *a);
static Value fn77(Frame *up, Value *a);
static Value fn78(Frame *up, Value *a);
static Value fn79(Frame *up, Value *a);
static Value fn80(Frame *up, Value *a);
static Value fn81(Frame *up, Value *a);
static Value fn82(Frame *up, Value *a);
static Value fn83(Frame *up, Value *a);
static Value fn84(Frame *up, Value *a);
static Value fn85(Frame *up, Value *a);
static Value fn86(Frame *up, Value *a);
static Value fn87(Frame *up, Value *a);
static Value fn88(Frame *up, Value *a);
static Value fn89(Frame *up, Value *a);

static Value fn1(Frame *up, Value *a) {    /* 函数 半角，第 57 行 */
    Value L[3] = {{0}};
    Value t0, t1, t2, t3, t4, t5, t6, t7;
    L[0] = a[0];    /* 字 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    bh_line = 59;
    t0 = bh_chk(G[6], "全角表");
    t1 = bi_find(2, (Value[]){t0, L[0]});
    if (!L[1].tag && or1) bh_modify_outer("下标"); L[1] = t1;
    bh_line = 60;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[19], "下标"); }
        if (bh_cmp(5, t1, bh_int(0LL))) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 61;
        t2 = bh_chk(G[7], "半角表");
        if (L[1].tag) t3 = L[1]; else { or1 = 1; t3 = bh_chk(G[19], "下标"); }
        t4 = bh_index(t2, t3);
        return t4;
    } else {
    }
    bh_line = 62;
    t0 = bi_ord(1, (Value[]){L[0]});
    if (!L[2].tag && or2) bh_modify_outer("码"); L[2] = t0;
    bh_line = 63;
    {
        t1 = BH_FALSE;
        {
            if (L[2].tag) t2 = L[2]; else { or2 = 1; t2 = bh_chk(G[20], "码"); }
            if (bh_cmp(5, t2, bh_int(65281LL))) {
                t1 = BH_TRUE;
            }
        }
        if (bh_truthy(t1)) {
            t3 = BH_FALSE;
            {
                bh_line = 63;
                if (L[2].tag) t4 = L[2]; else { or2 = 1; t4 = bh_chk(G[20], "码"); }
                if (bh_cmp(4, t4, bh_int(65374LL))) {
                    t3 = BH_TRUE;
                }
            }
            t0 = t3;
        } else {
            t0 = t1;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 64;
        if (L[2].tag) t5 = L[2]; else { or2 = 1; t5 = bh_chk(G[20], "码"); }
        t6 = bh_sub(t5, bh_int(65248LL));
        t7 = bi_chr(1, (Value[]){t6});
        return t7;
    } else {
    }
    bh_line = 65;
    return L[0];
    return BH_NIL;
}

static Value fn2(Frame *up, Value *a) {    /* 函数 是字母，第 68 行 */
    struct { BhRoots h; Value v[3]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 3);
    L = fr.v; T = fr.v + 3;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15;
    L[0] = a[0];    /* 字 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    BH_SAFEPOINT();
    bh_line = 69;
    t0 = BH_FALSE;
    {
        t1 = bi_len(1, (Value[]){L[0]});
        if (bh_cmp(1, t1, bh_int(1LL))) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 70;
        BH_RETURN(fr, BH_FALSE);
    } else {
    }
    bh_line = 71;
    t0 = bi_ord(1, (Value[]){L[0]});
    if (!L[1].tag && or1) bh_modify_outer("码"); L[1] = t0;
    bh_line = 72;
    if (!L[2].tag && or2) bh_modify_outer("下标"); L[2] = bh_int(0LL);
    bh_line = 73;
    for (;;) {
        BH_SAFEPOINT();
        t0 = BH_FALSE;
        {
            bh_line = 73;
            if (L[2].tag) t1 = L[2]; else { or2 = 1; t1 = bh_chk(G[19], "下标"); }
            t2 = bh_chk(G[17], "字母范围");
            t3 = bi_len(1, (Value[]){t2});
            if (bh_cmp(2, t1, t3)) {
                t0 = BH_TRUE;
            }
        }
        if (!bh_truthy(t0)) break;
        bh_line = 74;
        {
            t5 = BH_FALSE;
            {
                if (L[1].tag) t6 = L[1]; else { or1 = 1; t6 = bh_chk(G[20], "码"); }
                t7 = bh_chk(G[17], "字母范围");
                if (L[2].tag) t8 = L[2]; else { or2 = 1; t8 = bh_chk(G[19], "下标"); }
                t9 = bh_index(t7, t8);
                if (bh_cmp(5, t6, t9)) {
                    t5 = BH_TRUE;
                }
            }
            if (bh_truthy(t5)) {
                t10 = BH_FALSE;
                {
                    bh_line = 74;
                    if (L[1].tag) t11 = L[1]; else { or1 = 1; t11 = bh_chk(G[20], "码"); }
                    t12 = bh_chk(G[17], "字母范围");
                    if (L[2].tag) t13 = L[2]; else { or2 = 1; t13 = bh_chk(G[19], "下标"); }
                    t14 = bh_add(t13, bh_int(1LL));
                    t15 = bh_index(t12, t14);
                    if (bh_cmp(4, t11, t15)) {
                        t10 = BH_TRUE;
                    }
                }
                t4 = t10;
            } else {
                t4 = t5;
            }
        }
        if (bh_truthy(t4)) {
            bh_line = 75;
            BH_RETURN(fr, BH_TRUE);
        } else {
        }
        bh_line = 76;
        if (L[2].tag) t4 = L[2]; else { or2 = 1; t4 = bh_chk(G[19], "下标"); }
        t5 = bh_add(t4, bh_int(2LL));
        if (!L[2].tag && or2) bh_modify_outer("下标"); L[2] = t5;
    }
    bh_line = 77;
    BH_RETURN(fr, BH_FALSE);
    BH_RETURN(fr, BH_NIL);
}

static Value fn3(Frame *up, Value *a) {    /* 函数 是数字，第 80 行 */
    Value L[1] = {{0}};
    Value t0, t1, t2, t3, t4, t5, t6, t7;
    L[0] = a[0];    /* 字 */
    char or0 = 0;
    bh_line = 81;
    {
        {
            t2 = BH_FALSE;
            {
                t3 = bi_len(1, (Value[]){L[0]});
                if (bh_cmp(0, t3, bh_int(1LL))) {
                    t2 = BH_TRUE;
                }
            }
            if (bh_truthy(t2)) {
                t4 = BH_FALSE;
                {
                    bh_line = 81;
                    t5 = bi_ord(1, (Value[]){L[0]});
                    if (bh_cmp(5, t5, bh_int(48LL))) {
                        t4 = BH_TRUE;
                    }
                }
                t1 = t4;
            } else {
                t1 = t2;
            }
        }
        if (bh_truthy(t1)) {
            t6 = BH_FALSE;
            {
                bh_line = 81;
                t7 = bi_ord(1, (Value[]){L[0]});
                if (bh_cmp(4, t7, bh_int(57LL))) {
                    t6 = BH_TRUE;
                }
            }
            t0 = t6;
        } else {
            t0 = t1;
        }
    }
    return t0;
    return BH_NIL;
}

static Value fn4(Frame *up, Value *a) {    /* 函数 名字字符，第 84 行 */
    struct { BhRoots h; Value v[1]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 1);
    L = fr.v; T = fr.v + 1;
    Value t0, t1, t2, t3, t4, t5, t6;
    L[0] = a[0];    /* 字 */
    char or0 = 0;
    BH_SAFEPOINT();
    bh_line = 85;
    {
        {
            t2 = bh_chk(G[22], "是字母");
            t3 = bh_call(t2, 1, (Value[]){L[0]}, "是字母");
            if (bh_truthy(t3)) {
                t1 = t3;
            } else {
                t4 = BH_FALSE;
                {
                    bh_line = 85;
                    if (bh_cmp(0, L[0], K[75])) {
                        t4 = BH_TRUE;
                    }
                }
                t1 = t4;
            }
        }
        if (bh_truthy(t1)) {
            t0 = t1;
        } else {
            bh_line = 85;
            t5 = bh_chk(G[23], "是数字");
            t6 = bh_call(t5, 1, (Value[]){L[0]}, "是数字");
            t0 = t6;
        }
    }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn5(Frame *up, Value *a) {    /* 函数 去空白，第 88 行 */
    struct { BhRoots h; Value v[3]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 3);
    L = fr.v; T = fr.v + 3;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11;
    L[0] = a[0];    /* 文 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    BH_SAFEPOINT();
    bh_line = 89;
    if (!L[1].tag && or1) bh_modify_outer("头"); L[1] = bh_int(0LL);
    bh_line = 90;
    t0 = bi_len(1, (Value[]){L[0]});
    if (!L[2].tag && or2) bh_modify_outer("尾"); L[2] = t0;
    bh_line = 91;
    for (;;) {
        BH_SAFEPOINT();
        {
            t1 = BH_FALSE;
            {
                bh_line = 91;
                if (L[1].tag) t2 = L[1]; else { or1 = 1; t2 = bh_chk(G[25], "头"); }
                if (L[2].tag) t3 = L[2]; else { or2 = 1; t3 = bh_chk(G[26], "尾"); }
                if (bh_cmp(2, t2, t3)) {
                    t1 = BH_TRUE;
                }
            }
            if (bh_truthy(t1)) {
                t4 = BH_FALSE;
                {
                    bh_line = 91;
                    t5 = bh_chk(G[10], "空白字符");
                    if (L[1].tag) t6 = L[1]; else { or1 = 1; t6 = bh_chk(G[25], "头"); }
                    t7 = bh_index(L[0], t6);
                    t8 = bi_find(2, (Value[]){t5, t7});
                    if (bh_cmp(5, t8, bh_int(0LL))) {
                        t4 = BH_TRUE;
                    }
                }
                t0 = t4;
            } else {
                t0 = t1;
            }
        }
        if (!bh_truthy(t0)) break;
        bh_line = 92;
        if (L[1].tag) t9 = L[1]; else { or1 = 1; t9 = bh_chk(G[25], "头"); }
        t10 = bh_add(t9, bh_int(1LL));
        if (!L[1].tag && or1) bh_modify_outer("头"); L[1] = t10;
    }
    bh_line = 93;
    for (;;) {
        BH_SAFEPOINT();
        {
            t1 = BH_FALSE;
            {
                bh_line = 93;
                if (L[2].tag) t2 = L[2]; else { or2 = 1; t2 = bh_chk(G[26], "尾"); }
                if (L[1].tag) t3 = L[1]; else { or1 = 1; t3 = bh_chk(G[25], "头"); }
                if (bh_cmp(3, t2, t3)) {
                    t1 = BH_TRUE;
                }
            }
            if (bh_truthy(t1)) {
                t4 = BH_FALSE;
                {
                    bh_line = 93;
                    t5 = bh_chk(G[10], "空白字符");
                    if (L[2].tag) t6 = L[2]; else { or2 = 1; t6 = bh_chk(G[26], "尾"); }
                    t7 = bh_sub(t6, bh_int(1LL));
                    t8 = bh_index(L[0], t7);
                    t9 = bi_find(2, (Value[]){t5, t8});
                    if (bh_cmp(5, t9, bh_int(0LL))) {
                        t4 = BH_TRUE;
                    }
                }
                t0 = t4;
            } else {
                t0 = t1;
            }
        }
        if (!bh_truthy(t0)) break;
        bh_line = 94;
        if (L[2].tag) t10 = L[2]; else { or2 = 1; t10 = bh_chk(G[26], "尾"); }
        t11 = bh_sub(t10, bh_int(1LL));
        if (!L[2].tag && or2) bh_modify_outer("尾"); L[2] = t11;
    }
    bh_line = 95;
    if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[25], "头"); }
    if (L[2].tag) t1 = L[2]; else { or2 = 1; t1 = bh_chk(G[26], "尾"); }
    if (L[1].tag) t2 = L[1]; else { or1 = 1; t2 = bh_chk(G[25], "头"); }
    t3 = bh_sub(t1, t2);
    t4 = bi_slice(3, (Value[]){L[0], t0, t3});
    BH_RETURN(fr, t4);
    BH_RETURN(fr, BH_NIL);
}

static Value fn6(Frame *up, Value *a) {    /* 函数 半角串，第 98 行 */
    struct { BhRoots h; Value v[6]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 6);
    L = fr.v; T = fr.v + 5;
    Value t0, t1, t2, t3, t4, t5, t6, t7;
    L[0] = a[0];    /* 文本 */
    char or0 = 0;
    L[1] = a[1];    /* 起 */
    char or1 = 0;
    L[2] = a[2];    /* 止 */
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    BH_SAFEPOINT();
    bh_line = 99;
    t0 = bh_list_make(0, NULL);
    if (!L[3].tag && or3) bh_modify_outer("片"); L[3] = t0;
    bh_line = 100;
    if (!L[4].tag && or4) bh_modify_outer("下标"); L[4] = L[1];
    bh_line = 101;
    for (;;) {
        BH_SAFEPOINT();
        t0 = BH_FALSE;
        {
            bh_line = 101;
            if (L[4].tag) t1 = L[4]; else { or4 = 1; t1 = bh_chk(G[19], "下标"); }
            if (bh_cmp(2, t1, L[2])) {
                t0 = BH_TRUE;
            }
        }
        if (!bh_truthy(t0)) break;
        bh_line = 102;
        if (L[3].tag) t2 = L[3]; else { or3 = 1; t2 = bh_chk(G[28], "片"); }
        T[0] = t2;
        t3 = bh_chk(G[21], "半角");
        if (L[4].tag) t4 = L[4]; else { or4 = 1; t4 = bh_chk(G[19], "下标"); }
        t5 = bh_index(L[0], t4);
        t6 = bh_call(t3, 1, (Value[]){t5}, "半角");
        t7 = bi_append(2, (Value[]){T[0], t6});
        bh_line = 103;
        if (L[4].tag) t2 = L[4]; else { or4 = 1; t2 = bh_chk(G[19], "下标"); }
        t3 = bh_add(t2, bh_int(1LL));
        if (!L[4].tag && or4) bh_modify_outer("下标"); L[4] = t3;
    }
    bh_line = 104;
    if (L[3].tag) t0 = L[3]; else { or3 = 1; t0 = bh_chk(G[28], "片"); }
    t1 = bi_join(1, (Value[]){t0});
    BH_RETURN(fr, t1);
    BH_RETURN(fr, BH_NIL);
}

static Value fn7(Frame *up, Value *a) {    /* 函数 去前导零，第 107 行 */
    struct { BhRoots h; Value v[2]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 2);
    L = fr.v; T = fr.v + 2;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9;
    L[0] = a[0];    /* 文 */
    char or0 = 0;
    char or1 = 0;
    BH_SAFEPOINT();
    bh_line = 108;
    if (!L[1].tag && or1) bh_modify_outer("头"); L[1] = bh_int(0LL);
    bh_line = 109;
    for (;;) {
        BH_SAFEPOINT();
        {
            t1 = BH_FALSE;
            {
                bh_line = 109;
                if (L[1].tag) t2 = L[1]; else { or1 = 1; t2 = bh_chk(G[25], "头"); }
                t3 = bi_len(1, (Value[]){L[0]});
                t4 = bh_sub(t3, bh_int(1LL));
                if (bh_cmp(2, t2, t4)) {
                    t1 = BH_TRUE;
                }
            }
            if (bh_truthy(t1)) {
                t5 = BH_FALSE;
                {
                    bh_line = 109;
                    if (L[1].tag) t6 = L[1]; else { or1 = 1; t6 = bh_chk(G[25], "头"); }
                    t7 = bh_index(L[0], t6);
                    if (bh_cmp(0, t7, K[76])) {
                        t5 = BH_TRUE;
                    }
                }
                t0 = t5;
            } else {
                t0 = t1;
            }
        }
        if (!bh_truthy(t0)) break;
        bh_line = 110;
        if (L[1].tag) t8 = L[1]; else { or1 = 1; t8 = bh_chk(G[25], "头"); }
        t9 = bh_add(t8, bh_int(1LL));
        if (!L[1].tag && or1) bh_modify_outer("头"); L[1] = t9;
    }
    bh_line = 111;
    if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[25], "头"); }
    t1 = bi_len(1, (Value[]){L[0]});
    if (L[1].tag) t2 = L[1]; else { or1 = 1; t2 = bh_chk(G[25], "头"); }
    t3 = bh_sub(t1, t2);
    t4 = bi_slice(3, (Value[]){L[0], t0, t3});
    BH_RETURN(fr, t4);
    BH_RETURN(fr, BH_NIL);
}

static Value fn8(Frame *up, Value *a) {    /* 函数 语法出错，第 114 行 */
    struct { BhRoots h; Value v[3]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 3);
    L = fr.v; T = fr.v + 3;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9;
    L[0] = a[0];    /* 消息 */
    char or0 = 0;
    L[1] = a[1];    /* 行 */
    char or1 = 0;
    char or2 = 0;
    BH_SAFEPOINT();
    bh_line = 116;
    t0 = bh_add(K[77], L[1]);
    t1 = bh_add(t0, K[78]);
    t2 = bh_add(t1, L[0]);
    t3 = bi_eprint(1, (Value[]){t2});
    bh_line = 117;
    {
        t1 = BH_FALSE;
        {
            if (bh_cmp(5, L[1], bh_int(1LL))) {
                t1 = BH_TRUE;
            }
        }
        if (bh_truthy(t1)) {
            t2 = BH_FALSE;
            {
                bh_line = 117;
                t3 = bh_chk(G[18], "源码行表");
                t4 = bi_len(1, (Value[]){t3});
                if (bh_cmp(4, L[1], t4)) {
                    t2 = BH_TRUE;
                }
            }
            t0 = t2;
        } else {
            t0 = t1;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 118;
        t5 = bh_chk(G[27], "去空白");
        t6 = bh_chk(G[18], "源码行表");
        t7 = bh_sub(L[1], bh_int(1LL));
        t8 = bh_index(t6, t7);
        t9 = bh_call(t5, 1, (Value[]){t8}, "去空白");
        if (!L[2].tag && or2) bh_modify_outer("这行"); L[2] = t9;
        bh_line = 119;
        t5 = BH_FALSE;
        {
            if (L[2].tag) t6 = L[2]; else { or2 = 1; t6 = bh_chk(G[31], "这行"); }
            if (bh_cmp(1, t6, K[79])) {
                t5 = BH_TRUE;
            }
        }
        if (bh_truthy(t5)) {
            bh_line = 120;
            if (L[2].tag) t7 = L[2]; else { or2 = 1; t7 = bh_chk(G[31], "这行"); }
            t8 = bh_add(K[80], t7);
            t9 = bi_eprint(1, (Value[]){t8});
        } else {
        }
    } else {
    }
    bh_line = 121;
    t0 = bi_exit(1, (Value[]){bh_int(1LL)});
    BH_RETURN(fr, BH_NIL);
}

static Value fn9(Frame *up, Value *a) {    /* 函数 空格提示，第 124 行 */
    struct { BhRoots h; Value v[5]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 5);
    L = fr.v; T = fr.v + 3;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14;
    L[0] = a[0];    /* 名字们 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    BH_SAFEPOINT();
    bh_line = 126;
    {
        T[0] = bh_iter(L[0]);
        int64_t n1;
        for (n1 = 0; n1 < bh_list_len(T[0]); n1++) {
            BH_SAFEPOINT();
            bh_line = 126;
            if (!L[1].tag && or1) bh_modify_outer("名字"); L[1] = bh_list_at(T[0], n1);
            bh_line = 127;
            {
                t0 = bh_chk(G[3], "长关键字");
                T[1] = bh_iter(t0);
                int64_t n2;
                for (n2 = 0; n2 < bh_list_len(T[1]); n2++) {
                    BH_SAFEPOINT();
                    bh_line = 127;
                    if (!L[2].tag && or2) bh_modify_outer("词"); L[2] = bh_list_at(T[1], n2);
                    bh_line = 128;
                    {
                        t2 = BH_FALSE;
                        {
                            if (L[1].tag) t3 = L[1]; else { or1 = 1; t3 = bh_chk(G[33], "名字"); }
                            if (L[2].tag) t4 = L[2]; else { or2 = 1; t4 = bh_chk(G[5], "词"); }
                            t5 = bi_find(2, (Value[]){t3, t4});
                            if (bh_cmp(5, t5, bh_int(0LL))) {
                                t2 = BH_TRUE;
                            }
                        }
                        if (bh_truthy(t2)) {
                            t6 = BH_FALSE;
                            {
                                bh_line = 128;
                                if (L[2].tag) t7 = L[2]; else { or2 = 1; t7 = bh_chk(G[5], "词"); }
                                if (L[1].tag) t8 = L[1]; else { or1 = 1; t8 = bh_chk(G[33], "名字"); }
                                if (bh_cmp(1, t7, t8)) {
                                    t6 = BH_TRUE;
                                }
                            }
                            t1 = t6;
                        } else {
                            t1 = t2;
                        }
                    }
                    if (bh_truthy(t1)) {
                        bh_line = 129;
                        if (L[1].tag) t9 = L[1]; else { or1 = 1; t9 = bh_chk(G[33], "名字"); }
                        t10 = bh_add(K[81], t9);
                        t11 = bh_add(t10, K[82]);
                        if (L[2].tag) t12 = L[2]; else { or2 = 1; t12 = bh_chk(G[5], "词"); }
                        t13 = bh_add(t11, t12);
                        t14 = bh_add(t13, K[83]);
                        BH_RETURN(fr, t14);
                    } else {
                    }
                }
            }
        }
    }
    bh_line = 130;
    BH_RETURN(fr, K[79]);
    BH_RETURN(fr, BH_NIL);
}

static Value fn10(Frame *up, Value *a) {    /* 函数 词法分析，第 133 行 */
    struct { BhRoots h; Value v[25]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 25);
    L = fr.v; T = fr.v + 22;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17, t18, t19, t20, t21, t22, t23, t24, t25, t26, t27, t28, t29, t30, t31, t32, t33, t34, t35, t36, t37, t38, t39, t40, t41, t42, t43, t44, t45, t46, t47, t48, t49, t50, t51, t52, t53, t54;
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    char or5 = 0;
    char or6 = 0;
    char or7 = 0;
    char or8 = 0;
    char or9 = 0;
    char or10 = 0;
    char or11 = 0;
    char or12 = 0;
    char or13 = 0;
    char or14 = 0;
    char or15 = 0;
    char or16 = 0;
    char or17 = 0;
    char or18 = 0;
    char or19 = 0;
    char or20 = 0;
    char or21 = 0;
    BH_SAFEPOINT();
    bh_line = 134;
    t0 = bh_list_make(0, NULL);
    if (!L[0].tag && or0) bh_modify_outer("记号表"); L[0] = t0;
    bh_line = 135;
    t0 = bh_list_make(1, (Value[]){bh_int(0LL)});
    if (!L[1].tag && or1) bh_modify_outer("缩进栈"); L[1] = t0;
    bh_line = 136;
    t0 = bh_list_make(0, NULL);
    if (!L[2].tag && or2) bh_modify_outer("括号栈"); L[2] = t0;
    bh_line = 137;
    if (!L[3].tag && or3) bh_modify_outer("行号"); L[3] = bh_int(0LL);
    bh_line = 138;
    {
        t0 = bh_chk(G[18], "源码行表");
        T[0] = bh_iter(t0);
        int64_t n1;
        for (n1 = 0; n1 < bh_list_len(T[0]); n1++) {
            BH_SAFEPOINT();
            bh_line = 138;
            if (!L[4].tag && or4) bh_modify_outer("文本"); L[4] = bh_list_at(T[0], n1);
            bh_line = 139;
            if (L[3].tag) t1 = L[3]; else { or3 = 1; t1 = bh_chk(G[35], "行号"); }
            t2 = bh_add(t1, bh_int(1LL));
            if (!L[3].tag && or3) bh_modify_outer("行号"); L[3] = t2;
            bh_line = 140;
            if (!L[5].tag && or5) bh_modify_outer("位置"); L[5] = bh_int(0LL);
            bh_line = 141;
            if (L[4].tag) t1 = L[4]; else { or4 = 1; t1 = bh_chk(G[36], "文本"); }
            t2 = bi_len(1, (Value[]){t1});
            if (!L[6].tag && or6) bh_modify_outer("总长"); L[6] = t2;
            bh_line = 143;
            t1 = BH_FALSE;
            {
                if (L[2].tag) t2 = L[2]; else { or2 = 1; t2 = bh_chk(G[37], "括号栈"); }
                t3 = bi_len(1, (Value[]){t2});
                if (bh_cmp(0, t3, bh_int(0LL))) {
                    t1 = BH_TRUE;
                }
            }
            if (bh_truthy(t1)) {
                bh_line = 145;
                if (!L[7].tag && or7) bh_modify_outer("宽度"); L[7] = bh_int(0LL);
                bh_line = 146;
                for (;;) {
                    BH_SAFEPOINT();
                    {
                        t5 = BH_FALSE;
                        {
                            bh_line = 146;
                            if (L[5].tag) t6 = L[5]; else { or5 = 1; t6 = bh_chk(G[38], "位置"); }
                            if (L[6].tag) t7 = L[6]; else { or6 = 1; t7 = bh_chk(G[39], "总长"); }
                            if (bh_cmp(2, t6, t7)) {
                                t5 = BH_TRUE;
                            }
                        }
                        if (bh_truthy(t5)) {
                            t8 = BH_FALSE;
                            {
                                bh_line = 146;
                                t9 = bh_chk(G[8], "缩进字符");
                                if (L[4].tag) t10 = L[4]; else { or4 = 1; t10 = bh_chk(G[36], "文本"); }
                                if (L[5].tag) t11 = L[5]; else { or5 = 1; t11 = bh_chk(G[38], "位置"); }
                                t12 = bh_index(t10, t11);
                                t13 = bi_find(2, (Value[]){t9, t12});
                                if (bh_cmp(5, t13, bh_int(0LL))) {
                                    t8 = BH_TRUE;
                                }
                            }
                            t4 = t8;
                        } else {
                            t4 = t5;
                        }
                    }
                    if (!bh_truthy(t4)) break;
                    bh_line = 147;
                    t14 = bh_chk(G[9], "缩进宽度");
                    t15 = bh_chk(G[8], "缩进字符");
                    if (L[4].tag) t16 = L[4]; else { or4 = 1; t16 = bh_chk(G[36], "文本"); }
                    if (L[5].tag) t17 = L[5]; else { or5 = 1; t17 = bh_chk(G[38], "位置"); }
                    t18 = bh_index(t16, t17);
                    t19 = bi_find(2, (Value[]){t15, t18});
                    t20 = bh_index(t14, t19);
                    if (L[7].tag) t21 = L[7]; else { or7 = 1; t21 = bh_chk(G[40], "宽度"); }
                    t22 = bh_add(t21, t20);
                    if (!L[7].tag && or7) bh_modify_outer("宽度"); L[7] = t22;
                    bh_line = 148;
                    if (L[5].tag) t14 = L[5]; else { or5 = 1; t14 = bh_chk(G[38], "位置"); }
                    t15 = bh_add(t14, bh_int(1LL));
                    if (!L[5].tag && or5) bh_modify_outer("位置"); L[5] = t15;
                }
                bh_line = 149;
                {
                    t5 = BH_FALSE;
                    {
                        if (L[5].tag) t6 = L[5]; else { or5 = 1; t6 = bh_chk(G[38], "位置"); }
                        if (L[6].tag) t7 = L[6]; else { or6 = 1; t7 = bh_chk(G[39], "总长"); }
                        if (bh_cmp(0, t6, t7)) {
                            t5 = BH_TRUE;
                        }
                    }
                    if (bh_truthy(t5)) {
                        t4 = t5;
                    } else {
                        t8 = BH_FALSE;
                        {
                            bh_line = 149;
                            t9 = bh_chk(G[21], "半角");
                            if (L[4].tag) t10 = L[4]; else { or4 = 1; t10 = bh_chk(G[36], "文本"); }
                            if (L[5].tag) t11 = L[5]; else { or5 = 1; t11 = bh_chk(G[38], "位置"); }
                            t12 = bh_index(t10, t11);
                            t13 = bh_call(t9, 1, (Value[]){t12}, "半角");
                            if (bh_cmp(0, t13, K[84])) {
                                t8 = BH_TRUE;
                            }
                        }
                        t4 = t8;
                    }
                }
                if (bh_truthy(t4)) {
                    bh_line = 150;
                    continue;
                } else {
                }
                bh_line = 151;
                t4 = BH_FALSE;
                {
                    if (L[7].tag) t5 = L[7]; else { or7 = 1; t5 = bh_chk(G[40], "宽度"); }
                    if (L[1].tag) t6 = L[1]; else { or1 = 1; t6 = bh_chk(G[41], "缩进栈"); }
                    t7 = bh_neg(bh_int(1LL));
                    t8 = bh_index(t6, t7);
                    if (bh_cmp(3, t5, t8)) {
                        t4 = BH_TRUE;
                    }
                }
                if (bh_truthy(t4)) {
                    bh_line = 152;
                    if (L[1].tag) t9 = L[1]; else { or1 = 1; t9 = bh_chk(G[41], "缩进栈"); }
                    if (L[7].tag) t10 = L[7]; else { or7 = 1; t10 = bh_chk(G[40], "宽度"); }
                    t11 = bi_append(2, (Value[]){t9, t10});
                    bh_line = 153;
                    if (L[0].tag) t9 = L[0]; else { or0 = 1; t9 = bh_chk(G[42], "记号表"); }
                    if (L[3].tag) t10 = L[3]; else { or3 = 1; t10 = bh_chk(G[35], "行号"); }
                    t11 = bh_list_make(4, (Value[]){K[85], BH_NIL, t10, K[79]});
                    t12 = bi_append(2, (Value[]){t9, t11});
                } else {
                    bh_line = 155;
                    for (;;) {
                        BH_SAFEPOINT();
                        t9 = BH_FALSE;
                        {
                            bh_line = 155;
                            if (L[7].tag) t10 = L[7]; else { or7 = 1; t10 = bh_chk(G[40], "宽度"); }
                            if (L[1].tag) t11 = L[1]; else { or1 = 1; t11 = bh_chk(G[41], "缩进栈"); }
                            t12 = bh_neg(bh_int(1LL));
                            t13 = bh_index(t11, t12);
                            if (bh_cmp(2, t10, t13)) {
                                t9 = BH_TRUE;
                            }
                        }
                        if (!bh_truthy(t9)) break;
                        bh_line = 156;
                        if (L[1].tag) t14 = L[1]; else { or1 = 1; t14 = bh_chk(G[41], "缩进栈"); }
                        t15 = bh_neg(bh_int(1LL));
                        t16 = bi_remove(2, (Value[]){t14, t15});
                        bh_line = 157;
                        if (L[0].tag) t14 = L[0]; else { or0 = 1; t14 = bh_chk(G[42], "记号表"); }
                        if (L[3].tag) t15 = L[3]; else { or3 = 1; t15 = bh_chk(G[35], "行号"); }
                        t16 = bh_list_make(4, (Value[]){K[86], BH_NIL, t15, K[79]});
                        t17 = bi_append(2, (Value[]){t14, t16});
                    }
                    bh_line = 158;
                    t9 = BH_FALSE;
                    {
                        if (L[7].tag) t10 = L[7]; else { or7 = 1; t10 = bh_chk(G[40], "宽度"); }
                        if (L[1].tag) t11 = L[1]; else { or1 = 1; t11 = bh_chk(G[41], "缩进栈"); }
                        t12 = bh_neg(bh_int(1LL));
                        t13 = bh_index(t11, t12);
                        if (bh_cmp(1, t10, t13)) {
                            t9 = BH_TRUE;
                        }
                    }
                    if (bh_truthy(t9)) {
                        bh_line = 159;
                        t14 = bh_chk(G[32], "语法出错");
                        if (L[3].tag) t15 = L[3]; else { or3 = 1; t15 = bh_chk(G[35], "行号"); }
                        t16 = bh_call(t14, 2, (Value[]){K[87], t15}, "语法出错");
                    } else {
                    }
                }
            } else {
            }
            bh_line = 161;
            for (;;) {
                BH_SAFEPOINT();
                t1 = BH_FALSE;
                {
                    bh_line = 161;
                    if (L[5].tag) t2 = L[5]; else { or5 = 1; t2 = bh_chk(G[38], "位置"); }
                    if (L[6].tag) t3 = L[6]; else { or6 = 1; t3 = bh_chk(G[39], "总长"); }
                    if (bh_cmp(2, t2, t3)) {
                        t1 = BH_TRUE;
                    }
                }
                if (!bh_truthy(t1)) break;
                bh_line = 162;
                if (L[4].tag) t4 = L[4]; else { or4 = 1; t4 = bh_chk(G[36], "文本"); }
                if (L[5].tag) t5 = L[5]; else { or5 = 1; t5 = bh_chk(G[38], "位置"); }
                t6 = bh_index(t4, t5);
                if (!L[8].tag && or8) bh_modify_outer("原字"); L[8] = t6;
                bh_line = 163;
                t4 = bh_chk(G[21], "半角");
                if (L[8].tag) t5 = L[8]; else { or8 = 1; t5 = bh_chk(G[43], "原字"); }
                t6 = bh_call(t4, 1, (Value[]){t5}, "半角");
                if (!L[9].tag && or9) bh_modify_outer("字"); L[9] = t6;
                bh_line = 165;
                {
                    t5 = BH_FALSE;
                    {
                        if (L[9].tag) t6 = L[9]; else { or9 = 1; t6 = bh_chk(G[44], "字"); }
                        if (bh_cmp(0, t6, K[54])) {
                            t5 = BH_TRUE;
                        }
                    }
                    if (bh_truthy(t5)) {
                        t4 = t5;
                    } else {
                        t7 = BH_FALSE;
                        {
                            bh_line = 165;
                            if (L[9].tag) t8 = L[9]; else { or9 = 1; t8 = bh_chk(G[44], "字"); }
                            if (bh_cmp(0, t8, K[55])) {
                                t7 = BH_TRUE;
                            }
                        }
                        t4 = t7;
                    }
                }
                if (bh_truthy(t4)) {
                    bh_line = 166;
                    if (L[5].tag) t9 = L[5]; else { or5 = 1; t9 = bh_chk(G[38], "位置"); }
                    t10 = bh_add(t9, bh_int(1LL));
                    if (!L[5].tag && or5) bh_modify_outer("位置"); L[5] = t10;
                } else {
                    t9 = BH_FALSE;
                    {
                        bh_line = 168;
                        if (L[9].tag) t10 = L[9]; else { or9 = 1; t10 = bh_chk(G[44], "字"); }
                        if (bh_cmp(0, t10, K[84])) {
                            t9 = BH_TRUE;
                        }
                    }
                    if (bh_truthy(t9)) {
                        bh_line = 169;
                        break;
                    } else {
                        {
                            t12 = BH_FALSE;
                            {
                                bh_line = 171;
                                t13 = bh_chk(G[11], "引号开");
                                if (L[8].tag) t14 = L[8]; else { or8 = 1; t14 = bh_chk(G[43], "原字"); }
                                t15 = bi_find(2, (Value[]){t13, t14});
                                if (bh_cmp(5, t15, bh_int(0LL))) {
                                    t12 = BH_TRUE;
                                }
                            }
                            if (bh_truthy(t12)) {
                                t11 = t12;
                            } else {
                                t16 = BH_FALSE;
                                {
                                    bh_line = 171;
                                    t17 = bh_chk(G[11], "引号开");
                                    if (L[9].tag) t18 = L[9]; else { or9 = 1; t18 = bh_chk(G[44], "字"); }
                                    t19 = bi_find(2, (Value[]){t17, t18});
                                    if (bh_cmp(5, t19, bh_int(0LL))) {
                                        t16 = BH_TRUE;
                                    }
                                }
                                t11 = t16;
                            }
                        }
                        if (bh_truthy(t11)) {
                            bh_line = 173;
                            t20 = bh_chk(G[11], "引号开");
                            if (L[8].tag) t21 = L[8]; else { or8 = 1; t21 = bh_chk(G[43], "原字"); }
                            t22 = bi_find(2, (Value[]){t20, t21});
                            if (!L[10].tag && or10) bh_modify_outer("号"); L[10] = t22;
                            bh_line = 174;
                            t20 = BH_FALSE;
                            {
                                if (L[10].tag) t21 = L[10]; else { or10 = 1; t21 = bh_chk(G[45], "号"); }
                                if (bh_cmp(2, t21, bh_int(0LL))) {
                                    t20 = BH_TRUE;
                                }
                            }
                            if (bh_truthy(t20)) {
                                bh_line = 175;
                                t22 = bh_chk(G[11], "引号开");
                                if (L[9].tag) t23 = L[9]; else { or9 = 1; t23 = bh_chk(G[44], "字"); }
                                t24 = bi_find(2, (Value[]){t22, t23});
                                if (!L[10].tag && or10) bh_modify_outer("号"); L[10] = t24;
                            } else {
                            }
                            bh_line = 176;
                            t20 = bh_chk(G[12], "引号关");
                            if (L[10].tag) t21 = L[10]; else { or10 = 1; t21 = bh_chk(G[45], "号"); }
                            t22 = bh_index(t20, t21);
                            if (!L[11].tag && or11) bh_modify_outer("结尾们"); L[11] = t22;
                            bh_line = 177;
                            t20 = bh_list_make(0, NULL);
                            if (!L[12].tag && or12) bh_modify_outer("内容"); L[12] = t20;
                            bh_line = 178;
                            if (L[5].tag) t20 = L[5]; else { or5 = 1; t20 = bh_chk(G[38], "位置"); }
                            t21 = bh_add(t20, bh_int(1LL));
                            if (!L[13].tag && or13) bh_modify_outer("指针"); L[13] = t21;
                            bh_line = 179;
                            for (;;) {
                                BH_SAFEPOINT();
                                {
                                    {
                                        t22 = BH_FALSE;
                                        {
                                            bh_line = 179;
                                            if (L[13].tag) t23 = L[13]; else { or13 = 1; t23 = bh_chk(G[46], "指针"); }
                                            if (L[6].tag) t24 = L[6]; else { or6 = 1; t24 = bh_chk(G[39], "总长"); }
                                            if (bh_cmp(2, t23, t24)) {
                                                t22 = BH_TRUE;
                                            }
                                        }
                                        if (bh_truthy(t22)) {
                                            t25 = BH_FALSE;
                                            {
                                                bh_line = 179;
                                                if (L[11].tag) t26 = L[11]; else { or11 = 1; t26 = bh_chk(G[47], "结尾们"); }
                                                if (L[4].tag) t27 = L[4]; else { or4 = 1; t27 = bh_chk(G[36], "文本"); }
                                                if (L[13].tag) t28 = L[13]; else { or13 = 1; t28 = bh_chk(G[46], "指针"); }
                                                t29 = bh_index(t27, t28);
                                                t30 = bi_find(2, (Value[]){t26, t29});
                                                if (bh_cmp(2, t30, bh_int(0LL))) {
                                                    t25 = BH_TRUE;
                                                }
                                            }
                                            t21 = t25;
                                        } else {
                                            t21 = t22;
                                        }
                                    }
                                    if (bh_truthy(t21)) {
                                        t31 = BH_FALSE;
                                        {
                                            bh_line = 179;
                                            if (L[11].tag) t32 = L[11]; else { or11 = 1; t32 = bh_chk(G[47], "结尾们"); }
                                            T[1] = t32;
                                            t33 = bh_chk(G[21], "半角");
                                            if (L[4].tag) t34 = L[4]; else { or4 = 1; t34 = bh_chk(G[36], "文本"); }
                                            if (L[13].tag) t35 = L[13]; else { or13 = 1; t35 = bh_chk(G[46], "指针"); }
                                            t36 = bh_index(t34, t35);
                                            t37 = bh_call(t33, 1, (Value[]){t36}, "半角");
                                            t38 = bi_find(2, (Value[]){T[1], t37});
                                            if (bh_cmp(2, t38, bh_int(0LL))) {
                                                t31 = BH_TRUE;
                                            }
                                        }
                                        t20 = t31;
                                    } else {
                                        t20 = t21;
                                    }
                                }
                                if (!bh_truthy(t20)) break;
                                bh_line = 180;
                                {
                                    t40 = BH_FALSE;
                                    {
                                        if (L[4].tag) t41 = L[4]; else { or4 = 1; t41 = bh_chk(G[36], "文本"); }
                                        if (L[13].tag) t42 = L[13]; else { or13 = 1; t42 = bh_chk(G[46], "指针"); }
                                        t43 = bh_index(t41, t42);
                                        if (bh_cmp(0, t43, K[88])) {
                                            t40 = BH_TRUE;
                                        }
                                    }
                                    if (bh_truthy(t40)) {
                                        t44 = BH_FALSE;
                                        {
                                            bh_line = 180;
                                            if (L[13].tag) t45 = L[13]; else { or13 = 1; t45 = bh_chk(G[46], "指针"); }
                                            t46 = bh_add(t45, bh_int(1LL));
                                            if (L[6].tag) t47 = L[6]; else { or6 = 1; t47 = bh_chk(G[39], "总长"); }
                                            if (bh_cmp(2, t46, t47)) {
                                                t44 = BH_TRUE;
                                            }
                                        }
                                        t39 = t44;
                                    } else {
                                        t39 = t40;
                                    }
                                }
                                if (bh_truthy(t39)) {
                                    bh_line = 181;
                                    if (L[13].tag) t48 = L[13]; else { or13 = 1; t48 = bh_chk(G[46], "指针"); }
                                    t49 = bh_add(t48, bh_int(1LL));
                                    if (!L[13].tag && or13) bh_modify_outer("指针"); L[13] = t49;
                                    bh_line = 182;
                                    if (L[4].tag) t48 = L[4]; else { or4 = 1; t48 = bh_chk(G[36], "文本"); }
                                    if (L[13].tag) t49 = L[13]; else { or13 = 1; t49 = bh_chk(G[46], "指针"); }
                                    t50 = bh_index(t48, t49);
                                    if (!L[14].tag && or14) bh_modify_outer("转义"); L[14] = t50;
                                    bh_line = 183;
                                    t48 = BH_FALSE;
                                    {
                                        if (L[14].tag) t49 = L[14]; else { or14 = 1; t49 = bh_chk(G[48], "转义"); }
                                        if (bh_cmp(0, t49, K[89])) {
                                            t48 = BH_TRUE;
                                        }
                                    }
                                    if (bh_truthy(t48)) {
                                        bh_line = 184;
                                        if (L[12].tag) t50 = L[12]; else { or12 = 1; t50 = bh_chk(G[49], "内容"); }
                                        t51 = bi_append(2, (Value[]){t50, K[56]});
                                    } else {
                                        t50 = BH_FALSE;
                                        {
                                            bh_line = 185;
                                            if (L[14].tag) t51 = L[14]; else { or14 = 1; t51 = bh_chk(G[48], "转义"); }
                                            if (bh_cmp(0, t51, K[90])) {
                                                t50 = BH_TRUE;
                                            }
                                        }
                                        if (bh_truthy(t50)) {
                                            bh_line = 186;
                                            if (L[12].tag) t52 = L[12]; else { or12 = 1; t52 = bh_chk(G[49], "内容"); }
                                            t53 = bi_append(2, (Value[]){t52, K[55]});
                                        } else {
                                            bh_line = 188;
                                            if (L[12].tag) t52 = L[12]; else { or12 = 1; t52 = bh_chk(G[49], "内容"); }
                                            if (L[14].tag) t53 = L[14]; else { or14 = 1; t53 = bh_chk(G[48], "转义"); }
                                            t54 = bi_append(2, (Value[]){t52, t53});
                                        }
                                    }
                                } else {
                                    bh_line = 190;
                                    if (L[12].tag) t48 = L[12]; else { or12 = 1; t48 = bh_chk(G[49], "内容"); }
                                    if (L[4].tag) t49 = L[4]; else { or4 = 1; t49 = bh_chk(G[36], "文本"); }
                                    if (L[13].tag) t50 = L[13]; else { or13 = 1; t50 = bh_chk(G[46], "指针"); }
                                    t51 = bh_index(t49, t50);
                                    t52 = bi_append(2, (Value[]){t48, t51});
                                }
                                bh_line = 191;
                                if (L[13].tag) t39 = L[13]; else { or13 = 1; t39 = bh_chk(G[46], "指针"); }
                                t40 = bh_add(t39, bh_int(1LL));
                                if (!L[13].tag && or13) bh_modify_outer("指针"); L[13] = t40;
                            }
                            bh_line = 192;
                            t20 = BH_FALSE;
                            {
                                if (L[13].tag) t21 = L[13]; else { or13 = 1; t21 = bh_chk(G[46], "指针"); }
                                if (L[6].tag) t22 = L[6]; else { or6 = 1; t22 = bh_chk(G[39], "总长"); }
                                if (bh_cmp(5, t21, t22)) {
                                    t20 = BH_TRUE;
                                }
                            }
                            if (bh_truthy(t20)) {
                                bh_line = 193;
                                t23 = bh_chk(G[32], "语法出错");
                                if (L[8].tag) t24 = L[8]; else { or8 = 1; t24 = bh_chk(G[43], "原字"); }
                                t25 = bh_add(K[91], t24);
                                t26 = bh_add(t25, K[92]);
                                if (L[3].tag) t27 = L[3]; else { or3 = 1; t27 = bh_chk(G[35], "行号"); }
                                t28 = bh_call(t23, 2, (Value[]){t26, t27}, "语法出错");
                            } else {
                            }
                            bh_line = 194;
                            if (L[0].tag) t20 = L[0]; else { or0 = 1; t20 = bh_chk(G[42], "记号表"); }
                            if (L[12].tag) t21 = L[12]; else { or12 = 1; t21 = bh_chk(G[49], "内容"); }
                            t22 = bi_join(1, (Value[]){t21});
                            if (L[3].tag) t23 = L[3]; else { or3 = 1; t23 = bh_chk(G[35], "行号"); }
                            if (L[4].tag) t24 = L[4]; else { or4 = 1; t24 = bh_chk(G[36], "文本"); }
                            if (L[5].tag) t25 = L[5]; else { or5 = 1; t25 = bh_chk(G[38], "位置"); }
                            if (L[13].tag) t26 = L[13]; else { or13 = 1; t26 = bh_chk(G[46], "指针"); }
                            t27 = bh_add(t26, bh_int(1LL));
                            if (L[5].tag) t28 = L[5]; else { or5 = 1; t28 = bh_chk(G[38], "位置"); }
                            t29 = bh_sub(t27, t28);
                            t30 = bi_slice(3, (Value[]){t24, t25, t29});
                            t31 = bh_list_make(4, (Value[]){K[93], t22, t23, t30});
                            t32 = bi_append(2, (Value[]){t20, t31});
                            bh_line = 195;
                            if (L[13].tag) t20 = L[13]; else { or13 = 1; t20 = bh_chk(G[46], "指针"); }
                            t21 = bh_add(t20, bh_int(1LL));
                            if (!L[5].tag && or5) bh_modify_outer("位置"); L[5] = t21;
                        } else {
                            bh_line = 197;
                            t20 = bh_chk(G[23], "是数字");
                            if (L[9].tag) t21 = L[9]; else { or9 = 1; t21 = bh_chk(G[44], "字"); }
                            t22 = bh_call(t20, 1, (Value[]){t21}, "是数字");
                            if (bh_truthy(t22)) {
                                bh_line = 198;
                                if (L[5].tag) t23 = L[5]; else { or5 = 1; t23 = bh_chk(G[38], "位置"); }
                                if (!L[13].tag && or13) bh_modify_outer("指针"); L[13] = t23;
                                bh_line = 199;
                                for (;;) {
                                    BH_SAFEPOINT();
                                    {
                                        t24 = BH_FALSE;
                                        {
                                            bh_line = 199;
                                            if (L[13].tag) t25 = L[13]; else { or13 = 1; t25 = bh_chk(G[46], "指针"); }
                                            if (L[6].tag) t26 = L[6]; else { or6 = 1; t26 = bh_chk(G[39], "总长"); }
                                            if (bh_cmp(2, t25, t26)) {
                                                t24 = BH_TRUE;
                                            }
                                        }
                                        if (bh_truthy(t24)) {
                                            bh_line = 199;
                                            t27 = bh_chk(G[23], "是数字");
                                            T[1] = t27;
                                            t28 = bh_chk(G[21], "半角");
                                            if (L[4].tag) t29 = L[4]; else { or4 = 1; t29 = bh_chk(G[36], "文本"); }
                                            if (L[13].tag) t30 = L[13]; else { or13 = 1; t30 = bh_chk(G[46], "指针"); }
                                            t31 = bh_index(t29, t30);
                                            t32 = bh_call(t28, 1, (Value[]){t31}, "半角");
                                            t33 = bh_call(T[1], 1, (Value[]){t32}, "是数字");
                                            t23 = t33;
                                        } else {
                                            t23 = t24;
                                        }
                                    }
                                    if (!bh_truthy(t23)) break;
                                    bh_line = 200;
                                    if (L[13].tag) t34 = L[13]; else { or13 = 1; t34 = bh_chk(G[46], "指针"); }
                                    t35 = bh_add(t34, bh_int(1LL));
                                    if (!L[13].tag && or13) bh_modify_outer("指针"); L[13] = t35;
                                }
                                bh_line = 201;
                                t23 = bh_chk(G[29], "半角串");
                                if (L[4].tag) t24 = L[4]; else { or4 = 1; t24 = bh_chk(G[36], "文本"); }
                                if (L[5].tag) t25 = L[5]; else { or5 = 1; t25 = bh_chk(G[38], "位置"); }
                                if (L[13].tag) t26 = L[13]; else { or13 = 1; t26 = bh_chk(G[46], "指针"); }
                                t27 = bh_call(t23, 3, (Value[]){t24, t25, t26}, "半角串");
                                if (!L[15].tag && or15) bh_modify_outer("数值"); L[15] = t27;
                                bh_line = 203;
                                {
                                    {
                                        t25 = BH_FALSE;
                                        {
                                            if (L[13].tag) t26 = L[13]; else { or13 = 1; t26 = bh_chk(G[46], "指针"); }
                                            t27 = bh_add(t26, bh_int(1LL));
                                            if (L[6].tag) t28 = L[6]; else { or6 = 1; t28 = bh_chk(G[39], "总长"); }
                                            if (bh_cmp(2, t27, t28)) {
                                                t25 = BH_TRUE;
                                            }
                                        }
                                        if (bh_truthy(t25)) {
                                            t29 = BH_FALSE;
                                            {
                                                bh_line = 203;
                                                if (L[4].tag) t30 = L[4]; else { or4 = 1; t30 = bh_chk(G[36], "文本"); }
                                                if (L[13].tag) t31 = L[13]; else { or13 = 1; t31 = bh_chk(G[46], "指针"); }
                                                t32 = bh_index(t30, t31);
                                                t33 = bi_find(2, (Value[]){K[94], t32});
                                                if (bh_cmp(5, t33, bh_int(0LL))) {
                                                    t29 = BH_TRUE;
                                                }
                                            }
                                            t24 = t29;
                                        } else {
                                            t24 = t25;
                                        }
                                    }
                                    if (bh_truthy(t24)) {
                                        bh_line = 203;
                                        t34 = bh_chk(G[23], "是数字");
                                        T[1] = t34;
                                        t35 = bh_chk(G[21], "半角");
                                        if (L[4].tag) t36 = L[4]; else { or4 = 1; t36 = bh_chk(G[36], "文本"); }
                                        if (L[13].tag) t37 = L[13]; else { or13 = 1; t37 = bh_chk(G[46], "指针"); }
                                        t38 = bh_add(t37, bh_int(1LL));
                                        t39 = bh_index(t36, t38);
                                        t40 = bh_call(t35, 1, (Value[]){t39}, "半角");
                                        t41 = bh_call(T[1], 1, (Value[]){t40}, "是数字");
                                        t23 = t41;
                                    } else {
                                        t23 = t24;
                                    }
                                }
                                if (bh_truthy(t23)) {
                                    bh_line = 204;
                                    if (L[13].tag) t42 = L[13]; else { or13 = 1; t42 = bh_chk(G[46], "指针"); }
                                    t43 = bh_add(t42, bh_int(1LL));
                                    if (!L[16].tag && or16) bh_modify_outer("尾"); L[16] = t43;
                                    bh_line = 205;
                                    for (;;) {
                                        BH_SAFEPOINT();
                                        {
                                            t43 = BH_FALSE;
                                            {
                                                bh_line = 205;
                                                if (L[16].tag) t44 = L[16]; else { or16 = 1; t44 = bh_chk(G[26], "尾"); }
                                                if (L[6].tag) t45 = L[6]; else { or6 = 1; t45 = bh_chk(G[39], "总长"); }
                                                if (bh_cmp(2, t44, t45)) {
                                                    t43 = BH_TRUE;
                                                }
                                            }
                                            if (bh_truthy(t43)) {
                                                bh_line = 205;
                                                t46 = bh_chk(G[23], "是数字");
                                                T[2] = t46;
                                                t47 = bh_chk(G[21], "半角");
                                                if (L[4].tag) t48 = L[4]; else { or4 = 1; t48 = bh_chk(G[36], "文本"); }
                                                if (L[16].tag) t49 = L[16]; else { or16 = 1; t49 = bh_chk(G[26], "尾"); }
                                                t50 = bh_index(t48, t49);
                                                t51 = bh_call(t47, 1, (Value[]){t50}, "半角");
                                                t52 = bh_call(T[2], 1, (Value[]){t51}, "是数字");
                                                t42 = t52;
                                            } else {
                                                t42 = t43;
                                            }
                                        }
                                        if (!bh_truthy(t42)) break;
                                        bh_line = 206;
                                        if (L[16].tag) t53 = L[16]; else { or16 = 1; t53 = bh_chk(G[26], "尾"); }
                                        t54 = bh_add(t53, bh_int(1LL));
                                        if (!L[16].tag && or16) bh_modify_outer("尾"); L[16] = t54;
                                    }
                                    bh_line = 207;
                                    if (L[15].tag) t42 = L[15]; else { or15 = 1; t42 = bh_chk(G[50], "数值"); }
                                    t43 = bh_add(t42, K[95]);
                                    T[2] = t43;
                                    t44 = bh_chk(G[29], "半角串");
                                    if (L[4].tag) t45 = L[4]; else { or4 = 1; t45 = bh_chk(G[36], "文本"); }
                                    if (L[13].tag) t46 = L[13]; else { or13 = 1; t46 = bh_chk(G[46], "指针"); }
                                    t47 = bh_add(t46, bh_int(1LL));
                                    if (L[16].tag) t48 = L[16]; else { or16 = 1; t48 = bh_chk(G[26], "尾"); }
                                    t49 = bh_call(t44, 3, (Value[]){t45, t47, t48}, "半角串");
                                    t50 = bh_add(T[2], t49);
                                    if (!L[15].tag && or15) bh_modify_outer("数值"); L[15] = t50;
                                    bh_line = 208;
                                    if (L[16].tag) t42 = L[16]; else { or16 = 1; t42 = bh_chk(G[26], "尾"); }
                                    if (!L[13].tag && or13) bh_modify_outer("指针"); L[13] = t42;
                                } else {
                                    bh_line = 210;
                                    t42 = bh_chk(G[30], "去前导零");
                                    if (L[15].tag) t43 = L[15]; else { or15 = 1; t43 = bh_chk(G[50], "数值"); }
                                    t44 = bh_call(t42, 1, (Value[]){t43}, "去前导零");
                                    if (!L[15].tag && or15) bh_modify_outer("数值"); L[15] = t44;
                                }
                                bh_line = 211;
                                if (L[0].tag) t23 = L[0]; else { or0 = 1; t23 = bh_chk(G[42], "记号表"); }
                                if (L[15].tag) t24 = L[15]; else { or15 = 1; t24 = bh_chk(G[50], "数值"); }
                                if (L[3].tag) t25 = L[3]; else { or3 = 1; t25 = bh_chk(G[35], "行号"); }
                                if (L[4].tag) t26 = L[4]; else { or4 = 1; t26 = bh_chk(G[36], "文本"); }
                                if (L[5].tag) t27 = L[5]; else { or5 = 1; t27 = bh_chk(G[38], "位置"); }
                                if (L[13].tag) t28 = L[13]; else { or13 = 1; t28 = bh_chk(G[46], "指针"); }
                                if (L[5].tag) t29 = L[5]; else { or5 = 1; t29 = bh_chk(G[38], "位置"); }
                                t30 = bh_sub(t28, t29);
                                t31 = bi_slice(3, (Value[]){t26, t27, t30});
                                t32 = bh_list_make(4, (Value[]){K[96], t24, t25, t31});
                                t33 = bi_append(2, (Value[]){t23, t32});
                                bh_line = 212;
                                if (L[13].tag) t23 = L[13]; else { or13 = 1; t23 = bh_chk(G[46], "指针"); }
                                if (!L[5].tag && or5) bh_modify_outer("位置"); L[5] = t23;
                            } else {
                                {
                                    bh_line = 214;
                                    t24 = bh_chk(G[22], "是字母");
                                    if (L[9].tag) t25 = L[9]; else { or9 = 1; t25 = bh_chk(G[44], "字"); }
                                    t26 = bh_call(t24, 1, (Value[]){t25}, "是字母");
                                    if (bh_truthy(t26)) {
                                        t23 = t26;
                                    } else {
                                        t27 = BH_FALSE;
                                        {
                                            bh_line = 214;
                                            if (L[9].tag) t28 = L[9]; else { or9 = 1; t28 = bh_chk(G[44], "字"); }
                                            if (bh_cmp(0, t28, K[75])) {
                                                t27 = BH_TRUE;
                                            }
                                        }
                                        t23 = t27;
                                    }
                                }
                                if (bh_truthy(t23)) {
                                    bh_line = 216;
                                    if (L[5].tag) t29 = L[5]; else { or5 = 1; t29 = bh_chk(G[38], "位置"); }
                                    if (!L[13].tag && or13) bh_modify_outer("指针"); L[13] = t29;
                                    bh_line = 217;
                                    for (;;) {
                                        BH_SAFEPOINT();
                                        {
                                            t30 = BH_FALSE;
                                            {
                                                bh_line = 217;
                                                if (L[13].tag) t31 = L[13]; else { or13 = 1; t31 = bh_chk(G[46], "指针"); }
                                                if (L[6].tag) t32 = L[6]; else { or6 = 1; t32 = bh_chk(G[39], "总长"); }
                                                if (bh_cmp(2, t31, t32)) {
                                                    t30 = BH_TRUE;
                                                }
                                            }
                                            if (bh_truthy(t30)) {
                                                bh_line = 217;
                                                t33 = bh_chk(G[24], "名字字符");
                                                T[1] = t33;
                                                t34 = bh_chk(G[21], "半角");
                                                if (L[4].tag) t35 = L[4]; else { or4 = 1; t35 = bh_chk(G[36], "文本"); }
                                                if (L[13].tag) t36 = L[13]; else { or13 = 1; t36 = bh_chk(G[46], "指针"); }
                                                t37 = bh_index(t35, t36);
                                                t38 = bh_call(t34, 1, (Value[]){t37}, "半角");
                                                t39 = bh_call(T[1], 1, (Value[]){t38}, "名字字符");
                                                t29 = t39;
                                            } else {
                                                t29 = t30;
                                            }
                                        }
                                        if (!bh_truthy(t29)) break;
                                        bh_line = 218;
                                        if (L[13].tag) t40 = L[13]; else { or13 = 1; t40 = bh_chk(G[46], "指针"); }
                                        t41 = bh_add(t40, bh_int(1LL));
                                        if (!L[13].tag && or13) bh_modify_outer("指针"); L[13] = t41;
                                    }
                                    bh_line = 219;
                                    t29 = bh_chk(G[29], "半角串");
                                    if (L[4].tag) t30 = L[4]; else { or4 = 1; t30 = bh_chk(G[36], "文本"); }
                                    if (L[5].tag) t31 = L[5]; else { or5 = 1; t31 = bh_chk(G[38], "位置"); }
                                    if (L[13].tag) t32 = L[13]; else { or13 = 1; t32 = bh_chk(G[46], "指针"); }
                                    t33 = bh_call(t29, 3, (Value[]){t30, t31, t32}, "半角串");
                                    if (!L[17].tag && or17) bh_modify_outer("词"); L[17] = t33;
                                    bh_line = 220;
                                    t29 = bh_chk(G[1], "文字运算符");
                                    if (L[17].tag) t30 = L[17]; else { or17 = 1; t30 = bh_chk(G[5], "词"); }
                                    t31 = bi_find(2, (Value[]){t29, t30});
                                    if (!L[10].tag && or10) bh_modify_outer("号"); L[10] = t31;
                                    bh_line = 221;
                                    t29 = BH_FALSE;
                                    {
                                        if (L[10].tag) t30 = L[10]; else { or10 = 1; t30 = bh_chk(G[45], "号"); }
                                        if (bh_cmp(5, t30, bh_int(0LL))) {
                                            t29 = BH_TRUE;
                                        }
                                    }
                                    if (bh_truthy(t29)) {
                                        bh_line = 222;
                                        if (L[0].tag) t31 = L[0]; else { or0 = 1; t31 = bh_chk(G[42], "记号表"); }
                                        t32 = bh_chk(G[2], "对应符号");
                                        if (L[10].tag) t33 = L[10]; else { or10 = 1; t33 = bh_chk(G[45], "号"); }
                                        t34 = bh_index(t32, t33);
                                        if (L[3].tag) t35 = L[3]; else { or3 = 1; t35 = bh_chk(G[35], "行号"); }
                                        if (L[17].tag) t36 = L[17]; else { or17 = 1; t36 = bh_chk(G[5], "词"); }
                                        t37 = bh_list_make(4, (Value[]){K[97], t34, t35, t36});
                                        t38 = bi_append(2, (Value[]){t31, t37});
                                    } else {
                                        t31 = BH_FALSE;
                                        {
                                            bh_line = 223;
                                            t32 = bh_chk(G[0], "关键字表");
                                            if (L[17].tag) t33 = L[17]; else { or17 = 1; t33 = bh_chk(G[5], "词"); }
                                            t34 = bi_find(2, (Value[]){t32, t33});
                                            if (bh_cmp(5, t34, bh_int(0LL))) {
                                                t31 = BH_TRUE;
                                            }
                                        }
                                        if (bh_truthy(t31)) {
                                            bh_line = 224;
                                            if (L[0].tag) t35 = L[0]; else { or0 = 1; t35 = bh_chk(G[42], "记号表"); }
                                            if (L[17].tag) t36 = L[17]; else { or17 = 1; t36 = bh_chk(G[5], "词"); }
                                            if (L[3].tag) t37 = L[3]; else { or3 = 1; t37 = bh_chk(G[35], "行号"); }
                                            if (L[17].tag) t38 = L[17]; else { or17 = 1; t38 = bh_chk(G[5], "词"); }
                                            t39 = bh_list_make(4, (Value[]){K[98], t36, t37, t38});
                                            t40 = bi_append(2, (Value[]){t35, t39});
                                        } else {
                                            bh_line = 226;
                                            if (L[0].tag) t35 = L[0]; else { or0 = 1; t35 = bh_chk(G[42], "记号表"); }
                                            if (L[17].tag) t36 = L[17]; else { or17 = 1; t36 = bh_chk(G[5], "词"); }
                                            if (L[3].tag) t37 = L[3]; else { or3 = 1; t37 = bh_chk(G[35], "行号"); }
                                            if (L[17].tag) t38 = L[17]; else { or17 = 1; t38 = bh_chk(G[5], "词"); }
                                            t39 = bh_list_make(4, (Value[]){K[99], t36, t37, t38});
                                            t40 = bi_append(2, (Value[]){t35, t39});
                                        }
                                    }
                                    bh_line = 227;
                                    if (L[13].tag) t29 = L[13]; else { or13 = 1; t29 = bh_chk(G[46], "指针"); }
                                    if (!L[5].tag && or5) bh_modify_outer("位置"); L[5] = t29;
                                } else {
                                    bh_line = 230;
                                    if (!L[18].tag && or18) bh_modify_outer("后一个"); L[18] = K[79];
                                    bh_line = 231;
                                    t29 = BH_FALSE;
                                    {
                                        if (L[5].tag) t30 = L[5]; else { or5 = 1; t30 = bh_chk(G[38], "位置"); }
                                        t31 = bh_add(t30, bh_int(1LL));
                                        if (L[6].tag) t32 = L[6]; else { or6 = 1; t32 = bh_chk(G[39], "总长"); }
                                        if (bh_cmp(2, t31, t32)) {
                                            t29 = BH_TRUE;
                                        }
                                    }
                                    if (bh_truthy(t29)) {
                                        bh_line = 232;
                                        t33 = bh_chk(G[21], "半角");
                                        if (L[4].tag) t34 = L[4]; else { or4 = 1; t34 = bh_chk(G[36], "文本"); }
                                        if (L[5].tag) t35 = L[5]; else { or5 = 1; t35 = bh_chk(G[38], "位置"); }
                                        t36 = bh_add(t35, bh_int(1LL));
                                        t37 = bh_index(t34, t36);
                                        t38 = bh_call(t33, 1, (Value[]){t37}, "半角");
                                        if (!L[18].tag && or18) bh_modify_outer("后一个"); L[18] = t38;
                                    } else {
                                    }
                                    bh_line = 233;
                                    t29 = BH_FALSE;
                                    {
                                        if (L[9].tag) t30 = L[9]; else { or9 = 1; t30 = bh_chk(G[44], "字"); }
                                        t31 = bi_len(1, (Value[]){t30});
                                        if (bh_cmp(0, t31, bh_int(2LL))) {
                                            t29 = BH_TRUE;
                                        }
                                    }
                                    if (bh_truthy(t29)) {
                                        bh_line = 234;
                                        if (L[9].tag) t32 = L[9]; else { or9 = 1; t32 = bh_chk(G[44], "字"); }
                                        if (!L[19].tag && or19) bh_modify_outer("符号"); L[19] = t32;
                                        bh_line = 235;
                                        if (!L[20].tag && or20) bh_modify_outer("占"); L[20] = bh_int(1LL);
                                    } else {
                                        t32 = BH_FALSE;
                                        {
                                            bh_line = 236;
                                            t33 = bh_chk(G[13], "双字符号");
                                            if (L[9].tag) t34 = L[9]; else { or9 = 1; t34 = bh_chk(G[44], "字"); }
                                            if (L[18].tag) t35 = L[18]; else { or18 = 1; t35 = bh_chk(G[51], "后一个"); }
                                            t36 = bh_add(t34, t35);
                                            t37 = bi_find(2, (Value[]){t33, t36});
                                            if (bh_cmp(5, t37, bh_int(0LL))) {
                                                t32 = BH_TRUE;
                                            }
                                        }
                                        if (bh_truthy(t32)) {
                                            bh_line = 237;
                                            if (L[9].tag) t38 = L[9]; else { or9 = 1; t38 = bh_chk(G[44], "字"); }
                                            if (L[18].tag) t39 = L[18]; else { or18 = 1; t39 = bh_chk(G[51], "后一个"); }
                                            t40 = bh_add(t38, t39);
                                            if (!L[19].tag && or19) bh_modify_outer("符号"); L[19] = t40;
                                            bh_line = 238;
                                            if (!L[20].tag && or20) bh_modify_outer("占"); L[20] = bh_int(2LL);
                                        } else {
                                            t38 = BH_FALSE;
                                            {
                                                bh_line = 239;
                                                t39 = bh_chk(G[14], "单字符号");
                                                if (L[9].tag) t40 = L[9]; else { or9 = 1; t40 = bh_chk(G[44], "字"); }
                                                t41 = bi_find(2, (Value[]){t39, t40});
                                                if (bh_cmp(5, t41, bh_int(0LL))) {
                                                    t38 = BH_TRUE;
                                                }
                                            }
                                            if (bh_truthy(t38)) {
                                                bh_line = 240;
                                                if (L[9].tag) t42 = L[9]; else { or9 = 1; t42 = bh_chk(G[44], "字"); }
                                                if (!L[19].tag && or19) bh_modify_outer("符号"); L[19] = t42;
                                                bh_line = 241;
                                                if (!L[20].tag && or20) bh_modify_outer("占"); L[20] = bh_int(1LL);
                                            } else {
                                                bh_line = 243;
                                                t42 = bh_chk(G[32], "语法出错");
                                                if (L[8].tag) t43 = L[8]; else { or8 = 1; t43 = bh_chk(G[43], "原字"); }
                                                t44 = bh_add(K[100], t43);
                                                t45 = bh_add(t44, K[67]);
                                                if (L[3].tag) t46 = L[3]; else { or3 = 1; t46 = bh_chk(G[35], "行号"); }
                                                t47 = bh_call(t42, 2, (Value[]){t45, t46}, "语法出错");
                                            }
                                        }
                                    }
                                    bh_line = 244;
                                    {
                                        t30 = BH_FALSE;
                                        {
                                            if (L[19].tag) t31 = L[19]; else { or19 = 1; t31 = bh_chk(G[52], "符号"); }
                                            if (bh_cmp(0, t31, K[101])) {
                                                t30 = BH_TRUE;
                                            }
                                        }
                                        if (bh_truthy(t30)) {
                                            t29 = t30;
                                        } else {
                                            t32 = BH_FALSE;
                                            {
                                                bh_line = 244;
                                                if (L[19].tag) t33 = L[19]; else { or19 = 1; t33 = bh_chk(G[52], "符号"); }
                                                if (bh_cmp(0, t33, K[49])) {
                                                    t32 = BH_TRUE;
                                                }
                                            }
                                            t29 = t32;
                                        }
                                    }
                                    if (bh_truthy(t29)) {
                                        bh_line = 245;
                                        if (L[2].tag) t34 = L[2]; else { or2 = 1; t34 = bh_chk(G[37], "括号栈"); }
                                        if (L[19].tag) t35 = L[19]; else { or19 = 1; t35 = bh_chk(G[52], "符号"); }
                                        if (L[8].tag) t36 = L[8]; else { or8 = 1; t36 = bh_chk(G[43], "原字"); }
                                        if (L[3].tag) t37 = L[3]; else { or3 = 1; t37 = bh_chk(G[35], "行号"); }
                                        t38 = bh_list_make(3, (Value[]){t35, t36, t37});
                                        t39 = bi_append(2, (Value[]){t34, t38});
                                    } else {
                                        {
                                            t35 = BH_FALSE;
                                            {
                                                bh_line = 246;
                                                if (L[19].tag) t36 = L[19]; else { or19 = 1; t36 = bh_chk(G[52], "符号"); }
                                                if (bh_cmp(0, t36, K[102])) {
                                                    t35 = BH_TRUE;
                                                }
                                            }
                                            if (bh_truthy(t35)) {
                                                t34 = t35;
                                            } else {
                                                t37 = BH_FALSE;
                                                {
                                                    bh_line = 246;
                                                    if (L[19].tag) t38 = L[19]; else { or19 = 1; t38 = bh_chk(G[52], "符号"); }
                                                    if (bh_cmp(0, t38, K[50])) {
                                                        t37 = BH_TRUE;
                                                    }
                                                }
                                                t34 = t37;
                                            }
                                        }
                                        if (bh_truthy(t34)) {
                                            bh_line = 247;
                                            t39 = BH_FALSE;
                                            {
                                                if (L[2].tag) t40 = L[2]; else { or2 = 1; t40 = bh_chk(G[37], "括号栈"); }
                                                t41 = bi_len(1, (Value[]){t40});
                                                if (bh_cmp(0, t41, bh_int(0LL))) {
                                                    t39 = BH_TRUE;
                                                }
                                            }
                                            if (bh_truthy(t39)) {
                                                bh_line = 248;
                                                t42 = bh_chk(G[32], "语法出错");
                                                if (L[8].tag) t43 = L[8]; else { or8 = 1; t43 = bh_chk(G[43], "原字"); }
                                                t44 = bh_add(K[103], t43);
                                                t45 = bh_add(t44, K[104]);
                                                if (L[3].tag) t46 = L[3]; else { or3 = 1; t46 = bh_chk(G[35], "行号"); }
                                                t47 = bh_call(t42, 2, (Value[]){t45, t46}, "语法出错");
                                            } else {
                                            }
                                            bh_line = 249;
                                            if (L[2].tag) t39 = L[2]; else { or2 = 1; t39 = bh_chk(G[37], "括号栈"); }
                                            t40 = bh_neg(bh_int(1LL));
                                            t41 = bh_index(t39, t40);
                                            if (!L[21].tag && or21) bh_modify_outer("开"); L[21] = t41;
                                            bh_line = 250;
                                            if (L[2].tag) t39 = L[2]; else { or2 = 1; t39 = bh_chk(G[37], "括号栈"); }
                                            t40 = bh_neg(bh_int(1LL));
                                            t41 = bi_remove(2, (Value[]){t39, t40});
                                            bh_line = 251;
                                            t39 = BH_FALSE;
                                            {
                                                t40 = BH_FALSE;
                                                {
                                                    if (L[21].tag) t41 = L[21]; else { or21 = 1; t41 = bh_chk(G[53], "开"); }
                                                    t42 = bh_index(t41, bh_int(0LL));
                                                    if (bh_cmp(0, t42, K[101])) {
                                                        t40 = BH_TRUE;
                                                    }
                                                }
                                                t43 = BH_FALSE;
                                                {
                                                    bh_line = 251;
                                                    if (L[19].tag) t44 = L[19]; else { or19 = 1; t44 = bh_chk(G[52], "符号"); }
                                                    if (bh_cmp(0, t44, K[102])) {
                                                        t43 = BH_TRUE;
                                                    }
                                                }
                                                bh_line = 251;
                                                if (bh_cmp(1, t40, t43)) {
                                                    t39 = BH_TRUE;
                                                }
                                            }
                                            if (bh_truthy(t39)) {
                                                bh_line = 252;
                                                t45 = bh_chk(G[32], "语法出错");
                                                if (L[21].tag) t46 = L[21]; else { or21 = 1; t46 = bh_chk(G[53], "开"); }
                                                t47 = bh_index(t46, bh_int(1LL));
                                                t48 = bh_add(K[105], t47);
                                                t49 = bh_add(t48, K[106]);
                                                if (L[8].tag) t50 = L[8]; else { or8 = 1; t50 = bh_chk(G[43], "原字"); }
                                                t51 = bh_add(t49, t50);
                                                t52 = bh_add(t51, K[107]);
                                                if (L[3].tag) t53 = L[3]; else { or3 = 1; t53 = bh_chk(G[35], "行号"); }
                                                t54 = bh_call(t45, 2, (Value[]){t52, t53}, "语法出错");
                                            } else {
                                            }
                                        } else {
                                        }
                                    }
                                    bh_line = 253;
                                    if (L[0].tag) t29 = L[0]; else { or0 = 1; t29 = bh_chk(G[42], "记号表"); }
                                    if (L[19].tag) t30 = L[19]; else { or19 = 1; t30 = bh_chk(G[52], "符号"); }
                                    if (L[3].tag) t31 = L[3]; else { or3 = 1; t31 = bh_chk(G[35], "行号"); }
                                    if (L[4].tag) t32 = L[4]; else { or4 = 1; t32 = bh_chk(G[36], "文本"); }
                                    if (L[5].tag) t33 = L[5]; else { or5 = 1; t33 = bh_chk(G[38], "位置"); }
                                    if (L[20].tag) t34 = L[20]; else { or20 = 1; t34 = bh_chk(G[54], "占"); }
                                    t35 = bi_slice(3, (Value[]){t32, t33, t34});
                                    t36 = bh_list_make(4, (Value[]){K[97], t30, t31, t35});
                                    t37 = bi_append(2, (Value[]){t29, t36});
                                    bh_line = 254;
                                    if (L[20].tag) t29 = L[20]; else { or20 = 1; t29 = bh_chk(G[54], "占"); }
                                    if (L[5].tag) t30 = L[5]; else { or5 = 1; t30 = bh_chk(G[38], "位置"); }
                                    t31 = bh_add(t30, t29);
                                    if (!L[5].tag && or5) bh_modify_outer("位置"); L[5] = t31;
                                }
                            }
                        }
                    }
                }
            }
            bh_line = 256;
            t1 = BH_FALSE;
            {
                if (L[2].tag) t2 = L[2]; else { or2 = 1; t2 = bh_chk(G[37], "括号栈"); }
                t3 = bi_len(1, (Value[]){t2});
                if (bh_cmp(0, t3, bh_int(0LL))) {
                    t1 = BH_TRUE;
                }
            }
            if (bh_truthy(t1)) {
                bh_line = 257;
                if (L[0].tag) t4 = L[0]; else { or0 = 1; t4 = bh_chk(G[42], "记号表"); }
                if (L[3].tag) t5 = L[3]; else { or3 = 1; t5 = bh_chk(G[35], "行号"); }
                t6 = bh_list_make(4, (Value[]){K[108], BH_NIL, t5, K[79]});
                t7 = bi_append(2, (Value[]){t4, t6});
            } else {
            }
        }
    }
    bh_line = 259;
    t0 = BH_FALSE;
    {
        if (L[2].tag) t1 = L[2]; else { or2 = 1; t1 = bh_chk(G[37], "括号栈"); }
        t2 = bi_len(1, (Value[]){t1});
        if (bh_cmp(3, t2, bh_int(0LL))) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 260;
        if (L[2].tag) t3 = L[2]; else { or2 = 1; t3 = bh_chk(G[37], "括号栈"); }
        t4 = bh_neg(bh_int(1LL));
        t5 = bh_index(t3, t4);
        if (!L[21].tag && or21) bh_modify_outer("开"); L[21] = t5;
        bh_line = 261;
        t3 = bh_chk(G[32], "语法出错");
        if (L[21].tag) t4 = L[21]; else { or21 = 1; t4 = bh_chk(G[53], "开"); }
        t5 = bh_index(t4, bh_int(1LL));
        t6 = bh_add(K[109], t5);
        t7 = bh_add(t6, K[110]);
        if (L[21].tag) t8 = L[21]; else { or21 = 1; t8 = bh_chk(G[53], "开"); }
        t9 = bh_index(t8, bh_int(2LL));
        t10 = bh_call(t3, 2, (Value[]){t7, t9}, "语法出错");
    } else {
    }
    bh_line = 262;
    {
        if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[41], "缩进栈"); }
        t1 = bi_len(1, (Value[]){t0});
        t2 = bh_sub(t1, bh_int(1LL));
        int64_t n2 = bh_count(t2), n3;
        for (n3 = 0; n3 < n2; n3++) {
            BH_SAFEPOINT();
            bh_line = 263;
            if (L[0].tag) t3 = L[0]; else { or0 = 1; t3 = bh_chk(G[42], "记号表"); }
            t4 = bh_chk(G[18], "源码行表");
            t5 = bi_len(1, (Value[]){t4});
            t6 = bh_list_make(4, (Value[]){K[86], BH_NIL, t5, K[79]});
            t7 = bi_append(2, (Value[]){t3, t6});
        }
    }
    bh_line = 264;
    if (L[0].tag) t0 = L[0]; else { or0 = 1; t0 = bh_chk(G[42], "记号表"); }
    t1 = bh_chk(G[18], "源码行表");
    t2 = bi_len(1, (Value[]){t1});
    t3 = bh_list_make(4, (Value[]){K[111], BH_NIL, t2, K[79]});
    t4 = bi_append(2, (Value[]){t0, t3});
    bh_line = 265;
    if (L[0].tag) t0 = L[0]; else { or0 = 1; t0 = bh_chk(G[42], "记号表"); }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn11(Frame *up, Value *a) {    /* 函数 看，第 285 行 */
    Value L[1] = {{0}};
    Value t0, t1, t2;
    bh_line = 286;
    t0 = bh_chk(G[56], "记号");
    t1 = bh_chk(G[57], "游标");
    t2 = bh_index(t0, t1);
    return t2;
    return BH_NIL;
}

static Value fn12(Frame *up, Value *a) {    /* 函数 前进，第 289 行 */
    Value L[2] = {{0}};
    Value t0, t1, t2, t3, t4;
    char or0 = 0;
    char or1 = 0;
    char gf0 = 0;
    bh_line = 290;
    gf0 = 1;
    bh_line = 291;
    t0 = bh_chk(G[56], "记号");
    if (gf0) t1 = bh_chk(G[57], "游标"); else if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[57], "游标"); }
    t2 = bh_index(t0, t1);
    if (!L[0].tag && or0) bh_modify_outer("这个"); L[0] = t2;
    bh_line = 292;
    t0 = BH_FALSE;
    {
        if (L[0].tag) t1 = L[0]; else { or0 = 1; t1 = bh_chk(G[61], "这个"); }
        t2 = bh_index(t1, bh_int(0LL));
        if (bh_cmp(1, t2, K[111])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 293;
        if (gf0) t3 = bh_chk(G[57], "游标"); else if (L[1].tag) t3 = L[1]; else { or1 = 1; t3 = bh_chk(G[57], "游标"); }
        t4 = bh_add(t3, bh_int(1LL));
        if (gf0) G[57] = t4; else { if (!L[1].tag && or1) bh_modify_outer("游标"); L[1] = t4; }
    } else {
    }
    bh_line = 294;
    if (L[0].tag) t0 = L[0]; else { or0 = 1; t0 = bh_chk(G[61], "这个"); }
    return t0;
    return BH_NIL;
}

static Value fn13(Frame *up, Value *a) {    /* 函数 是这个，第 297 行 */
    Value L[3] = {{0}};
    Value t0, t1, t2, t3, t4;
    L[0] = a[0];    /* 种类 */
    char or0 = 0;
    L[1] = a[1];    /* 值 */
    char or1 = 0;
    char or2 = 0;
    bh_line = 299;
    t0 = bh_chk(G[56], "记号");
    t1 = bh_chk(G[57], "游标");
    t2 = bh_index(t0, t1);
    if (!L[2].tag && or2) bh_modify_outer("这个"); L[2] = t2;
    bh_line = 300;
    t0 = BH_FALSE;
    {
        if (L[2].tag) t1 = L[2]; else { or2 = 1; t1 = bh_chk(G[61], "这个"); }
        t2 = bh_index(t1, bh_int(0LL));
        if (bh_cmp(1, t2, L[0])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 301;
        return BH_FALSE;
    } else {
    }
    bh_line = 302;
    {
        t1 = BH_FALSE;
        {
            if (bh_cmp(0, L[1], BH_NIL)) {
                t1 = BH_TRUE;
            }
        }
        if (bh_truthy(t1)) {
            t0 = t1;
        } else {
            t2 = BH_FALSE;
            {
                bh_line = 302;
                if (L[2].tag) t3 = L[2]; else { or2 = 1; t3 = bh_chk(G[61], "这个"); }
                t4 = bh_index(t3, bh_int(1LL));
                if (bh_cmp(0, t4, L[1])) {
                    t2 = BH_TRUE;
                }
            }
            t0 = t2;
        }
    }
    return t0;
    return BH_NIL;
}

static Value fn14(Frame *up, Value *a) {    /* 函数 接受，第 305 行 */
    struct { BhRoots h; Value v[2]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 2);
    L = fr.v; T = fr.v + 2;
    Value t0, t1, t2, t3;
    L[0] = a[0];    /* 种类 */
    char or0 = 0;
    L[1] = a[1];    /* 值 */
    char or1 = 0;
    BH_SAFEPOINT();
    bh_line = 306;
    t0 = bh_chk(G[63], "是这个");
    t1 = bh_call(t0, 2, (Value[]){L[0], L[1]}, "是这个");
    if (bh_truthy(t1)) {
        bh_line = 307;
        t2 = bh_chk(G[62], "前进");
        t3 = bh_call(t2, 0, NULL, "前进");
        BH_RETURN(fr, t3);
    } else {
    }
    bh_line = 308;
    BH_RETURN(fr, BH_NIL);
    BH_RETURN(fr, BH_NIL);
}

static Value fn15(Frame *up, Value *a) {    /* 函数 说，第 311 行 */
    Value L[2] = {{0}};
    Value t0, t1, t2;
    L[0] = a[0];    /* 某记号 */
    char or0 = 0;
    char or1 = 0;
    bh_line = 313;
    t0 = bh_index(L[0], bh_int(0LL));
    if (!L[1].tag && or1) bh_modify_outer("种类"); L[1] = t0;
    bh_line = 314;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[108])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 314;
        return K[112];
    } else {
    }
    bh_line = 315;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[85])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 315;
        return K[113];
    } else {
    }
    bh_line = 316;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[86])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 316;
        return K[114];
    } else {
    }
    bh_line = 317;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[111])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 317;
        return K[115];
    } else {
    }
    bh_line = 318;
    t0 = bh_index(L[0], bh_int(3LL));
    t1 = bh_add(K[63], t0);
    t2 = bh_add(t1, K[67]);
    return t2;
    return BH_NIL;
}

static Value fn16(Frame *up, Value *a) {    /* 函数 语法错，第 321 行 */
    struct { BhRoots h; Value v[6]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 6);
    L = fr.v; T = fr.v + 5;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12;
    L[0] = a[0];    /* 消息 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    BH_SAFEPOINT();
    bh_line = 322;
    t0 = bh_chk(G[56], "记号");
    t1 = bh_chk(G[57], "游标");
    t2 = bh_index(t0, t1);
    if (!L[1].tag && or1) bh_modify_outer("这个"); L[1] = t2;
    bh_line = 323;
    if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[61], "这个"); }
    t1 = bh_index(t0, bh_int(2LL));
    if (!L[2].tag && or2) bh_modify_outer("行"); L[2] = t1;
    bh_line = 324;
    {
        t1 = BH_FALSE;
        {
            if (L[1].tag) t2 = L[1]; else { or1 = 1; t2 = bh_chk(G[61], "这个"); }
            t3 = bh_index(t2, bh_int(0LL));
            if (bh_cmp(0, t3, K[111])) {
                t1 = BH_TRUE;
            }
        }
        if (bh_truthy(t1)) {
            t4 = BH_FALSE;
            {
                bh_line = 324;
                t5 = bh_chk(G[57], "游标");
                if (bh_cmp(3, t5, bh_int(0LL))) {
                    t4 = BH_TRUE;
                }
            }
            t0 = t4;
        } else {
            t0 = t1;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 325;
        t6 = bh_chk(G[56], "记号");
        t7 = bh_chk(G[57], "游标");
        t8 = bh_sub(t7, bh_int(1LL));
        t9 = bh_index(t6, t8);
        t10 = bh_index(t9, bh_int(2LL));
        if (!L[2].tag && or2) bh_modify_outer("行"); L[2] = t10;
    } else {
    }
    bh_line = 326;
    t0 = bh_list_make(0, NULL);
    if (!L[3].tag && or3) bh_modify_outer("名字们"); L[3] = t0;
    bh_line = 327;
    {
        t0 = bh_chk(G[56], "记号");
        T[0] = bh_iter(t0);
        int64_t n1;
        for (n1 = 0; n1 < bh_list_len(T[0]); n1++) {
            BH_SAFEPOINT();
            bh_line = 327;
            if (!L[4].tag && or4) bh_modify_outer("某个"); L[4] = bh_list_at(T[0], n1);
            bh_line = 328;
            {
                t2 = BH_FALSE;
                {
                    if (L[4].tag) t3 = L[4]; else { or4 = 1; t3 = bh_chk(G[67], "某个"); }
                    t4 = bh_index(t3, bh_int(0LL));
                    if (bh_cmp(0, t4, K[99])) {
                        t2 = BH_TRUE;
                    }
                }
                if (bh_truthy(t2)) {
                    t5 = BH_FALSE;
                    {
                        bh_line = 328;
                        if (L[4].tag) t6 = L[4]; else { or4 = 1; t6 = bh_chk(G[67], "某个"); }
                        t7 = bh_index(t6, bh_int(2LL));
                        if (L[2].tag) t8 = L[2]; else { or2 = 1; t8 = bh_chk(G[68], "行"); }
                        if (bh_cmp(0, t7, t8)) {
                            t5 = BH_TRUE;
                        }
                    }
                    t1 = t5;
                } else {
                    t1 = t2;
                }
            }
            if (bh_truthy(t1)) {
                bh_line = 329;
                if (L[3].tag) t9 = L[3]; else { or3 = 1; t9 = bh_chk(G[69], "名字们"); }
                if (L[4].tag) t10 = L[4]; else { or4 = 1; t10 = bh_chk(G[67], "某个"); }
                t11 = bh_index(t10, bh_int(1LL));
                t12 = bi_append(2, (Value[]){t9, t11});
            } else {
            }
        }
    }
    bh_line = 330;
    t0 = bh_chk(G[32], "语法出错");
    T[0] = t0;
    t1 = bh_chk(G[34], "空格提示");
    if (L[3].tag) t2 = L[3]; else { or3 = 1; t2 = bh_chk(G[69], "名字们"); }
    t3 = bh_call(t1, 1, (Value[]){t2}, "空格提示");
    t4 = bh_add(L[0], t3);
    if (L[2].tag) t5 = L[2]; else { or2 = 1; t5 = bh_chk(G[68], "行"); }
    t6 = bh_call(T[0], 2, (Value[]){t4, t5}, "语法出错");
    BH_RETURN(fr, BH_NIL);
}

static Value fn17(Frame *up, Value *a) {    /* 函数 期待，第 333 行 */
    struct { BhRoots h; Value v[6]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 6);
    L = fr.v; T = fr.v + 3;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12;
    L[0] = a[0];    /* 种类 */
    char or0 = 0;
    L[1] = a[1];    /* 值 */
    char or1 = 0;
    char or2 = 0;
    BH_SAFEPOINT();
    bh_line = 334;
    t0 = bh_chk(G[63], "是这个");
    t1 = bh_call(t0, 2, (Value[]){L[0], L[1]}, "是这个");
    t2 = bh_bool(!bh_truthy(t1));
    if (bh_truthy(t2)) {
        bh_line = 335;
        t3 = bh_add(K[63], L[1]);
        t4 = bh_add(t3, K[67]);
        if (!L[2].tag && or2) bh_modify_outer("想要"); L[2] = t4;
        bh_line = 336;
        t3 = BH_FALSE;
        {
            if (bh_cmp(0, L[1], K[116])) {
                t3 = BH_TRUE;
            }
        }
        if (bh_truthy(t3)) {
            bh_line = 336;
            if (!L[2].tag && or2) bh_modify_outer("想要"); L[2] = K[117];
        } else {
        }
        bh_line = 337;
        t3 = BH_FALSE;
        {
            if (bh_cmp(0, L[1], K[101])) {
                t3 = BH_TRUE;
            }
        }
        if (bh_truthy(t3)) {
            bh_line = 337;
            if (!L[2].tag && or2) bh_modify_outer("想要"); L[2] = K[118];
        } else {
        }
        bh_line = 338;
        t3 = BH_FALSE;
        {
            if (bh_cmp(0, L[1], K[102])) {
                t3 = BH_TRUE;
            }
        }
        if (bh_truthy(t3)) {
            bh_line = 338;
            if (!L[2].tag && or2) bh_modify_outer("想要"); L[2] = K[119];
        } else {
        }
        bh_line = 339;
        t3 = BH_FALSE;
        {
            if (bh_cmp(0, L[1], K[50])) {
                t3 = BH_TRUE;
            }
        }
        if (bh_truthy(t3)) {
            bh_line = 339;
            if (!L[2].tag && or2) bh_modify_outer("想要"); L[2] = K[120];
        } else {
        }
        bh_line = 340;
        t3 = bh_chk(G[70], "语法错");
        T[0] = t3;
        if (L[2].tag) t4 = L[2]; else { or2 = 1; t4 = bh_chk(G[71], "想要"); }
        t5 = bh_add(K[121], t4);
        t6 = bh_add(t5, K[122]);
        T[1] = t6;
        t7 = bh_chk(G[66], "说");
        T[2] = t7;
        t8 = bh_chk(G[60], "看");
        t9 = bh_call(t8, 0, NULL, "看");
        t10 = bh_call(T[2], 1, (Value[]){t9}, "说");
        t11 = bh_add(T[1], t10);
        t12 = bh_call(T[0], 1, (Value[]){t11}, "语法错");
    } else {
    }
    bh_line = 341;
    t0 = bh_chk(G[62], "前进");
    t1 = bh_call(t0, 0, NULL, "前进");
    BH_RETURN(fr, t1);
    BH_RETURN(fr, BH_NIL);
}

static Value fn18(Frame *up, Value *a) {    /* 函数 期待名字，第 344 行 */
    struct { BhRoots h; Value v[4]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 4);
    L = fr.v; T = fr.v + 2;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15;
    L[0] = a[0];    /* 什么 */
    char or0 = 0;
    char or1 = 0;
    BH_SAFEPOINT();
    bh_line = 345;
    t0 = bh_chk(G[60], "看");
    t1 = bh_call(t0, 0, NULL, "看");
    if (!L[1].tag && or1) bh_modify_outer("这个"); L[1] = t1;
    bh_line = 346;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[61], "这个"); }
        t2 = bh_index(t1, bh_int(0LL));
        if (bh_cmp(0, t2, K[99])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 347;
        t3 = bh_chk(G[62], "前进");
        t4 = bh_call(t3, 0, NULL, "前进");
        t5 = bh_index(t4, bh_int(1LL));
        BH_RETURN(fr, t5);
    } else {
    }
    bh_line = 348;
    {
        t1 = BH_FALSE;
        {
            if (L[1].tag) t2 = L[1]; else { or1 = 1; t2 = bh_chk(G[61], "这个"); }
            t3 = bh_index(t2, bh_int(0LL));
            if (bh_cmp(0, t3, K[98])) {
                t1 = BH_TRUE;
            }
        }
        if (bh_truthy(t1)) {
            t0 = t1;
        } else {
            t4 = BH_FALSE;
            {
                bh_line = 348;
                t5 = bh_chk(G[1], "文字运算符");
                if (L[1].tag) t6 = L[1]; else { or1 = 1; t6 = bh_chk(G[61], "这个"); }
                t7 = bh_index(t6, bh_int(3LL));
                t8 = bi_find(2, (Value[]){t5, t7});
                if (bh_cmp(5, t8, bh_int(0LL))) {
                    t4 = BH_TRUE;
                }
            }
            t0 = t4;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 349;
        t9 = bh_chk(G[70], "语法错");
        if (L[1].tag) t10 = L[1]; else { or1 = 1; t10 = bh_chk(G[61], "这个"); }
        t11 = bh_index(t10, bh_int(3LL));
        t12 = bh_add(K[63], t11);
        t13 = bh_add(t12, K[123]);
        t14 = bh_add(t13, L[0]);
        t15 = bh_call(t9, 1, (Value[]){t14}, "语法错");
    } else {
    }
    bh_line = 350;
    t0 = bh_chk(G[70], "语法错");
    T[0] = t0;
    t1 = bh_add(K[124], L[0]);
    t2 = bh_add(t1, K[122]);
    T[1] = t2;
    t3 = bh_chk(G[66], "说");
    if (L[1].tag) t4 = L[1]; else { or1 = 1; t4 = bh_chk(G[61], "这个"); }
    t5 = bh_call(t3, 1, (Value[]){t4}, "说");
    t6 = bh_add(T[1], t5);
    t7 = bh_call(T[0], 1, (Value[]){t6}, "语法错");
    BH_RETURN(fr, BH_NIL);
}

static Value fn19(Frame *up, Value *a) {    /* 函数 语句结束了，第 353 行 */
    struct { BhRoots h; Value v[1]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 1);
    L = fr.v; T = fr.v + 0;
    Value t0, t1, t2, t3, t4, t5, t6, t7;
    BH_SAFEPOINT();
    bh_line = 354;
    {
        {
            t2 = bh_chk(G[63], "是这个");
            t3 = bh_call(t2, 2, (Value[]){K[108], BH_NIL}, "是这个");
            if (bh_truthy(t3)) {
                t1 = t3;
            } else {
                bh_line = 354;
                t4 = bh_chk(G[63], "是这个");
                t5 = bh_call(t4, 2, (Value[]){K[111], BH_NIL}, "是这个");
                t1 = t5;
            }
        }
        if (bh_truthy(t1)) {
            t0 = t1;
        } else {
            bh_line = 354;
            t6 = bh_chk(G[63], "是这个");
            t7 = bh_call(t6, 2, (Value[]){K[97], K[52]}, "是这个");
            t0 = t7;
        }
    }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn20(Frame *up, Value *a) {    /* 函数 并入，第 357 行 */
    struct { BhRoots h; Value v[4]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 4);
    L = fr.v; T = fr.v + 3;
    Value t0, t1;
    L[0] = a[0];    /* 表 */
    char or0 = 0;
    L[1] = a[1];    /* 新的 */
    char or1 = 0;
    char or2 = 0;
    BH_SAFEPOINT();
    bh_line = 358;
    {
        T[0] = bh_iter(L[1]);
        int64_t n1;
        for (n1 = 0; n1 < bh_list_len(T[0]); n1++) {
            BH_SAFEPOINT();
            bh_line = 358;
            if (!L[2].tag && or2) bh_modify_outer("项"); L[2] = bh_list_at(T[0], n1);
            bh_line = 359;
            if (L[2].tag) t0 = L[2]; else { or2 = 1; t0 = bh_chk(G[75], "项"); }
            t1 = bi_append(2, (Value[]){L[0], t0});
        }
    }
    BH_RETURN(fr, BH_NIL);
}

static Value fn21(Frame *up, Value *a) {    /* 函数 解析程序，第 364 行 */
    struct { BhRoots h; Value v[3]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 3);
    L = fr.v; T = fr.v + 1;
    Value t0, t1, t2, t3, t4, t5, t6, t7;
    char or0 = 0;
    BH_SAFEPOINT();
    bh_line = 365;
    t0 = bh_list_make(0, NULL);
    if (!L[0].tag && or0) bh_modify_outer("语句表"); L[0] = t0;
    bh_line = 366;
    for (;;) {
        BH_SAFEPOINT();
        bh_line = 366;
        t0 = bh_chk(G[63], "是这个");
        t1 = bh_call(t0, 2, (Value[]){K[111], BH_NIL}, "是这个");
        t2 = bh_bool(!bh_truthy(t1));
        if (!bh_truthy(t2)) break;
        bh_line = 367;
        t3 = bh_chk(G[76], "并入");
        T[0] = t3;
        if (L[0].tag) t4 = L[0]; else { or0 = 1; t4 = bh_chk(G[77], "语句表"); }
        T[1] = t4;
        t5 = bh_chk(G[78], "解析语句");
        t6 = bh_call(t5, 0, NULL, "解析语句");
        t7 = bh_call(T[0], 2, (Value[]){T[1], t6}, "并入");
    }
    bh_line = 368;
    if (L[0].tag) t0 = L[0]; else { or0 = 1; t0 = bh_chk(G[77], "语句表"); }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn22(Frame *up, Value *a) {    /* 函数 解析语句，第 371 行 */
    struct { BhRoots h; Value v[2]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 2);
    L = fr.v; T = fr.v + 2;
    Value t0, t1, t2, t3, t4, t5, t6, t7;
    char or0 = 0;
    char or1 = 0;
    BH_SAFEPOINT();
    bh_line = 373;
    t0 = bh_chk(G[60], "看");
    t1 = bh_call(t0, 0, NULL, "看");
    if (!L[0].tag && or0) bh_modify_outer("这个"); L[0] = t1;
    bh_line = 374;
    t0 = BH_FALSE;
    {
        if (L[0].tag) t1 = L[0]; else { or0 = 1; t1 = bh_chk(G[61], "这个"); }
        t2 = bh_index(t1, bh_int(0LL));
        if (bh_cmp(0, t2, K[85])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 375;
        t3 = bh_chk(G[70], "语法错");
        t4 = bh_call(t3, 1, (Value[]){K[125]}, "语法错");
    } else {
    }
    bh_line = 376;
    t0 = BH_FALSE;
    {
        if (L[0].tag) t1 = L[0]; else { or0 = 1; t1 = bh_chk(G[61], "这个"); }
        t2 = bh_index(t1, bh_int(0LL));
        if (bh_cmp(0, t2, K[98])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 377;
        if (L[0].tag) t3 = L[0]; else { or0 = 1; t3 = bh_chk(G[61], "这个"); }
        t4 = bh_index(t3, bh_int(1LL));
        if (!L[1].tag && or1) bh_modify_outer("词"); L[1] = t4;
        bh_line = 378;
        t3 = BH_FALSE;
        {
            if (L[1].tag) t4 = L[1]; else { or1 = 1; t4 = bh_chk(G[5], "词"); }
            if (bh_cmp(0, t4, K[3])) {
                t3 = BH_TRUE;
            }
        }
        if (bh_truthy(t3)) {
            bh_line = 378;
            t5 = bh_chk(G[80], "解析如果");
            t6 = bh_call(t5, 0, NULL, "解析如果");
            t7 = bh_list_make(1, (Value[]){t6});
            BH_RETURN(fr, t7);
        } else {
        }
        bh_line = 379;
        t3 = BH_FALSE;
        {
            if (L[1].tag) t4 = L[1]; else { or1 = 1; t4 = bh_chk(G[5], "词"); }
            if (bh_cmp(0, t4, K[6])) {
                t3 = BH_TRUE;
            }
        }
        if (bh_truthy(t3)) {
            bh_line = 379;
            t5 = bh_chk(G[81], "解析当");
            t6 = bh_call(t5, 0, NULL, "解析当");
            t7 = bh_list_make(1, (Value[]){t6});
            BH_RETURN(fr, t7);
        } else {
        }
        bh_line = 380;
        t3 = BH_FALSE;
        {
            if (L[1].tag) t4 = L[1]; else { or1 = 1; t4 = bh_chk(G[5], "词"); }
            if (bh_cmp(0, t4, K[7])) {
                t3 = BH_TRUE;
            }
        }
        if (bh_truthy(t3)) {
            bh_line = 380;
            t5 = bh_chk(G[82], "解析对于");
            t6 = bh_call(t5, 0, NULL, "解析对于");
            t7 = bh_list_make(1, (Value[]){t6});
            BH_RETURN(fr, t7);
        } else {
        }
        bh_line = 381;
        t3 = BH_FALSE;
        {
            if (L[1].tag) t4 = L[1]; else { or1 = 1; t4 = bh_chk(G[5], "词"); }
            if (bh_cmp(0, t4, K[10])) {
                t3 = BH_TRUE;
            }
        }
        if (bh_truthy(t3)) {
            bh_line = 381;
            t5 = bh_chk(G[83], "解析重复");
            t6 = bh_call(t5, 0, NULL, "解析重复");
            t7 = bh_list_make(1, (Value[]){t6});
            BH_RETURN(fr, t7);
        } else {
        }
        bh_line = 382;
        t3 = BH_FALSE;
        {
            if (L[1].tag) t4 = L[1]; else { or1 = 1; t4 = bh_chk(G[5], "词"); }
            if (bh_cmp(0, t4, K[0])) {
                t3 = BH_TRUE;
            }
        }
        if (bh_truthy(t3)) {
            bh_line = 382;
            t5 = bh_chk(G[84], "解析函数");
            t6 = bh_call(t5, 0, NULL, "解析函数");
            t7 = bh_list_make(1, (Value[]){t6});
            BH_RETURN(fr, t7);
        } else {
        }
    } else {
    }
    bh_line = 383;
    t0 = bh_chk(G[85], "解析简单行");
    t1 = bh_call(t0, 0, NULL, "解析简单行");
    BH_RETURN(fr, t1);
    BH_RETURN(fr, BH_NIL);
}

static Value fn23(Frame *up, Value *a) {    /* 函数 解析简单行，第 386 行 */
    struct { BhRoots h; Value v[6]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 6);
    L = fr.v; T = fr.v + 5;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17, t18, t19, t20, t21, t22, t23, t24, t25, t26, t27, t28, t29, t30, t31;
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    BH_SAFEPOINT();
    bh_line = 387;
    t0 = bh_chk(G[86], "解析简单语句");
    t1 = bh_call(t0, 0, NULL, "解析简单语句");
    t2 = bh_list_make(1, (Value[]){t1});
    if (!L[0].tag && or0) bh_modify_outer("语句表"); L[0] = t2;
    bh_line = 388;
    for (;;) {
        BH_SAFEPOINT();
        bh_line = 388;
        t0 = bh_chk(G[64], "接受");
        t1 = bh_call(t0, 2, (Value[]){K[97], K[52]}, "接受");
        if (!bh_truthy(t1)) break;
        bh_line = 389;
        {
            t3 = bh_chk(G[63], "是这个");
            t4 = bh_call(t3, 2, (Value[]){K[108], BH_NIL}, "是这个");
            if (bh_truthy(t4)) {
                t2 = t4;
            } else {
                bh_line = 389;
                t5 = bh_chk(G[63], "是这个");
                t6 = bh_call(t5, 2, (Value[]){K[111], BH_NIL}, "是这个");
                t2 = t6;
            }
        }
        if (bh_truthy(t2)) {
            bh_line = 390;
            break;
        } else {
        }
        bh_line = 391;
        if (L[0].tag) t2 = L[0]; else { or0 = 1; t2 = bh_chk(G[77], "语句表"); }
        T[0] = t2;
        t3 = bh_chk(G[86], "解析简单语句");
        t4 = bh_call(t3, 0, NULL, "解析简单语句");
        t5 = bi_append(2, (Value[]){T[0], t4});
    }
    bh_line = 392;
    {
        t1 = bh_chk(G[63], "是这个");
        t2 = bh_call(t1, 2, (Value[]){K[111], BH_NIL}, "是这个");
        if (bh_truthy(t2)) {
            t0 = t2;
        } else {
            bh_line = 392;
            t3 = bh_chk(G[64], "接受");
            t4 = bh_call(t3, 2, (Value[]){K[108], BH_NIL}, "接受");
            t0 = t4;
        }
    }
    t5 = bh_bool(!bh_truthy(t0));
    if (bh_truthy(t5)) {
        bh_line = 393;
        t6 = bh_chk(G[60], "看");
        t7 = bh_call(t6, 0, NULL, "看");
        if (!L[1].tag && or1) bh_modify_outer("这个"); L[1] = t7;
        bh_line = 394;
        t6 = bh_chk(G[66], "说");
        if (L[1].tag) t7 = L[1]; else { or1 = 1; t7 = bh_chk(G[61], "这个"); }
        t8 = bh_call(t6, 1, (Value[]){t7}, "说");
        t9 = bh_add(K[126], t8);
        if (!L[2].tag && or2) bh_modify_outer("消息"); L[2] = t9;
        bh_line = 395;
        if (L[0].tag) t6 = L[0]; else { or0 = 1; t6 = bh_chk(G[77], "语句表"); }
        t7 = bh_neg(bh_int(1LL));
        t8 = bh_index(t6, t7);
        if (!L[3].tag && or3) bh_modify_outer("最后"); L[3] = t8;
        bh_line = 396;
        {
            {
                t8 = BH_FALSE;
                {
                    if (L[3].tag) t9 = L[3]; else { or3 = 1; t9 = bh_chk(G[87], "最后"); }
                    t10 = bh_index(t9, bh_int(0LL));
                    if (bh_cmp(0, t10, K[127])) {
                        t8 = BH_TRUE;
                    }
                }
                if (bh_truthy(t8)) {
                    t11 = BH_FALSE;
                    {
                        bh_line = 396;
                        if (L[3].tag) t12 = L[3]; else { or3 = 1; t12 = bh_chk(G[87], "最后"); }
                        t13 = bh_index(t12, bh_int(2LL));
                        t14 = bh_index(t13, bh_int(0LL));
                        if (bh_cmp(0, t14, K[128])) {
                            t11 = BH_TRUE;
                        }
                    }
                    t7 = t11;
                } else {
                    t7 = t8;
                }
            }
            if (bh_truthy(t7)) {
                t15 = BH_FALSE;
                {
                    t16 = bh_list_make(3, (Value[]){K[96], K[93], K[99]});
                    bh_line = 396;
                    if (L[1].tag) t17 = L[1]; else { or1 = 1; t17 = bh_chk(G[61], "这个"); }
                    t18 = bh_index(t17, bh_int(0LL));
                    t19 = bi_find(2, (Value[]){t16, t18});
                    if (bh_cmp(5, t19, bh_int(0LL))) {
                        t15 = BH_TRUE;
                    }
                }
                t6 = t15;
            } else {
                t6 = t7;
            }
        }
        if (bh_truthy(t6)) {
            bh_line = 397;
            if (L[3].tag) t20 = L[3]; else { or3 = 1; t20 = bh_chk(G[87], "最后"); }
            t21 = bh_index(t20, bh_int(2LL));
            t22 = bh_index(t21, bh_int(2LL));
            if (!L[4].tag && or4) bh_modify_outer("名"); L[4] = t22;
            bh_line = 398;
            if (L[2].tag) t20 = L[2]; else { or2 = 1; t20 = bh_chk(G[88], "消息"); }
            t21 = bh_add(t20, K[129]);
            if (L[4].tag) t22 = L[4]; else { or4 = 1; t22 = bh_chk(G[89], "名"); }
            t23 = bh_add(t21, t22);
            t24 = bh_add(t23, K[130]);
            if (L[4].tag) t25 = L[4]; else { or4 = 1; t25 = bh_chk(G[89], "名"); }
            t26 = bh_add(t24, t25);
            t27 = bh_add(t26, K[131]);
            if (L[1].tag) t28 = L[1]; else { or1 = 1; t28 = bh_chk(G[61], "这个"); }
            t29 = bh_index(t28, bh_int(3LL));
            t30 = bh_add(t27, t29);
            t31 = bh_add(t30, K[132]);
            if (!L[2].tag && or2) bh_modify_outer("消息"); L[2] = t31;
        } else {
            t20 = BH_FALSE;
            {
                bh_line = 399;
                if (L[1].tag) t21 = L[1]; else { or1 = 1; t21 = bh_chk(G[61], "这个"); }
                t22 = bh_index(t21, bh_int(3LL));
                if (bh_cmp(0, t22, K[40])) {
                    t20 = BH_TRUE;
                }
            }
            if (bh_truthy(t20)) {
                bh_line = 400;
                if (L[2].tag) t23 = L[2]; else { or2 = 1; t23 = bh_chk(G[88], "消息"); }
                t24 = bh_add(t23, K[133]);
                if (!L[2].tag && or2) bh_modify_outer("消息"); L[2] = t24;
            } else {
            }
        }
        bh_line = 401;
        t6 = bh_chk(G[70], "语法错");
        if (L[2].tag) t7 = L[2]; else { or2 = 1; t7 = bh_chk(G[88], "消息"); }
        t8 = bh_call(t6, 1, (Value[]){t7}, "语法错");
    } else {
    }
    bh_line = 402;
    if (L[0].tag) t0 = L[0]; else { or0 = 1; t0 = bh_chk(G[77], "语句表"); }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn24(Frame *up, Value *a) {    /* 函数 解析简单语句，第 405 行 */
    struct { BhRoots h; Value v[10]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 10);
    L = fr.v; T = fr.v + 7;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17, t18, t19, t20, t21;
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    char or5 = 0;
    char or6 = 0;
    BH_SAFEPOINT();
    bh_line = 406;
    t0 = bh_chk(G[60], "看");
    t1 = bh_call(t0, 0, NULL, "看");
    if (!L[0].tag && or0) bh_modify_outer("这个"); L[0] = t1;
    bh_line = 407;
    if (L[0].tag) t0 = L[0]; else { or0 = 1; t0 = bh_chk(G[61], "这个"); }
    t1 = bh_index(t0, bh_int(2LL));
    if (!L[1].tag && or1) bh_modify_outer("行"); L[1] = t1;
    bh_line = 408;
    t0 = BH_FALSE;
    {
        if (L[0].tag) t1 = L[0]; else { or0 = 1; t1 = bh_chk(G[61], "这个"); }
        t2 = bh_index(t1, bh_int(0LL));
        if (bh_cmp(0, t2, K[98])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 409;
        if (L[0].tag) t3 = L[0]; else { or0 = 1; t3 = bh_chk(G[61], "这个"); }
        t4 = bh_index(t3, bh_int(1LL));
        if (!L[2].tag && or2) bh_modify_outer("词"); L[2] = t4;
        bh_line = 410;
        t3 = BH_FALSE;
        {
            if (L[2].tag) t4 = L[2]; else { or2 = 1; t4 = bh_chk(G[5], "词"); }
            if (bh_cmp(0, t4, K[1])) {
                t3 = BH_TRUE;
            }
        }
        if (bh_truthy(t3)) {
            bh_line = 411;
            t5 = BH_FALSE;
            {
                t6 = bh_chk(G[59], "函数层数");
                if (bh_cmp(0, t6, bh_int(0LL))) {
                    t5 = BH_TRUE;
                }
            }
            if (bh_truthy(t5)) {
                bh_line = 412;
                t7 = bh_chk(G[70], "语法错");
                t8 = bh_call(t7, 1, (Value[]){K[134]}, "语法错");
            } else {
            }
            bh_line = 413;
            t5 = bh_chk(G[62], "前进");
            t6 = bh_call(t5, 0, NULL, "前进");
            bh_line = 414;
            if (!L[3].tag && or3) bh_modify_outer("值"); L[3] = BH_NIL;
            bh_line = 415;
            t5 = bh_chk(G[74], "语句结束了");
            t6 = bh_call(t5, 0, NULL, "语句结束了");
            t7 = bh_bool(!bh_truthy(t6));
            if (bh_truthy(t7)) {
                bh_line = 416;
                t8 = bh_chk(G[90], "解析表达式");
                t9 = bh_call(t8, 0, NULL, "解析表达式");
                if (!L[3].tag && or3) bh_modify_outer("值"); L[3] = t9;
            } else {
            }
            bh_line = 417;
            if (L[1].tag) t5 = L[1]; else { or1 = 1; t5 = bh_chk(G[68], "行"); }
            if (L[3].tag) t6 = L[3]; else { or3 = 1; t6 = bh_chk(G[91], "值"); }
            t7 = bh_list_make(3, (Value[]){K[135], t5, t6});
            BH_RETURN(fr, t7);
        } else {
        }
        bh_line = 418;
        {
            t4 = BH_FALSE;
            {
                if (L[2].tag) t5 = L[2]; else { or2 = 1; t5 = bh_chk(G[5], "词"); }
                if (bh_cmp(0, t5, K[12])) {
                    t4 = BH_TRUE;
                }
            }
            if (bh_truthy(t4)) {
                t3 = t4;
            } else {
                t6 = BH_FALSE;
                {
                    bh_line = 418;
                    if (L[2].tag) t7 = L[2]; else { or2 = 1; t7 = bh_chk(G[5], "词"); }
                    if (bh_cmp(0, t7, K[13])) {
                        t6 = BH_TRUE;
                    }
                }
                t3 = t6;
            }
        }
        if (bh_truthy(t3)) {
            bh_line = 419;
            t8 = BH_FALSE;
            {
                t9 = bh_chk(G[58], "循环层数");
                if (bh_cmp(0, t9, bh_int(0LL))) {
                    t8 = BH_TRUE;
                }
            }
            if (bh_truthy(t8)) {
                bh_line = 420;
                t10 = bh_chk(G[70], "语法错");
                if (L[2].tag) t11 = L[2]; else { or2 = 1; t11 = bh_chk(G[5], "词"); }
                t12 = bh_add(K[63], t11);
                t13 = bh_add(t12, K[136]);
                t14 = bh_call(t10, 1, (Value[]){t13}, "语法错");
            } else {
            }
            bh_line = 421;
            t8 = bh_chk(G[62], "前进");
            t9 = bh_call(t8, 0, NULL, "前进");
            bh_line = 422;
            t8 = BH_FALSE;
            {
                if (L[2].tag) t9 = L[2]; else { or2 = 1; t9 = bh_chk(G[5], "词"); }
                if (bh_cmp(0, t9, K[12])) {
                    t8 = BH_TRUE;
                }
            }
            if (bh_truthy(t8)) {
                bh_line = 423;
                if (L[1].tag) t10 = L[1]; else { or1 = 1; t10 = bh_chk(G[68], "行"); }
                t11 = bh_list_make(2, (Value[]){K[137], t10});
                BH_RETURN(fr, t11);
            } else {
            }
            bh_line = 424;
            if (L[1].tag) t8 = L[1]; else { or1 = 1; t8 = bh_chk(G[68], "行"); }
            t9 = bh_list_make(2, (Value[]){K[138], t8});
            BH_RETURN(fr, t9);
        } else {
        }
        bh_line = 425;
        t3 = BH_FALSE;
        {
            if (L[2].tag) t4 = L[2]; else { or2 = 1; t4 = bh_chk(G[5], "词"); }
            if (bh_cmp(0, t4, K[2])) {
                t3 = BH_TRUE;
            }
        }
        if (bh_truthy(t3)) {
            bh_line = 426;
            t5 = bh_chk(G[62], "前进");
            t6 = bh_call(t5, 0, NULL, "前进");
            bh_line = 427;
            t5 = bh_chk(G[73], "期待名字");
            t6 = bh_call(t5, 1, (Value[]){K[139]}, "期待名字");
            t7 = bh_list_make(1, (Value[]){t6});
            if (!L[4].tag && or4) bh_modify_outer("名字们"); L[4] = t7;
            bh_line = 428;
            for (;;) {
                BH_SAFEPOINT();
                bh_line = 428;
                t5 = bh_chk(G[64], "接受");
                t6 = bh_call(t5, 2, (Value[]){K[97], K[51]}, "接受");
                if (!bh_truthy(t6)) break;
                bh_line = 429;
                if (L[4].tag) t7 = L[4]; else { or4 = 1; t7 = bh_chk(G[69], "名字们"); }
                T[0] = t7;
                t8 = bh_chk(G[73], "期待名字");
                t9 = bh_call(t8, 1, (Value[]){K[139]}, "期待名字");
                t10 = bi_append(2, (Value[]){T[0], t9});
            }
            bh_line = 430;
            if (L[1].tag) t5 = L[1]; else { or1 = 1; t5 = bh_chk(G[68], "行"); }
            if (L[4].tag) t6 = L[4]; else { or4 = 1; t6 = bh_chk(G[69], "名字们"); }
            t7 = bh_list_make(3, (Value[]){K[140], t5, t6});
            BH_RETURN(fr, t7);
        } else {
        }
    } else {
    }
    bh_line = 432;
    t0 = bh_chk(G[90], "解析表达式");
    t1 = bh_call(t0, 0, NULL, "解析表达式");
    if (!L[5].tag && or5) bh_modify_outer("式"); L[5] = t1;
    bh_line = 433;
    t0 = bh_chk(G[60], "看");
    t1 = bh_call(t0, 0, NULL, "看");
    if (!L[6].tag && or6) bh_modify_outer("符"); L[6] = t1;
    bh_line = 434;
    {
        t1 = BH_FALSE;
        {
            if (L[6].tag) t2 = L[6]; else { or6 = 1; t2 = bh_chk(G[92], "符"); }
            t3 = bh_index(t2, bh_int(0LL));
            if (bh_cmp(0, t3, K[97])) {
                t1 = BH_TRUE;
            }
        }
        if (bh_truthy(t1)) {
            t4 = BH_FALSE;
            {
                bh_line = 434;
                t5 = bh_chk(G[15], "赋值符号");
                if (L[6].tag) t6 = L[6]; else { or6 = 1; t6 = bh_chk(G[92], "符"); }
                t7 = bh_index(t6, bh_int(1LL));
                t8 = bi_find(2, (Value[]){t5, t7});
                if (bh_cmp(5, t8, bh_int(0LL))) {
                    t4 = BH_TRUE;
                }
            }
            t0 = t4;
        } else {
            t0 = t1;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 435;
        {
            t10 = BH_FALSE;
            {
                if (L[5].tag) t11 = L[5]; else { or5 = 1; t11 = bh_chk(G[93], "式"); }
                t12 = bh_index(t11, bh_int(0LL));
                if (bh_cmp(1, t12, K[128])) {
                    t10 = BH_TRUE;
                }
            }
            if (bh_truthy(t10)) {
                t13 = BH_FALSE;
                {
                    bh_line = 435;
                    if (L[5].tag) t14 = L[5]; else { or5 = 1; t14 = bh_chk(G[93], "式"); }
                    t15 = bh_index(t14, bh_int(0LL));
                    if (bh_cmp(1, t15, K[141])) {
                        t13 = BH_TRUE;
                    }
                }
                t9 = t13;
            } else {
                t9 = t10;
            }
        }
        if (bh_truthy(t9)) {
            bh_line = 436;
            t16 = bh_chk(G[70], "语法错");
            if (L[6].tag) t17 = L[6]; else { or6 = 1; t17 = bh_chk(G[92], "符"); }
            t18 = bh_index(t17, bh_int(3LL));
            t19 = bh_add(K[63], t18);
            t20 = bh_add(t19, K[142]);
            t21 = bh_call(t16, 1, (Value[]){t20}, "语法错");
        } else {
        }
        bh_line = 437;
        t9 = bh_chk(G[62], "前进");
        t10 = bh_call(t9, 0, NULL, "前进");
        bh_line = 438;
        if (L[1].tag) t9 = L[1]; else { or1 = 1; t9 = bh_chk(G[68], "行"); }
        T[0] = t9;
        if (L[5].tag) t10 = L[5]; else { or5 = 1; t10 = bh_chk(G[93], "式"); }
        T[1] = t10;
        if (L[6].tag) t11 = L[6]; else { or6 = 1; t11 = bh_chk(G[92], "符"); }
        t12 = bh_index(t11, bh_int(1LL));
        T[2] = t12;
        t13 = bh_chk(G[90], "解析表达式");
        t14 = bh_call(t13, 0, NULL, "解析表达式");
        t15 = bh_list_make(5, (Value[]){K[143], T[0], T[1], T[2], t14});
        BH_RETURN(fr, t15);
    } else {
    }
    bh_line = 439;
    if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[68], "行"); }
    if (L[5].tag) t1 = L[5]; else { or5 = 1; t1 = bh_chk(G[93], "式"); }
    t2 = bh_list_make(3, (Value[]){K[127], t0, t1});
    BH_RETURN(fr, t2);
    BH_RETURN(fr, BH_NIL);
}

static Value fn25(Frame *up, Value *a) {    /* 函数 解析块，第 442 行 */
    struct { BhRoots h; Value v[3]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 3);
    L = fr.v; T = fr.v + 1;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11;
    char or0 = 0;
    BH_SAFEPOINT();
    bh_line = 444;
    t0 = bh_chk(G[63], "是这个");
    t1 = bh_call(t0, 2, (Value[]){K[97], K[74]}, "是这个");
    if (bh_truthy(t1)) {
        bh_line = 445;
        t2 = bh_chk(G[70], "语法错");
        t3 = bh_call(t2, 1, (Value[]){K[144]}, "语法错");
    } else {
    }
    bh_line = 446;
    t0 = bh_chk(G[72], "期待");
    t1 = bh_call(t0, 2, (Value[]){K[97], K[116]}, "期待");
    bh_line = 447;
    t0 = bh_chk(G[63], "是这个");
    t1 = bh_call(t0, 2, (Value[]){K[108], BH_NIL}, "是这个");
    t2 = bh_bool(!bh_truthy(t1));
    if (bh_truthy(t2)) {
        bh_line = 448;
        t3 = bh_chk(G[85], "解析简单行");
        t4 = bh_call(t3, 0, NULL, "解析简单行");
        BH_RETURN(fr, t4);
    } else {
    }
    bh_line = 449;
    t0 = bh_chk(G[62], "前进");
    t1 = bh_call(t0, 0, NULL, "前进");
    bh_line = 450;
    t0 = bh_chk(G[64], "接受");
    t1 = bh_call(t0, 2, (Value[]){K[85], BH_NIL}, "接受");
    t2 = bh_bool(!bh_truthy(t1));
    if (bh_truthy(t2)) {
        bh_line = 451;
        t3 = bh_chk(G[70], "语法错");
        t4 = bh_call(t3, 1, (Value[]){K[145]}, "语法错");
    } else {
    }
    bh_line = 452;
    t0 = bh_list_make(0, NULL);
    if (!L[0].tag && or0) bh_modify_outer("语句表"); L[0] = t0;
    bh_line = 453;
    for (;;) {
        BH_SAFEPOINT();
        {
            bh_line = 453;
            t1 = bh_chk(G[63], "是这个");
            t2 = bh_call(t1, 2, (Value[]){K[86], BH_NIL}, "是这个");
            t3 = bh_bool(!bh_truthy(t2));
            if (bh_truthy(t3)) {
                t4 = bh_chk(G[63], "是这个");
                t5 = bh_call(t4, 2, (Value[]){K[111], BH_NIL}, "是这个");
                t6 = bh_bool(!bh_truthy(t5));
                t0 = t6;
            } else {
                t0 = t3;
            }
        }
        if (!bh_truthy(t0)) break;
        bh_line = 454;
        t7 = bh_chk(G[76], "并入");
        T[0] = t7;
        if (L[0].tag) t8 = L[0]; else { or0 = 1; t8 = bh_chk(G[77], "语句表"); }
        T[1] = t8;
        t9 = bh_chk(G[78], "解析语句");
        t10 = bh_call(t9, 0, NULL, "解析语句");
        t11 = bh_call(T[0], 2, (Value[]){T[1], t10}, "并入");
    }
    bh_line = 455;
    t0 = bh_chk(G[64], "接受");
    t1 = bh_call(t0, 2, (Value[]){K[86], BH_NIL}, "接受");
    bh_line = 456;
    if (L[0].tag) t0 = L[0]; else { or0 = 1; t0 = bh_chk(G[77], "语句表"); }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn26(Frame *up, Value *a) {    /* 函数 解析循环体，第 459 行 */
    struct { BhRoots h; Value v[2]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 2);
    L = fr.v; T = fr.v + 2;
    Value t0, t1;
    char or0 = 0;
    char or1 = 0;
    char gf0 = 0;
    BH_SAFEPOINT();
    bh_line = 460;
    gf0 = 1;
    bh_line = 461;
    if (gf0) t0 = bh_chk(G[58], "循环层数"); else if (L[0].tag) t0 = L[0]; else { or0 = 1; t0 = bh_chk(G[58], "循环层数"); }
    t1 = bh_add(t0, bh_int(1LL));
    if (gf0) G[58] = t1; else { if (!L[0].tag && or0) bh_modify_outer("循环层数"); L[0] = t1; }
    bh_line = 462;
    t0 = bh_chk(G[94], "解析块");
    t1 = bh_call(t0, 0, NULL, "解析块");
    if (!L[1].tag && or1) bh_modify_outer("体"); L[1] = t1;
    bh_line = 463;
    if (gf0) t0 = bh_chk(G[58], "循环层数"); else if (L[0].tag) t0 = L[0]; else { or0 = 1; t0 = bh_chk(G[58], "循环层数"); }
    t1 = bh_sub(t0, bh_int(1LL));
    if (gf0) G[58] = t1; else { if (!L[0].tag && or0) bh_modify_outer("循环层数"); L[0] = t1; }
    bh_line = 464;
    if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[95], "体"); }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn27(Frame *up, Value *a) {    /* 函数 解析如果，第 467 行 */
    struct { BhRoots h; Value v[6]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 6);
    L = fr.v; T = fr.v + 4;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11;
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    BH_SAFEPOINT();
    bh_line = 468;
    t0 = bh_chk(G[62], "前进");
    t1 = bh_call(t0, 0, NULL, "前进");
    t2 = bh_index(t1, bh_int(2LL));
    if (!L[0].tag && or0) bh_modify_outer("行"); L[0] = t2;
    bh_line = 469;
    t0 = bh_chk(G[90], "解析表达式");
    t1 = bh_call(t0, 0, NULL, "解析表达式");
    if (!L[1].tag && or1) bh_modify_outer("条件"); L[1] = t1;
    bh_line = 470;
    if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[97], "条件"); }
    T[0] = t0;
    t1 = bh_chk(G[94], "解析块");
    t2 = bh_call(t1, 0, NULL, "解析块");
    t3 = bh_list_make(2, (Value[]){T[0], t2});
    t4 = bh_list_make(1, (Value[]){t3});
    if (!L[2].tag && or2) bh_modify_outer("分支们"); L[2] = t4;
    bh_line = 471;
    if (!L[3].tag && or3) bh_modify_outer("别的"); L[3] = BH_NIL;
    bh_line = 472;
    for (;;) {
        BH_SAFEPOINT();
        if (!bh_truthy(BH_TRUE)) break;
        bh_line = 473;
        t0 = bh_chk(G[64], "接受");
        t1 = bh_call(t0, 2, (Value[]){K[98], K[5]}, "接受");
        if (bh_truthy(t1)) {
            bh_line = 474;
            t2 = bh_chk(G[90], "解析表达式");
            t3 = bh_call(t2, 0, NULL, "解析表达式");
            if (!L[1].tag && or1) bh_modify_outer("条件"); L[1] = t3;
            bh_line = 475;
            if (L[2].tag) t2 = L[2]; else { or2 = 1; t2 = bh_chk(G[98], "分支们"); }
            T[0] = t2;
            if (L[1].tag) t3 = L[1]; else { or1 = 1; t3 = bh_chk(G[97], "条件"); }
            T[1] = t3;
            t4 = bh_chk(G[94], "解析块");
            t5 = bh_call(t4, 0, NULL, "解析块");
            t6 = bh_list_make(2, (Value[]){T[1], t5});
            t7 = bi_append(2, (Value[]){T[0], t6});
        } else {
            bh_line = 476;
            t2 = bh_chk(G[64], "接受");
            t3 = bh_call(t2, 2, (Value[]){K[98], K[4]}, "接受");
            if (bh_truthy(t3)) {
                bh_line = 477;
                t4 = bh_chk(G[64], "接受");
                t5 = bh_call(t4, 2, (Value[]){K[98], K[3]}, "接受");
                if (bh_truthy(t5)) {
                    bh_line = 478;
                    t6 = bh_chk(G[90], "解析表达式");
                    t7 = bh_call(t6, 0, NULL, "解析表达式");
                    if (!L[1].tag && or1) bh_modify_outer("条件"); L[1] = t7;
                    bh_line = 479;
                    if (L[2].tag) t6 = L[2]; else { or2 = 1; t6 = bh_chk(G[98], "分支们"); }
                    T[0] = t6;
                    if (L[1].tag) t7 = L[1]; else { or1 = 1; t7 = bh_chk(G[97], "条件"); }
                    T[1] = t7;
                    t8 = bh_chk(G[94], "解析块");
                    t9 = bh_call(t8, 0, NULL, "解析块");
                    t10 = bh_list_make(2, (Value[]){T[1], t9});
                    t11 = bi_append(2, (Value[]){T[0], t10});
                } else {
                    bh_line = 481;
                    t6 = bh_chk(G[94], "解析块");
                    t7 = bh_call(t6, 0, NULL, "解析块");
                    if (!L[3].tag && or3) bh_modify_outer("别的"); L[3] = t7;
                    bh_line = 482;
                    break;
                }
            } else {
                bh_line = 484;
                break;
            }
        }
    }
    bh_line = 485;
    if (L[0].tag) t0 = L[0]; else { or0 = 1; t0 = bh_chk(G[68], "行"); }
    if (L[2].tag) t1 = L[2]; else { or2 = 1; t1 = bh_chk(G[98], "分支们"); }
    if (L[3].tag) t2 = L[3]; else { or3 = 1; t2 = bh_chk(G[99], "别的"); }
    t3 = bh_list_make(4, (Value[]){K[146], t0, t1, t2});
    BH_RETURN(fr, t3);
    BH_RETURN(fr, BH_NIL);
}

static Value fn28(Frame *up, Value *a) {    /* 函数 解析当，第 488 行 */
    struct { BhRoots h; Value v[4]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 4);
    L = fr.v; T = fr.v + 2;
    Value t0, t1, t2, t3, t4;
    char or0 = 0;
    char or1 = 0;
    BH_SAFEPOINT();
    bh_line = 489;
    t0 = bh_chk(G[62], "前进");
    t1 = bh_call(t0, 0, NULL, "前进");
    t2 = bh_index(t1, bh_int(2LL));
    if (!L[0].tag && or0) bh_modify_outer("行"); L[0] = t2;
    bh_line = 490;
    t0 = bh_chk(G[90], "解析表达式");
    t1 = bh_call(t0, 0, NULL, "解析表达式");
    if (!L[1].tag && or1) bh_modify_outer("条件"); L[1] = t1;
    bh_line = 491;
    if (L[0].tag) t0 = L[0]; else { or0 = 1; t0 = bh_chk(G[68], "行"); }
    T[0] = t0;
    if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[97], "条件"); }
    T[1] = t1;
    t2 = bh_chk(G[96], "解析循环体");
    t3 = bh_call(t2, 0, NULL, "解析循环体");
    t4 = bh_list_make(4, (Value[]){K[147], T[0], T[1], t3});
    BH_RETURN(fr, t4);
    BH_RETURN(fr, BH_NIL);
}

static Value fn29(Frame *up, Value *a) {    /* 函数 解析对于，第 494 行 */
    struct { BhRoots h; Value v[6]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 6);
    L = fr.v; T = fr.v + 3;
    Value t0, t1, t2, t3, t4, t5, t6, t7;
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    BH_SAFEPOINT();
    bh_line = 495;
    t0 = bh_chk(G[62], "前进");
    t1 = bh_call(t0, 0, NULL, "前进");
    t2 = bh_index(t1, bh_int(2LL));
    if (!L[0].tag && or0) bh_modify_outer("行"); L[0] = t2;
    bh_line = 496;
    t0 = bh_chk(G[90], "解析表达式");
    t1 = bh_call(t0, 0, NULL, "解析表达式");
    if (!L[1].tag && or1) bh_modify_outer("来源"); L[1] = t1;
    bh_line = 497;
    {
        t1 = bh_chk(G[64], "接受");
        t2 = bh_call(t1, 2, (Value[]){K[98], K[8]}, "接受");
        if (bh_truthy(t2)) {
            t0 = t2;
        } else {
            bh_line = 497;
            t3 = bh_chk(G[64], "接受");
            t4 = bh_call(t3, 2, (Value[]){K[98], K[9]}, "接受");
            t0 = t4;
        }
    }
    t5 = bh_bool(!bh_truthy(t0));
    if (bh_truthy(t5)) {
        bh_line = 498;
        t6 = bh_chk(G[70], "语法错");
        t7 = bh_call(t6, 1, (Value[]){K[148]}, "语法错");
    } else {
    }
    bh_line = 499;
    t0 = bh_chk(G[73], "期待名字");
    t1 = bh_call(t0, 1, (Value[]){K[139]}, "期待名字");
    if (!L[2].tag && or2) bh_modify_outer("名"); L[2] = t1;
    bh_line = 500;
    if (L[0].tag) t0 = L[0]; else { or0 = 1; t0 = bh_chk(G[68], "行"); }
    T[0] = t0;
    if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[100], "来源"); }
    T[1] = t1;
    if (L[2].tag) t2 = L[2]; else { or2 = 1; t2 = bh_chk(G[89], "名"); }
    T[2] = t2;
    t3 = bh_chk(G[96], "解析循环体");
    t4 = bh_call(t3, 0, NULL, "解析循环体");
    t5 = bh_list_make(5, (Value[]){K[149], T[0], T[1], T[2], t4});
    BH_RETURN(fr, t5);
    BH_RETURN(fr, BH_NIL);
}

static Value fn30(Frame *up, Value *a) {    /* 函数 解析重复，第 503 行 */
    struct { BhRoots h; Value v[4]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 4);
    L = fr.v; T = fr.v + 2;
    Value t0, t1, t2, t3, t4;
    char or0 = 0;
    char or1 = 0;
    BH_SAFEPOINT();
    bh_line = 504;
    t0 = bh_chk(G[62], "前进");
    t1 = bh_call(t0, 0, NULL, "前进");
    t2 = bh_index(t1, bh_int(2LL));
    if (!L[0].tag && or0) bh_modify_outer("行"); L[0] = t2;
    bh_line = 505;
    t0 = bh_chk(G[90], "解析表达式");
    t1 = bh_call(t0, 0, NULL, "解析表达式");
    if (!L[1].tag && or1) bh_modify_outer("次数"); L[1] = t1;
    bh_line = 506;
    t0 = bh_chk(G[64], "接受");
    t1 = bh_call(t0, 2, (Value[]){K[98], K[11]}, "接受");
    t2 = bh_bool(!bh_truthy(t1));
    if (bh_truthy(t2)) {
        bh_line = 507;
        t3 = bh_chk(G[70], "语法错");
        t4 = bh_call(t3, 1, (Value[]){K[150]}, "语法错");
    } else {
    }
    bh_line = 508;
    if (L[0].tag) t0 = L[0]; else { or0 = 1; t0 = bh_chk(G[68], "行"); }
    T[0] = t0;
    if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[101], "次数"); }
    T[1] = t1;
    t2 = bh_chk(G[96], "解析循环体");
    t3 = bh_call(t2, 0, NULL, "解析循环体");
    t4 = bh_list_make(4, (Value[]){K[151], T[0], T[1], t3});
    BH_RETURN(fr, t4);
    BH_RETURN(fr, BH_NIL);
}

static Value fn31(Frame *up, Value *a) {    /* 函数 解析函数，第 511 行 */
    struct { BhRoots h; Value v[8]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 8);
    L = fr.v; T = fr.v + 8;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11;
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    char or5 = 0;
    char or6 = 0;
    char or7 = 0;
    char gf0 = 0;
    char gf1 = 0;
    BH_SAFEPOINT();
    bh_line = 512;
    gf0 = 1;
    gf1 = 1;
    bh_line = 513;
    t0 = bh_chk(G[62], "前进");
    t1 = bh_call(t0, 0, NULL, "前进");
    t2 = bh_index(t1, bh_int(2LL));
    if (!L[0].tag && or0) bh_modify_outer("行"); L[0] = t2;
    bh_line = 514;
    t0 = bh_chk(G[73], "期待名字");
    t1 = bh_call(t0, 1, (Value[]){K[152]}, "期待名字");
    if (!L[1].tag && or1) bh_modify_outer("名"); L[1] = t1;
    bh_line = 515;
    t0 = bh_chk(G[72], "期待");
    t1 = bh_call(t0, 2, (Value[]){K[97], K[101]}, "期待");
    bh_line = 516;
    t0 = bh_list_make(0, NULL);
    if (!L[2].tag && or2) bh_modify_outer("参数们"); L[2] = t0;
    bh_line = 517;
    for (;;) {
        BH_SAFEPOINT();
        bh_line = 517;
        t0 = bh_chk(G[63], "是这个");
        t1 = bh_call(t0, 2, (Value[]){K[97], K[102]}, "是这个");
        t2 = bh_bool(!bh_truthy(t1));
        if (!bh_truthy(t2)) break;
        bh_line = 518;
        t3 = bh_chk(G[73], "期待名字");
        t4 = bh_call(t3, 1, (Value[]){K[153]}, "期待名字");
        if (!L[3].tag && or3) bh_modify_outer("参数"); L[3] = t4;
        bh_line = 519;
        t3 = BH_FALSE;
        {
            if (L[2].tag) t4 = L[2]; else { or2 = 1; t4 = bh_chk(G[102], "参数们"); }
            if (L[3].tag) t5 = L[3]; else { or3 = 1; t5 = bh_chk(G[103], "参数"); }
            t6 = bi_find(2, (Value[]){t4, t5});
            if (bh_cmp(5, t6, bh_int(0LL))) {
                t3 = BH_TRUE;
            }
        }
        if (bh_truthy(t3)) {
            bh_line = 520;
            t7 = bh_chk(G[70], "语法错");
            if (L[3].tag) t8 = L[3]; else { or3 = 1; t8 = bh_chk(G[103], "参数"); }
            t9 = bh_add(K[154], t8);
            t10 = bh_add(t9, K[155]);
            t11 = bh_call(t7, 1, (Value[]){t10}, "语法错");
        } else {
        }
        bh_line = 521;
        if (L[2].tag) t3 = L[2]; else { or2 = 1; t3 = bh_chk(G[102], "参数们"); }
        if (L[3].tag) t4 = L[3]; else { or3 = 1; t4 = bh_chk(G[103], "参数"); }
        t5 = bi_append(2, (Value[]){t3, t4});
        bh_line = 522;
        t3 = bh_chk(G[64], "接受");
        t4 = bh_call(t3, 2, (Value[]){K[97], K[51]}, "接受");
        t5 = bh_bool(!bh_truthy(t4));
        if (bh_truthy(t5)) {
            bh_line = 523;
            break;
        } else {
        }
    }
    bh_line = 524;
    t0 = bh_chk(G[72], "期待");
    t1 = bh_call(t0, 2, (Value[]){K[97], K[102]}, "期待");
    bh_line = 526;
    if (gf0) t0 = bh_chk(G[58], "循环层数"); else if (L[5].tag) t0 = L[5]; else { or5 = 1; t0 = bh_chk(G[58], "循环层数"); }
    if (!L[4].tag && or4) bh_modify_outer("外面的循环"); L[4] = t0;
    bh_line = 527;
    if (gf0) G[58] = bh_int(0LL); else { if (!L[5].tag && or5) bh_modify_outer("循环层数"); L[5] = bh_int(0LL); }
    bh_line = 528;
    if (gf1) t0 = bh_chk(G[59], "函数层数"); else if (L[6].tag) t0 = L[6]; else { or6 = 1; t0 = bh_chk(G[59], "函数层数"); }
    t1 = bh_add(t0, bh_int(1LL));
    if (gf1) G[59] = t1; else { if (!L[6].tag && or6) bh_modify_outer("函数层数"); L[6] = t1; }
    bh_line = 529;
    t0 = bh_chk(G[94], "解析块");
    t1 = bh_call(t0, 0, NULL, "解析块");
    if (!L[7].tag && or7) bh_modify_outer("体"); L[7] = t1;
    bh_line = 530;
    if (gf1) t0 = bh_chk(G[59], "函数层数"); else if (L[6].tag) t0 = L[6]; else { or6 = 1; t0 = bh_chk(G[59], "函数层数"); }
    t1 = bh_sub(t0, bh_int(1LL));
    if (gf1) G[59] = t1; else { if (!L[6].tag && or6) bh_modify_outer("函数层数"); L[6] = t1; }
    bh_line = 531;
    if (L[4].tag) t0 = L[4]; else { or4 = 1; t0 = bh_chk(G[104], "外面的循环"); }
    if (gf0) G[58] = t0; else { if (!L[5].tag && or5) bh_modify_outer("循环层数"); L[5] = t0; }
    bh_line = 532;
    if (L[0].tag) t0 = L[0]; else { or0 = 1; t0 = bh_chk(G[68], "行"); }
    if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[89], "名"); }
    if (L[2].tag) t2 = L[2]; else { or2 = 1; t2 = bh_chk(G[102], "参数们"); }
    if (L[7].tag) t3 = L[7]; else { or7 = 1; t3 = bh_chk(G[95], "体"); }
    t4 = bh_list_make(5, (Value[]){K[156], t0, t1, t2, t3});
    BH_RETURN(fr, t4);
    BH_RETURN(fr, BH_NIL);
}

static Value fn32(Frame *up, Value *a) {    /* 函数 解析表达式，第 537 行 */
    struct { BhRoots h; Value v[1]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 1);
    L = fr.v; T = fr.v + 0;
    Value t0, t1;
    BH_SAFEPOINT();
    bh_line = 538;
    t0 = bh_chk(G[105], "解析或");
    t1 = bh_call(t0, 0, NULL, "解析或");
    BH_RETURN(fr, t1);
    BH_RETURN(fr, BH_NIL);
}

static Value fn33(Frame *up, Value *a) {    /* 函数 解析或，第 541 行 */
    struct { BhRoots h; Value v[4]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 4);
    L = fr.v; T = fr.v + 2;
    Value t0, t1, t2, t3, t4, t5, t6;
    char or0 = 0;
    char or1 = 0;
    BH_SAFEPOINT();
    bh_line = 542;
    t0 = bh_chk(G[106], "解析且");
    t1 = bh_call(t0, 0, NULL, "解析且");
    if (!L[0].tag && or0) bh_modify_outer("左"); L[0] = t1;
    bh_line = 543;
    for (;;) {
        BH_SAFEPOINT();
        bh_line = 543;
        t0 = bh_chk(G[63], "是这个");
        t1 = bh_call(t0, 2, (Value[]){K[97], K[36]}, "是这个");
        if (!bh_truthy(t1)) break;
        bh_line = 544;
        t2 = bh_chk(G[62], "前进");
        t3 = bh_call(t2, 0, NULL, "前进");
        t4 = bh_index(t3, bh_int(2LL));
        if (!L[1].tag && or1) bh_modify_outer("行"); L[1] = t4;
        bh_line = 545;
        if (L[1].tag) t2 = L[1]; else { or1 = 1; t2 = bh_chk(G[68], "行"); }
        T[0] = t2;
        if (L[0].tag) t3 = L[0]; else { or0 = 1; t3 = bh_chk(G[107], "左"); }
        T[1] = t3;
        t4 = bh_chk(G[106], "解析且");
        t5 = bh_call(t4, 0, NULL, "解析且");
        t6 = bh_list_make(4, (Value[]){K[36], T[0], T[1], t5});
        if (!L[0].tag && or0) bh_modify_outer("左"); L[0] = t6;
    }
    bh_line = 546;
    if (L[0].tag) t0 = L[0]; else { or0 = 1; t0 = bh_chk(G[107], "左"); }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn34(Frame *up, Value *a) {    /* 函数 解析且，第 549 行 */
    struct { BhRoots h; Value v[4]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 4);
    L = fr.v; T = fr.v + 2;
    Value t0, t1, t2, t3, t4, t5, t6;
    char or0 = 0;
    char or1 = 0;
    BH_SAFEPOINT();
    bh_line = 550;
    t0 = bh_chk(G[108], "解析非");
    t1 = bh_call(t0, 0, NULL, "解析非");
    if (!L[0].tag && or0) bh_modify_outer("左"); L[0] = t1;
    bh_line = 551;
    for (;;) {
        BH_SAFEPOINT();
        bh_line = 551;
        t0 = bh_chk(G[63], "是这个");
        t1 = bh_call(t0, 2, (Value[]){K[97], K[35]}, "是这个");
        if (!bh_truthy(t1)) break;
        bh_line = 552;
        t2 = bh_chk(G[62], "前进");
        t3 = bh_call(t2, 0, NULL, "前进");
        t4 = bh_index(t3, bh_int(2LL));
        if (!L[1].tag && or1) bh_modify_outer("行"); L[1] = t4;
        bh_line = 553;
        if (L[1].tag) t2 = L[1]; else { or1 = 1; t2 = bh_chk(G[68], "行"); }
        T[0] = t2;
        if (L[0].tag) t3 = L[0]; else { or0 = 1; t3 = bh_chk(G[107], "左"); }
        T[1] = t3;
        t4 = bh_chk(G[108], "解析非");
        t5 = bh_call(t4, 0, NULL, "解析非");
        t6 = bh_list_make(4, (Value[]){K[35], T[0], T[1], t5});
        if (!L[0].tag && or0) bh_modify_outer("左"); L[0] = t6;
    }
    bh_line = 554;
    if (L[0].tag) t0 = L[0]; else { or0 = 1; t0 = bh_chk(G[107], "左"); }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn35(Frame *up, Value *a) {    /* 函数 解析非，第 557 行 */
    struct { BhRoots h; Value v[2]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 2);
    L = fr.v; T = fr.v + 1;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8;
    char or0 = 0;
    BH_SAFEPOINT();
    bh_line = 558;
    {
        t1 = bh_chk(G[63], "是这个");
        t2 = bh_call(t1, 2, (Value[]){K[97], K[37]}, "是这个");
        if (bh_truthy(t2)) {
            t0 = t2;
        } else {
            bh_line = 558;
            t3 = bh_chk(G[63], "是这个");
            t4 = bh_call(t3, 2, (Value[]){K[97], K[157]}, "是这个");
            t0 = t4;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 559;
        t5 = bh_chk(G[62], "前进");
        t6 = bh_call(t5, 0, NULL, "前进");
        t7 = bh_index(t6, bh_int(2LL));
        if (!L[0].tag && or0) bh_modify_outer("行"); L[0] = t7;
        bh_line = 560;
        if (L[0].tag) t5 = L[0]; else { or0 = 1; t5 = bh_chk(G[68], "行"); }
        T[0] = t5;
        t6 = bh_chk(G[108], "解析非");
        t7 = bh_call(t6, 0, NULL, "解析非");
        t8 = bh_list_make(3, (Value[]){K[37], T[0], t7});
        BH_RETURN(fr, t8);
    } else {
    }
    bh_line = 561;
    t0 = bh_chk(G[109], "解析比较");
    t1 = bh_call(t0, 0, NULL, "解析比较");
    BH_RETURN(fr, t1);
    BH_RETURN(fr, BH_NIL);
}

static Value fn36(Frame *up, Value *a) {    /* 函数 解析比较，第 564 行 */
    struct { BhRoots h; Value v[7]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 7);
    L = fr.v; T = fr.v + 5;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15;
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    BH_SAFEPOINT();
    bh_line = 565;
    t0 = bh_chk(G[110], "解析加减");
    t1 = bh_call(t0, 0, NULL, "解析加减");
    if (!L[0].tag && or0) bh_modify_outer("首"); L[0] = t1;
    bh_line = 566;
    t0 = bh_chk(G[60], "看");
    t1 = bh_call(t0, 0, NULL, "看");
    t2 = bh_index(t1, bh_int(2LL));
    if (!L[1].tag && or1) bh_modify_outer("行"); L[1] = t2;
    bh_line = 567;
    t0 = bh_list_make(0, NULL);
    if (!L[2].tag && or2) bh_modify_outer("其余"); L[2] = t0;
    bh_line = 568;
    for (;;) {
        BH_SAFEPOINT();
        if (!bh_truthy(BH_TRUE)) break;
        bh_line = 569;
        t0 = bh_chk(G[60], "看");
        t1 = bh_call(t0, 0, NULL, "看");
        if (!L[3].tag && or3) bh_modify_outer("这个"); L[3] = t1;
        bh_line = 570;
        {
            t1 = BH_FALSE;
            {
                if (L[3].tag) t2 = L[3]; else { or3 = 1; t2 = bh_chk(G[61], "这个"); }
                t3 = bh_index(t2, bh_int(0LL));
                if (bh_cmp(0, t3, K[97])) {
                    t1 = BH_TRUE;
                }
            }
            if (bh_truthy(t1)) {
                t4 = BH_FALSE;
                {
                    bh_line = 570;
                    t5 = bh_chk(G[16], "比较符号");
                    if (L[3].tag) t6 = L[3]; else { or3 = 1; t6 = bh_chk(G[61], "这个"); }
                    t7 = bh_index(t6, bh_int(1LL));
                    t8 = bi_find(2, (Value[]){t5, t7});
                    if (bh_cmp(5, t8, bh_int(0LL))) {
                        t4 = BH_TRUE;
                    }
                }
                t0 = t4;
            } else {
                t0 = t1;
            }
        }
        if (bh_truthy(t0)) {
            bh_line = 571;
            if (L[3].tag) t9 = L[3]; else { or3 = 1; t9 = bh_chk(G[61], "这个"); }
            t10 = bh_index(t9, bh_int(1LL));
            if (!L[4].tag && or4) bh_modify_outer("符"); L[4] = t10;
        } else {
            {
                t10 = BH_FALSE;
                {
                    bh_line = 572;
                    if (L[3].tag) t11 = L[3]; else { or3 = 1; t11 = bh_chk(G[61], "这个"); }
                    t12 = bh_index(t11, bh_int(0LL));
                    if (bh_cmp(0, t12, K[97])) {
                        t10 = BH_TRUE;
                    }
                }
                if (bh_truthy(t10)) {
                    t13 = BH_FALSE;
                    {
                        bh_line = 572;
                        if (L[3].tag) t14 = L[3]; else { or3 = 1; t14 = bh_chk(G[61], "这个"); }
                        t15 = bh_index(t14, bh_int(1LL));
                        if (bh_cmp(0, t15, K[37])) {
                            t13 = BH_TRUE;
                        }
                    }
                    t9 = t13;
                } else {
                    t9 = t10;
                }
            }
            if (bh_truthy(t9)) {
                bh_line = 573;
                if (!L[4].tag && or4) bh_modify_outer("符"); L[4] = K[34];
            } else {
                bh_line = 575;
                break;
            }
        }
        bh_line = 576;
        t0 = bh_chk(G[62], "前进");
        t1 = bh_call(t0, 0, NULL, "前进");
        bh_line = 577;
        if (L[2].tag) t0 = L[2]; else { or2 = 1; t0 = bh_chk(G[111], "其余"); }
        T[0] = t0;
        if (L[4].tag) t1 = L[4]; else { or4 = 1; t1 = bh_chk(G[92], "符"); }
        T[1] = t1;
        t2 = bh_chk(G[110], "解析加减");
        t3 = bh_call(t2, 0, NULL, "解析加减");
        t4 = bh_list_make(2, (Value[]){T[1], t3});
        t5 = bi_append(2, (Value[]){T[0], t4});
    }
    bh_line = 578;
    t0 = BH_FALSE;
    {
        if (L[2].tag) t1 = L[2]; else { or2 = 1; t1 = bh_chk(G[111], "其余"); }
        t2 = bi_len(1, (Value[]){t1});
        if (bh_cmp(0, t2, bh_int(0LL))) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 579;
        if (L[0].tag) t3 = L[0]; else { or0 = 1; t3 = bh_chk(G[112], "首"); }
        BH_RETURN(fr, t3);
    } else {
    }
    bh_line = 580;
    if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[68], "行"); }
    if (L[0].tag) t1 = L[0]; else { or0 = 1; t1 = bh_chk(G[112], "首"); }
    if (L[2].tag) t2 = L[2]; else { or2 = 1; t2 = bh_chk(G[111], "其余"); }
    t3 = bh_list_make(4, (Value[]){K[158], t0, t1, t2});
    BH_RETURN(fr, t3);
    BH_RETURN(fr, BH_NIL);
}

static Value fn37(Frame *up, Value *a) {    /* 函数 解析加减，第 583 行 */
    struct { BhRoots h; Value v[5]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 5);
    L = fr.v; T = fr.v + 2;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17, t18, t19, t20, t21;
    char or0 = 0;
    char or1 = 0;
    BH_SAFEPOINT();
    bh_line = 584;
    t0 = bh_chk(G[113], "解析乘除");
    t1 = bh_call(t0, 0, NULL, "解析乘除");
    if (!L[0].tag && or0) bh_modify_outer("左"); L[0] = t1;
    bh_line = 585;
    for (;;) {
        BH_SAFEPOINT();
        {
            t1 = BH_FALSE;
            {
                bh_line = 585;
                t2 = bh_chk(G[60], "看");
                t3 = bh_call(t2, 0, NULL, "看");
                t4 = bh_index(t3, bh_int(0LL));
                if (bh_cmp(0, t4, K[97])) {
                    t1 = BH_TRUE;
                }
            }
            if (bh_truthy(t1)) {
                {
                    t6 = BH_FALSE;
                    {
                        bh_line = 585;
                        t7 = bh_chk(G[60], "看");
                        t8 = bh_call(t7, 0, NULL, "看");
                        t9 = bh_index(t8, bh_int(1LL));
                        if (bh_cmp(0, t9, K[159])) {
                            t6 = BH_TRUE;
                        }
                    }
                    if (bh_truthy(t6)) {
                        t5 = t6;
                    } else {
                        t10 = BH_FALSE;
                        {
                            bh_line = 585;
                            t11 = bh_chk(G[60], "看");
                            t12 = bh_call(t11, 0, NULL, "看");
                            t13 = bh_index(t12, bh_int(1LL));
                            if (bh_cmp(0, t13, K[160])) {
                                t10 = BH_TRUE;
                            }
                        }
                        t5 = t10;
                    }
                }
                t0 = t5;
            } else {
                t0 = t1;
            }
        }
        if (!bh_truthy(t0)) break;
        bh_line = 586;
        t14 = bh_chk(G[62], "前进");
        t15 = bh_call(t14, 0, NULL, "前进");
        if (!L[1].tag && or1) bh_modify_outer("这个"); L[1] = t15;
        bh_line = 587;
        if (L[1].tag) t14 = L[1]; else { or1 = 1; t14 = bh_chk(G[61], "这个"); }
        t15 = bh_index(t14, bh_int(2LL));
        T[0] = t15;
        if (L[1].tag) t16 = L[1]; else { or1 = 1; t16 = bh_chk(G[61], "这个"); }
        t17 = bh_index(t16, bh_int(1LL));
        T[1] = t17;
        if (L[0].tag) t18 = L[0]; else { or0 = 1; t18 = bh_chk(G[107], "左"); }
        T[2] = t18;
        t19 = bh_chk(G[113], "解析乘除");
        t20 = bh_call(t19, 0, NULL, "解析乘除");
        t21 = bh_list_make(5, (Value[]){K[161], T[0], T[1], T[2], t20});
        if (!L[0].tag && or0) bh_modify_outer("左"); L[0] = t21;
    }
    bh_line = 588;
    if (L[0].tag) t0 = L[0]; else { or0 = 1; t0 = bh_chk(G[107], "左"); }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn38(Frame *up, Value *a) {    /* 函数 解析乘除，第 591 行 */
    struct { BhRoots h; Value v[6]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 6);
    L = fr.v; T = fr.v + 2;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17, t18;
    char or0 = 0;
    char or1 = 0;
    BH_SAFEPOINT();
    bh_line = 592;
    t0 = bh_chk(G[114], "解析一元");
    t1 = bh_call(t0, 0, NULL, "解析一元");
    if (!L[0].tag && or0) bh_modify_outer("左"); L[0] = t1;
    bh_line = 593;
    for (;;) {
        BH_SAFEPOINT();
        {
            t1 = BH_FALSE;
            {
                bh_line = 593;
                t2 = bh_chk(G[60], "看");
                t3 = bh_call(t2, 0, NULL, "看");
                t4 = bh_index(t3, bh_int(0LL));
                if (bh_cmp(0, t4, K[97])) {
                    t1 = BH_TRUE;
                }
            }
            if (bh_truthy(t1)) {
                t5 = BH_FALSE;
                {
                    t6 = bh_list_make(3, (Value[]){K[53], K[28], K[162]});
                    T[0] = t6;
                    bh_line = 593;
                    t7 = bh_chk(G[60], "看");
                    t8 = bh_call(t7, 0, NULL, "看");
                    t9 = bh_index(t8, bh_int(1LL));
                    t10 = bi_find(2, (Value[]){T[0], t9});
                    if (bh_cmp(5, t10, bh_int(0LL))) {
                        t5 = BH_TRUE;
                    }
                }
                t0 = t5;
            } else {
                t0 = t1;
            }
        }
        if (!bh_truthy(t0)) break;
        bh_line = 594;
        t11 = bh_chk(G[62], "前进");
        t12 = bh_call(t11, 0, NULL, "前进");
        if (!L[1].tag && or1) bh_modify_outer("这个"); L[1] = t12;
        bh_line = 595;
        if (L[1].tag) t11 = L[1]; else { or1 = 1; t11 = bh_chk(G[61], "这个"); }
        t12 = bh_index(t11, bh_int(2LL));
        T[1] = t12;
        if (L[1].tag) t13 = L[1]; else { or1 = 1; t13 = bh_chk(G[61], "这个"); }
        t14 = bh_index(t13, bh_int(1LL));
        T[2] = t14;
        if (L[0].tag) t15 = L[0]; else { or0 = 1; t15 = bh_chk(G[107], "左"); }
        T[3] = t15;
        t16 = bh_chk(G[114], "解析一元");
        t17 = bh_call(t16, 0, NULL, "解析一元");
        t18 = bh_list_make(5, (Value[]){K[161], T[1], T[2], T[3], t17});
        if (!L[0].tag && or0) bh_modify_outer("左"); L[0] = t18;
    }
    bh_line = 596;
    if (L[0].tag) t0 = L[0]; else { or0 = 1; t0 = bh_chk(G[107], "左"); }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn39(Frame *up, Value *a) {    /* 函数 解析一元，第 599 行 */
    struct { BhRoots h; Value v[2]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 2);
    L = fr.v; T = fr.v + 1;
    Value t0, t1, t2, t3, t4, t5;
    char or0 = 0;
    BH_SAFEPOINT();
    bh_line = 600;
    t0 = bh_chk(G[63], "是这个");
    t1 = bh_call(t0, 2, (Value[]){K[97], K[160]}, "是这个");
    if (bh_truthy(t1)) {
        bh_line = 601;
        t2 = bh_chk(G[62], "前进");
        t3 = bh_call(t2, 0, NULL, "前进");
        t4 = bh_index(t3, bh_int(2LL));
        if (!L[0].tag && or0) bh_modify_outer("行"); L[0] = t4;
        bh_line = 602;
        if (L[0].tag) t2 = L[0]; else { or0 = 1; t2 = bh_chk(G[68], "行"); }
        T[0] = t2;
        t3 = bh_chk(G[114], "解析一元");
        t4 = bh_call(t3, 0, NULL, "解析一元");
        t5 = bh_list_make(3, (Value[]){K[163], T[0], t4});
        BH_RETURN(fr, t5);
    } else {
    }
    bh_line = 603;
    t0 = bh_chk(G[115], "解析后缀");
    t1 = bh_call(t0, 0, NULL, "解析后缀");
    BH_RETURN(fr, t1);
    BH_RETURN(fr, BH_NIL);
}

static Value fn40(Frame *up, Value *a) {    /* 函数 解析后缀，第 606 行 */
    struct { BhRoots h; Value v[5]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 5);
    L = fr.v; T = fr.v + 3;
    Value t0, t1, t2, t3, t4, t5, t6, t7;
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    BH_SAFEPOINT();
    bh_line = 607;
    t0 = bh_chk(G[116], "解析基本");
    t1 = bh_call(t0, 0, NULL, "解析基本");
    if (!L[0].tag && or0) bh_modify_outer("节点"); L[0] = t1;
    bh_line = 608;
    for (;;) {
        BH_SAFEPOINT();
        if (!bh_truthy(BH_TRUE)) break;
        bh_line = 609;
        t0 = bh_chk(G[63], "是这个");
        t1 = bh_call(t0, 2, (Value[]){K[97], K[101]}, "是这个");
        if (bh_truthy(t1)) {
            bh_line = 610;
            t2 = bh_chk(G[62], "前进");
            t3 = bh_call(t2, 0, NULL, "前进");
            t4 = bh_index(t3, bh_int(2LL));
            if (!L[1].tag && or1) bh_modify_outer("行"); L[1] = t4;
            bh_line = 611;
            if (L[1].tag) t2 = L[1]; else { or1 = 1; t2 = bh_chk(G[68], "行"); }
            T[0] = t2;
            if (L[0].tag) t3 = L[0]; else { or0 = 1; t3 = bh_chk(G[117], "节点"); }
            T[1] = t3;
            t4 = bh_chk(G[118], "解析一串");
            t5 = bh_call(t4, 1, (Value[]){K[102]}, "解析一串");
            t6 = bh_list_make(4, (Value[]){K[164], T[0], T[1], t5});
            if (!L[0].tag && or0) bh_modify_outer("节点"); L[0] = t6;
        } else {
            bh_line = 612;
            t2 = bh_chk(G[63], "是这个");
            t3 = bh_call(t2, 2, (Value[]){K[97], K[49]}, "是这个");
            if (bh_truthy(t3)) {
                bh_line = 613;
                t4 = bh_chk(G[62], "前进");
                t5 = bh_call(t4, 0, NULL, "前进");
                t6 = bh_index(t5, bh_int(2LL));
                if (!L[1].tag && or1) bh_modify_outer("行"); L[1] = t6;
                bh_line = 614;
                t4 = bh_chk(G[90], "解析表达式");
                t5 = bh_call(t4, 0, NULL, "解析表达式");
                if (!L[2].tag && or2) bh_modify_outer("下标"); L[2] = t5;
                bh_line = 615;
                t4 = bh_chk(G[72], "期待");
                t5 = bh_call(t4, 2, (Value[]){K[97], K[50]}, "期待");
                bh_line = 616;
                if (L[1].tag) t4 = L[1]; else { or1 = 1; t4 = bh_chk(G[68], "行"); }
                if (L[0].tag) t5 = L[0]; else { or0 = 1; t5 = bh_chk(G[117], "节点"); }
                if (L[2].tag) t6 = L[2]; else { or2 = 1; t6 = bh_chk(G[19], "下标"); }
                t7 = bh_list_make(4, (Value[]){K[141], t4, t5, t6});
                if (!L[0].tag && or0) bh_modify_outer("节点"); L[0] = t7;
            } else {
                bh_line = 618;
                if (L[0].tag) t4 = L[0]; else { or0 = 1; t4 = bh_chk(G[117], "节点"); }
                BH_RETURN(fr, t4);
            }
        }
    }
    BH_RETURN(fr, BH_NIL);
}

static Value fn41(Frame *up, Value *a) {    /* 函数 解析一串，第 621 行 */
    struct { BhRoots h; Value v[3]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 3);
    L = fr.v; T = fr.v + 2;
    Value t0, t1, t2, t3, t4, t5, t6;
    L[0] = a[0];    /* 收尾 */
    char or0 = 0;
    char or1 = 0;
    BH_SAFEPOINT();
    bh_line = 623;
    t0 = bh_list_make(0, NULL);
    if (!L[1].tag && or1) bh_modify_outer("项们"); L[1] = t0;
    bh_line = 624;
    for (;;) {
        BH_SAFEPOINT();
        bh_line = 624;
        t0 = bh_chk(G[63], "是这个");
        t1 = bh_call(t0, 2, (Value[]){K[97], L[0]}, "是这个");
        t2 = bh_bool(!bh_truthy(t1));
        if (!bh_truthy(t2)) break;
        bh_line = 625;
        if (L[1].tag) t3 = L[1]; else { or1 = 1; t3 = bh_chk(G[119], "项们"); }
        T[0] = t3;
        t4 = bh_chk(G[90], "解析表达式");
        t5 = bh_call(t4, 0, NULL, "解析表达式");
        t6 = bi_append(2, (Value[]){T[0], t5});
        bh_line = 626;
        t3 = bh_chk(G[64], "接受");
        t4 = bh_call(t3, 2, (Value[]){K[97], K[51]}, "接受");
        t5 = bh_bool(!bh_truthy(t4));
        if (bh_truthy(t5)) {
            bh_line = 627;
            break;
        } else {
        }
    }
    bh_line = 628;
    t0 = bh_chk(G[72], "期待");
    t1 = bh_call(t0, 2, (Value[]){K[97], L[0]}, "期待");
    bh_line = 629;
    if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[119], "项们"); }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn42(Frame *up, Value *a) {    /* 函数 解析基本，第 632 行 */
    struct { BhRoots h; Value v[9]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 9);
    L = fr.v; T = fr.v + 7;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17, t18;
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    char or5 = 0;
    char or6 = 0;
    BH_SAFEPOINT();
    bh_line = 633;
    t0 = bh_chk(G[60], "看");
    t1 = bh_call(t0, 0, NULL, "看");
    if (!L[0].tag && or0) bh_modify_outer("这个"); L[0] = t1;
    bh_line = 634;
    if (L[0].tag) t0 = L[0]; else { or0 = 1; t0 = bh_chk(G[61], "这个"); }
    t1 = bh_index(t0, bh_int(0LL));
    if (!L[1].tag && or1) bh_modify_outer("种类"); L[1] = t1;
    bh_line = 635;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[96])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 636;
        t2 = bh_chk(G[62], "前进");
        t3 = bh_call(t2, 0, NULL, "前进");
        bh_line = 637;
        if (L[0].tag) t2 = L[0]; else { or0 = 1; t2 = bh_chk(G[61], "这个"); }
        t3 = bh_index(t2, bh_int(2LL));
        if (L[0].tag) t4 = L[0]; else { or0 = 1; t4 = bh_chk(G[61], "这个"); }
        t5 = bh_index(t4, bh_int(1LL));
        t6 = bh_list_make(3, (Value[]){K[165], t3, t5});
        BH_RETURN(fr, t6);
    } else {
    }
    bh_line = 638;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[93])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 639;
        t2 = bh_chk(G[62], "前进");
        t3 = bh_call(t2, 0, NULL, "前进");
        bh_line = 640;
        if (L[0].tag) t2 = L[0]; else { or0 = 1; t2 = bh_chk(G[61], "这个"); }
        t3 = bh_index(t2, bh_int(2LL));
        if (L[0].tag) t4 = L[0]; else { or0 = 1; t4 = bh_chk(G[61], "这个"); }
        t5 = bh_index(t4, bh_int(1LL));
        t6 = bh_list_make(3, (Value[]){K[166], t3, t5});
        BH_RETURN(fr, t6);
    } else {
    }
    bh_line = 641;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[99])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 642;
        t2 = bh_chk(G[62], "前进");
        t3 = bh_call(t2, 0, NULL, "前进");
        bh_line = 643;
        if (L[0].tag) t2 = L[0]; else { or0 = 1; t2 = bh_chk(G[61], "这个"); }
        t3 = bh_index(t2, bh_int(2LL));
        if (L[0].tag) t4 = L[0]; else { or0 = 1; t4 = bh_chk(G[61], "这个"); }
        t5 = bh_index(t4, bh_int(1LL));
        t6 = bh_list_make(3, (Value[]){K[128], t3, t5});
        BH_RETURN(fr, t6);
    } else {
    }
    bh_line = 644;
    {
        t1 = BH_FALSE;
        {
            if (L[1].tag) t2 = L[1]; else { or1 = 1; t2 = bh_chk(G[65], "种类"); }
            if (bh_cmp(0, t2, K[98])) {
                t1 = BH_TRUE;
            }
        }
        if (bh_truthy(t1)) {
            {
                {
                    t5 = BH_FALSE;
                    {
                        bh_line = 644;
                        if (L[0].tag) t6 = L[0]; else { or0 = 1; t6 = bh_chk(G[61], "这个"); }
                        t7 = bh_index(t6, bh_int(1LL));
                        if (bh_cmp(0, t7, K[14])) {
                            t5 = BH_TRUE;
                        }
                    }
                    if (bh_truthy(t5)) {
                        t4 = t5;
                    } else {
                        t8 = BH_FALSE;
                        {
                            bh_line = 644;
                            if (L[0].tag) t9 = L[0]; else { or0 = 1; t9 = bh_chk(G[61], "这个"); }
                            t10 = bh_index(t9, bh_int(1LL));
                            if (bh_cmp(0, t10, K[15])) {
                                t8 = BH_TRUE;
                            }
                        }
                        t4 = t8;
                    }
                }
                if (bh_truthy(t4)) {
                    t3 = t4;
                } else {
                    t11 = BH_FALSE;
                    {
                        bh_line = 644;
                        if (L[0].tag) t12 = L[0]; else { or0 = 1; t12 = bh_chk(G[61], "这个"); }
                        t13 = bh_index(t12, bh_int(1LL));
                        if (bh_cmp(0, t13, K[16])) {
                            t11 = BH_TRUE;
                        }
                    }
                    t3 = t11;
                }
            }
            t0 = t3;
        } else {
            t0 = t1;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 645;
        t14 = bh_chk(G[62], "前进");
        t15 = bh_call(t14, 0, NULL, "前进");
        bh_line = 646;
        if (L[0].tag) t14 = L[0]; else { or0 = 1; t14 = bh_chk(G[61], "这个"); }
        t15 = bh_index(t14, bh_int(2LL));
        if (L[0].tag) t16 = L[0]; else { or0 = 1; t16 = bh_chk(G[61], "这个"); }
        t17 = bh_index(t16, bh_int(1LL));
        t18 = bh_list_make(3, (Value[]){K[167], t15, t17});
        BH_RETURN(fr, t18);
    } else {
    }
    bh_line = 647;
    {
        t1 = BH_FALSE;
        {
            if (L[1].tag) t2 = L[1]; else { or1 = 1; t2 = bh_chk(G[65], "种类"); }
            if (bh_cmp(0, t2, K[97])) {
                t1 = BH_TRUE;
            }
        }
        if (bh_truthy(t1)) {
            t3 = BH_FALSE;
            {
                bh_line = 647;
                if (L[0].tag) t4 = L[0]; else { or0 = 1; t4 = bh_chk(G[61], "这个"); }
                t5 = bh_index(t4, bh_int(1LL));
                if (bh_cmp(0, t5, K[101])) {
                    t3 = BH_TRUE;
                }
            }
            t0 = t3;
        } else {
            t0 = t1;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 648;
        t6 = bh_chk(G[62], "前进");
        t7 = bh_call(t6, 0, NULL, "前进");
        bh_line = 649;
        t6 = bh_chk(G[90], "解析表达式");
        t7 = bh_call(t6, 0, NULL, "解析表达式");
        if (!L[2].tag && or2) bh_modify_outer("式"); L[2] = t7;
        bh_line = 650;
        t6 = bh_chk(G[72], "期待");
        t7 = bh_call(t6, 2, (Value[]){K[97], K[102]}, "期待");
        bh_line = 651;
        if (L[2].tag) t6 = L[2]; else { or2 = 1; t6 = bh_chk(G[93], "式"); }
        BH_RETURN(fr, t6);
    } else {
    }
    bh_line = 652;
    {
        t1 = BH_FALSE;
        {
            if (L[1].tag) t2 = L[1]; else { or1 = 1; t2 = bh_chk(G[65], "种类"); }
            if (bh_cmp(0, t2, K[97])) {
                t1 = BH_TRUE;
            }
        }
        if (bh_truthy(t1)) {
            t3 = BH_FALSE;
            {
                bh_line = 652;
                if (L[0].tag) t4 = L[0]; else { or0 = 1; t4 = bh_chk(G[61], "这个"); }
                t5 = bh_index(t4, bh_int(1LL));
                if (bh_cmp(0, t5, K[49])) {
                    t3 = BH_TRUE;
                }
            }
            t0 = t3;
        } else {
            t0 = t1;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 653;
        t6 = bh_chk(G[62], "前进");
        t7 = bh_call(t6, 0, NULL, "前进");
        bh_line = 654;
        t6 = bh_chk(G[64], "接受");
        t7 = bh_call(t6, 2, (Value[]){K[97], K[116]}, "接受");
        if (bh_truthy(t7)) {
            bh_line = 655;
            t8 = bh_chk(G[72], "期待");
            t9 = bh_call(t8, 2, (Value[]){K[97], K[50]}, "期待");
            bh_line = 656;
            if (L[0].tag) t8 = L[0]; else { or0 = 1; t8 = bh_chk(G[61], "这个"); }
            t9 = bh_index(t8, bh_int(2LL));
            t10 = bh_list_make(0, NULL);
            t11 = bh_list_make(3, (Value[]){K[168], t9, t10});
            BH_RETURN(fr, t11);
        } else {
        }
        bh_line = 657;
        t6 = bh_chk(G[64], "接受");
        t7 = bh_call(t6, 2, (Value[]){K[97], K[50]}, "接受");
        if (bh_truthy(t7)) {
            bh_line = 658;
            if (L[0].tag) t8 = L[0]; else { or0 = 1; t8 = bh_chk(G[61], "这个"); }
            t9 = bh_index(t8, bh_int(2LL));
            t10 = bh_list_make(0, NULL);
            t11 = bh_list_make(3, (Value[]){K[169], t9, t10});
            BH_RETURN(fr, t11);
        } else {
        }
        bh_line = 659;
        t6 = bh_chk(G[90], "解析表达式");
        t7 = bh_call(t6, 0, NULL, "解析表达式");
        if (!L[3].tag && or3) bh_modify_outer("首"); L[3] = t7;
        bh_line = 660;
        t6 = bh_chk(G[64], "接受");
        t7 = bh_call(t6, 2, (Value[]){K[97], K[116]}, "接受");
        t8 = bh_bool(!bh_truthy(t7));
        if (bh_truthy(t8)) {
            bh_line = 661;
            if (L[3].tag) t9 = L[3]; else { or3 = 1; t9 = bh_chk(G[112], "首"); }
            t10 = bh_list_make(1, (Value[]){t9});
            if (!L[4].tag && or4) bh_modify_outer("项们"); L[4] = t10;
            bh_line = 662;
            t9 = bh_chk(G[64], "接受");
            t10 = bh_call(t9, 2, (Value[]){K[97], K[51]}, "接受");
            if (bh_truthy(t10)) {
                bh_line = 663;
                t11 = bh_chk(G[76], "并入");
                T[0] = t11;
                if (L[4].tag) t12 = L[4]; else { or4 = 1; t12 = bh_chk(G[119], "项们"); }
                T[1] = t12;
                t13 = bh_chk(G[118], "解析一串");
                t14 = bh_call(t13, 1, (Value[]){K[50]}, "解析一串");
                t15 = bh_call(T[0], 2, (Value[]){T[1], t14}, "并入");
            } else {
                bh_line = 665;
                t11 = bh_chk(G[72], "期待");
                t12 = bh_call(t11, 2, (Value[]){K[97], K[50]}, "期待");
            }
            bh_line = 666;
            if (L[0].tag) t9 = L[0]; else { or0 = 1; t9 = bh_chk(G[61], "这个"); }
            t10 = bh_index(t9, bh_int(2LL));
            if (L[4].tag) t11 = L[4]; else { or4 = 1; t11 = bh_chk(G[119], "项们"); }
            t12 = bh_list_make(3, (Value[]){K[169], t10, t11});
            BH_RETURN(fr, t12);
        } else {
        }
        bh_line = 667;
        if (L[3].tag) t6 = L[3]; else { or3 = 1; t6 = bh_chk(G[112], "首"); }
        T[0] = t6;
        t7 = bh_chk(G[90], "解析表达式");
        t8 = bh_call(t7, 0, NULL, "解析表达式");
        t9 = bh_list_make(2, (Value[]){T[0], t8});
        t10 = bh_list_make(1, (Value[]){t9});
        if (!L[5].tag && or5) bh_modify_outer("对们"); L[5] = t10;
        bh_line = 668;
        for (;;) {
            BH_SAFEPOINT();
            bh_line = 668;
            t6 = bh_chk(G[64], "接受");
            t7 = bh_call(t6, 2, (Value[]){K[97], K[51]}, "接受");
            if (!bh_truthy(t7)) break;
            bh_line = 669;
            t8 = bh_chk(G[63], "是这个");
            t9 = bh_call(t8, 2, (Value[]){K[97], K[50]}, "是这个");
            if (bh_truthy(t9)) {
                bh_line = 670;
                break;
            } else {
            }
            bh_line = 671;
            t8 = bh_chk(G[90], "解析表达式");
            t9 = bh_call(t8, 0, NULL, "解析表达式");
            if (!L[6].tag && or6) bh_modify_outer("键"); L[6] = t9;
            bh_line = 672;
            t8 = bh_chk(G[72], "期待");
            t9 = bh_call(t8, 2, (Value[]){K[97], K[116]}, "期待");
            bh_line = 673;
            if (L[5].tag) t8 = L[5]; else { or5 = 1; t8 = bh_chk(G[120], "对们"); }
            T[0] = t8;
            if (L[6].tag) t9 = L[6]; else { or6 = 1; t9 = bh_chk(G[121], "键"); }
            T[1] = t9;
            t10 = bh_chk(G[90], "解析表达式");
            t11 = bh_call(t10, 0, NULL, "解析表达式");
            t12 = bh_list_make(2, (Value[]){T[1], t11});
            t13 = bi_append(2, (Value[]){T[0], t12});
        }
        bh_line = 674;
        t6 = bh_chk(G[72], "期待");
        t7 = bh_call(t6, 2, (Value[]){K[97], K[50]}, "期待");
        bh_line = 675;
        if (L[0].tag) t6 = L[0]; else { or0 = 1; t6 = bh_chk(G[61], "这个"); }
        t7 = bh_index(t6, bh_int(2LL));
        if (L[5].tag) t8 = L[5]; else { or5 = 1; t8 = bh_chk(G[120], "对们"); }
        t9 = bh_list_make(3, (Value[]){K[168], t7, t8});
        BH_RETURN(fr, t9);
    } else {
    }
    bh_line = 676;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[98])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 677;
        t2 = bh_chk(G[70], "语法错");
        if (L[0].tag) t3 = L[0]; else { or0 = 1; t3 = bh_chk(G[61], "这个"); }
        t4 = bh_index(t3, bh_int(3LL));
        t5 = bh_add(K[63], t4);
        t6 = bh_add(t5, K[170]);
        t7 = bh_call(t2, 1, (Value[]){t6}, "语法错");
    } else {
    }
    bh_line = 678;
    t0 = bh_chk(G[70], "语法错");
    T[0] = t0;
    t1 = bh_chk(G[66], "说");
    if (L[0].tag) t2 = L[0]; else { or0 = 1; t2 = bh_chk(G[61], "这个"); }
    t3 = bh_call(t1, 1, (Value[]){t2}, "说");
    t4 = bh_add(K[171], t3);
    t5 = bh_call(T[0], 1, (Value[]){t4}, "语法错");
    BH_RETURN(fr, BH_NIL);
}

static Value fn43(Frame *up, Value *a) {    /* 函数 新环境，第 733 行 */
    Value L[6] = {{0}};
    Value t0, t1, t2;
    L[0] = a[0];    /* 外层 */
    char or0 = 0;
    L[1] = a[1];    /* 局部 */
    char or1 = 0;
    L[2] = a[2];    /* 参数数 */
    char or2 = 0;
    L[3] = a[3];    /* 声明 */
    char or3 = 0;
    L[4] = a[4];    /* 有内层 */
    char or4 = 0;
    L[5] = a[5];    /* 是模块 */
    char or5 = 0;
    bh_line = 734;
    t0 = bh_list_make(0, NULL);
    t1 = bh_neg(bh_int(1LL));
    t2 = bh_list_make(15, (Value[]){t0, L[1], L[2], L[3], L[4], bh_int(0LL), t1, L[0], L[5], bh_int(1LL), bh_int(0LL), bh_int(0LL), bh_int(0LL), bh_int(0LL), BH_FALSE});
    return t2;
    return BH_NIL;
}

static Value fn44(Frame *up, Value *a) {    /* 函数 出，第 737 行 */
    Value L[1] = {{0}};
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8;
    L[0] = a[0];    /* 文本 */
    char or0 = 0;
    bh_line = 738;
    t0 = bh_chk(G[126], "环境");
    t1 = bh_chk(G[131], "代码位");
    t2 = bh_index(t0, t1);
    t3 = bh_chk(G[126], "环境");
    t4 = bh_chk(G[140], "缩进位");
    t5 = bh_index(t3, t4);
    t6 = bh_mul(K[172], t5);
    t7 = bh_add(t6, L[0]);
    t8 = bi_append(2, (Value[]){t2, t7});
    return BH_NIL;
}

static Value fn45(Frame *up, Value *a) {    /* 函数 开块，第 741 行 */
    struct { BhRoots h; Value v[1]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 1);
    L = fr.v; T = fr.v + 1;
    Value t0, t1, t2, t3, t4, t5;
    L[0] = a[0];    /* 文本 */
    char or0 = 0;
    BH_SAFEPOINT();
    bh_line = 742;
    t0 = bh_chk(G[147], "出");
    t1 = bh_call(t0, 1, (Value[]){L[0]}, "出");
    bh_line = 743;
    t0 = bh_chk(G[126], "环境");
    t1 = bh_chk(G[140], "缩进位");
    t2 = bh_index(t0, t1);
    t3 = bh_add(t2, bh_int(1LL));
    t4 = bh_chk(G[126], "环境");
    t5 = bh_chk(G[140], "缩进位");
    bh_set_index(t4, t5, t3);
    BH_RETURN(fr, BH_NIL);
}

static Value fn46(Frame *up, Value *a) {    /* 函数 关块，第 746 行 */
    struct { BhRoots h; Value v[1]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 1);
    L = fr.v; T = fr.v + 0;
    Value t0, t1, t2, t3, t4, t5;
    BH_SAFEPOINT();
    bh_line = 747;
    t0 = bh_chk(G[126], "环境");
    t1 = bh_chk(G[140], "缩进位");
    t2 = bh_index(t0, t1);
    t3 = bh_sub(t2, bh_int(1LL));
    t4 = bh_chk(G[126], "环境");
    t5 = bh_chk(G[140], "缩进位");
    bh_set_index(t4, t5, t3);
    bh_line = 748;
    t0 = bh_chk(G[147], "出");
    t1 = bh_call(t0, 1, (Value[]){K[173]}, "出");
    bh_line = 749;
    t0 = bh_neg(bh_int(1LL));
    t1 = bh_chk(G[126], "环境");
    t2 = bh_chk(G[137], "行位");
    bh_set_index(t1, t2, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn47(Frame *up, Value *a) {    /* 函数 否则块，第 752 行 */
    struct { BhRoots h; Value v[1]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 1);
    L = fr.v; T = fr.v + 0;
    Value t0, t1, t2, t3, t4, t5;
    BH_SAFEPOINT();
    bh_line = 753;
    t0 = bh_chk(G[126], "环境");
    t1 = bh_chk(G[140], "缩进位");
    t2 = bh_index(t0, t1);
    t3 = bh_sub(t2, bh_int(1LL));
    t4 = bh_chk(G[126], "环境");
    t5 = bh_chk(G[140], "缩进位");
    bh_set_index(t4, t5, t3);
    bh_line = 754;
    t0 = bh_chk(G[147], "出");
    t1 = bh_call(t0, 1, (Value[]){K[174]}, "出");
    bh_line = 755;
    t0 = bh_chk(G[126], "环境");
    t1 = bh_chk(G[140], "缩进位");
    t2 = bh_index(t0, t1);
    t3 = bh_add(t2, bh_int(1LL));
    t4 = bh_chk(G[126], "环境");
    t5 = bh_chk(G[140], "缩进位");
    bh_set_index(t4, t5, t3);
    bh_line = 756;
    t0 = bh_neg(bh_int(1LL));
    t1 = bh_chk(G[126], "环境");
    t2 = bh_chk(G[137], "行位");
    bh_set_index(t1, t2, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn48(Frame *up, Value *a) {    /* 函数 新临时，第 759 行 */
    Value L[1] = {{0}};
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11;
    char or0 = 0;
    bh_line = 761;
    t0 = bh_chk(G[126], "环境");
    t1 = bh_chk(G[136], "临时位");
    t2 = bh_index(t0, t1);
    if (!L[0].tag && or0) bh_modify_outer("格"); L[0] = t2;
    bh_line = 762;
    t0 = bh_chk(G[126], "环境");
    t1 = bh_chk(G[136], "临时位");
    t2 = bh_index(t0, t1);
    t3 = bh_add(t2, bh_int(1LL));
    t4 = bh_chk(G[126], "环境");
    t5 = bh_chk(G[136], "临时位");
    bh_set_index(t4, t5, t3);
    bh_line = 763;
    t0 = BH_FALSE;
    {
        t1 = bh_chk(G[126], "环境");
        t2 = bh_chk(G[136], "临时位");
        t3 = bh_index(t1, t2);
        t4 = bh_chk(G[126], "环境");
        t5 = bh_chk(G[141], "最多位");
        t6 = bh_index(t4, t5);
        if (bh_cmp(3, t3, t6)) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 764;
        t7 = bh_chk(G[126], "环境");
        t8 = bh_chk(G[136], "临时位");
        t9 = bh_index(t7, t8);
        t10 = bh_chk(G[126], "环境");
        t11 = bh_chk(G[141], "最多位");
        bh_set_index(t10, t11, t9);
    } else {
    }
    bh_line = 765;
    if (L[0].tag) t0 = L[0]; else { or0 = 1; t0 = bh_chk(G[151], "格"); }
    t1 = bh_add(K[90], t0);
    return t1;
    return BH_NIL;
}

static Value fn49(Frame *up, Value *a) {    /* 函数 根临时，第 768 行 */
    Value L[1] = {{0}};
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11;
    char or0 = 0;
    bh_line = 770;
    t0 = bh_chk(G[126], "环境");
    t1 = bh_chk(G[145], "叶子位");
    t2 = bh_index(t0, t1);
    if (bh_truthy(t2)) {
        bh_line = 771;
        t3 = bi_eprint(1, (Value[]){K[175]});
        bh_line = 772;
        t3 = bi_exit(1, (Value[]){bh_int(3LL)});
    } else {
    }
    bh_line = 773;
    t0 = bh_chk(G[126], "环境");
    t1 = bh_chk(G[143], "根位");
    t2 = bh_index(t0, t1);
    if (!L[0].tag && or0) bh_modify_outer("格"); L[0] = t2;
    bh_line = 774;
    t0 = bh_chk(G[126], "环境");
    t1 = bh_chk(G[143], "根位");
    t2 = bh_index(t0, t1);
    t3 = bh_add(t2, bh_int(1LL));
    t4 = bh_chk(G[126], "环境");
    t5 = bh_chk(G[143], "根位");
    bh_set_index(t4, t5, t3);
    bh_line = 775;
    t0 = BH_FALSE;
    {
        t1 = bh_chk(G[126], "环境");
        t2 = bh_chk(G[143], "根位");
        t3 = bh_index(t1, t2);
        t4 = bh_chk(G[126], "环境");
        t5 = bh_chk(G[144], "最多根位");
        t6 = bh_index(t4, t5);
        if (bh_cmp(3, t3, t6)) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 776;
        t7 = bh_chk(G[126], "环境");
        t8 = bh_chk(G[143], "根位");
        t9 = bh_index(t7, t8);
        t10 = bh_chk(G[126], "环境");
        t11 = bh_chk(G[144], "最多根位");
        bh_set_index(t10, t11, t9);
    } else {
    }
    bh_line = 777;
    if (L[0].tag) t0 = L[0]; else { or0 = 1; t0 = bh_chk(G[151], "格"); }
    t1 = bh_add(K[176], t0);
    t2 = bh_add(t1, K[50]);
    return t2;
    return BH_NIL;
}

static Value fn50(Frame *up, Value *a) {    /* 函数 不用保护，第 780 行 */
    Value L[2] = {{0}};
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10;
    L[0] = a[0];    /* 式 */
    char or0 = 0;
    char or1 = 0;
    bh_line = 782;
    t0 = bi_slice(3, (Value[]){L[0], bh_int(0LL), bh_int(2LL)});
    if (!L[1].tag && or1) bh_modify_outer("头"); L[1] = t0;
    bh_line = 783;
    {
        {
            {
                t3 = BH_FALSE;
                {
                    if (L[1].tag) t4 = L[1]; else { or1 = 1; t4 = bh_chk(G[25], "头"); }
                    if (bh_cmp(0, t4, K[176])) {
                        t3 = BH_TRUE;
                    }
                }
                if (bh_truthy(t3)) {
                    t2 = t3;
                } else {
                    t5 = BH_FALSE;
                    {
                        bh_line = 783;
                        if (L[1].tag) t6 = L[1]; else { or1 = 1; t6 = bh_chk(G[25], "头"); }
                        if (bh_cmp(0, t6, K[177])) {
                            t5 = BH_TRUE;
                        }
                    }
                    t2 = t5;
                }
            }
            if (bh_truthy(t2)) {
                t1 = t2;
            } else {
                t7 = BH_FALSE;
                {
                    bh_line = 783;
                    if (L[1].tag) t8 = L[1]; else { or1 = 1; t8 = bh_chk(G[25], "头"); }
                    if (bh_cmp(0, t8, K[178])) {
                        t7 = BH_TRUE;
                    }
                }
                t1 = t7;
            }
        }
        if (bh_truthy(t1)) {
            t0 = t1;
        } else {
            t9 = BH_FALSE;
            {
                bh_line = 783;
                if (L[1].tag) t10 = L[1]; else { or1 = 1; t10 = bh_chk(G[25], "头"); }
                if (bh_cmp(0, t10, K[179])) {
                    t9 = BH_TRUE;
                }
            }
            t0 = t9;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 784;
        return BH_TRUE;
    } else {
    }
    bh_line = 785;
    {
        t1 = BH_FALSE;
        {
            t2 = bi_slice(3, (Value[]){L[0], bh_int(0LL), bh_int(7LL)});
            if (bh_cmp(0, t2, K[180])) {
                t1 = BH_TRUE;
            }
        }
        if (bh_truthy(t1)) {
            t0 = t1;
        } else {
            t3 = BH_FALSE;
            {
                bh_line = 785;
                t4 = bi_slice(3, (Value[]){L[0], bh_int(0LL), bh_int(9LL)});
                if (bh_cmp(0, t4, K[181])) {
                    t3 = BH_TRUE;
                }
            }
            t0 = t3;
        }
    }
    return t0;
    return BH_NIL;
}

static Value fn51(Frame *up, Value *a) {    /* 函数 保根，第 788 行 */
    struct { BhRoots h; Value v[2]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 2);
    L = fr.v; T = fr.v + 2;
    Value t0, t1, t2, t3, t4, t5;
    L[0] = a[0];    /* 式 */
    char or0 = 0;
    char or1 = 0;
    BH_SAFEPOINT();
    bh_line = 790;
    t0 = bh_chk(G[154], "不用保护");
    t1 = bh_call(t0, 1, (Value[]){L[0]}, "不用保护");
    if (bh_truthy(t1)) {
        bh_line = 791;
        BH_RETURN(fr, L[0]);
    } else {
    }
    bh_line = 792;
    t0 = bh_chk(G[153], "根临时");
    t1 = bh_call(t0, 0, NULL, "根临时");
    if (!L[1].tag && or1) bh_modify_outer("格"); L[1] = t1;
    bh_line = 793;
    t0 = bh_chk(G[147], "出");
    if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[151], "格"); }
    t2 = bh_add(t1, K[182]);
    t3 = bh_add(t2, L[0]);
    t4 = bh_add(t3, K[52]);
    t5 = bh_call(t0, 1, (Value[]){t4}, "出");
    bh_line = 794;
    if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[151], "格"); }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn52(Frame *up, Value *a) {    /* 函数 新整型，第 797 行 */
    Value L[1] = {{0}};
    Value t0, t1, t2, t3, t4, t5;
    bh_line = 798;
    t0 = bh_chk(G[126], "环境");
    t1 = bh_chk(G[142], "整型位");
    t2 = bh_index(t0, t1);
    t3 = bh_add(t2, bh_int(1LL));
    t4 = bh_chk(G[126], "环境");
    t5 = bh_chk(G[142], "整型位");
    bh_set_index(t4, t5, t3);
    bh_line = 799;
    t0 = bh_chk(G[126], "环境");
    t1 = bh_chk(G[142], "整型位");
    t2 = bh_index(t0, t1);
    t3 = bh_add(K[89], t2);
    return t3;
    return BH_NIL;
}

static Value fn53(Frame *up, Value *a) {    /* 函数 定行，第 802 行 */
    struct { BhRoots h; Value v[1]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 1);
    L = fr.v; T = fr.v + 1;
    Value t0, t1, t2, t3, t4, t5, t6, t7;
    L[0] = a[0];    /* 行 */
    char or0 = 0;
    BH_SAFEPOINT();
    bh_line = 804;
    t0 = BH_FALSE;
    {
        t1 = bh_chk(G[126], "环境");
        t2 = bh_chk(G[137], "行位");
        t3 = bh_index(t1, t2);
        if (bh_cmp(1, t3, L[0])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 805;
        t4 = bh_chk(G[147], "出");
        t5 = bh_add(K[183], L[0]);
        t6 = bh_add(t5, K[52]);
        t7 = bh_call(t4, 1, (Value[]){t6}, "出");
        bh_line = 806;
        t4 = bh_chk(G[126], "环境");
        t5 = bh_chk(G[137], "行位");
        bh_set_index(t4, t5, L[0]);
    } else {
    }
    BH_RETURN(fr, BH_NIL);
}

static Value fn54(Frame *up, Value *a) {    /* 函数 全局槽，第 809 行 */
    Value L[2] = {{0}};
    Value t0, t1, t2, t3, t4;
    L[0] = a[0];    /* 名 */
    char or0 = 0;
    char or1 = 0;
    bh_line = 810;
    t0 = bh_chk(G[122], "全局名表");
    t1 = bi_find(2, (Value[]){t0, L[0]});
    if (!L[1].tag && or1) bh_modify_outer("号"); L[1] = t1;
    bh_line = 811;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[45], "号"); }
        if (bh_cmp(2, t1, bh_int(0LL))) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 812;
        t2 = bh_chk(G[122], "全局名表");
        t3 = bi_append(2, (Value[]){t2, L[0]});
        bh_line = 813;
        t2 = bh_chk(G[122], "全局名表");
        t3 = bi_len(1, (Value[]){t2});
        t4 = bh_sub(t3, bh_int(1LL));
        if (!L[1].tag && or1) bh_modify_outer("号"); L[1] = t4;
    } else {
    }
    bh_line = 814;
    if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[45], "号"); }
    return t0;
    return BH_NIL;
}

static Value fn55(Frame *up, Value *a) {    /* 函数 常量槽，第 817 行 */
    Value L[2] = {{0}};
    Value t0, t1, t2, t3, t4;
    L[0] = a[0];    /* 文 */
    char or0 = 0;
    char or1 = 0;
    bh_line = 818;
    t0 = bh_chk(G[123], "常量表");
    t1 = bi_find(2, (Value[]){t0, L[0]});
    if (!L[1].tag && or1) bh_modify_outer("号"); L[1] = t1;
    bh_line = 819;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[45], "号"); }
        if (bh_cmp(2, t1, bh_int(0LL))) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 820;
        t2 = bh_chk(G[123], "常量表");
        t3 = bi_append(2, (Value[]){t2, L[0]});
        bh_line = 821;
        t2 = bh_chk(G[123], "常量表");
        t3 = bi_len(1, (Value[]){t2});
        t4 = bh_sub(t3, bh_int(1LL));
        if (!L[1].tag && or1) bh_modify_outer("号"); L[1] = t4;
    } else {
    }
    bh_line = 822;
    if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[45], "号"); }
    return t0;
    return BH_NIL;
}

static Value fn56(Frame *up, Value *a) {    /* 函数 C文字，第 825 行 */
    struct { BhRoots h; Value v[5]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 5);
    L = fr.v; T = fr.v + 4;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17, t18, t19, t20, t21, t22, t23, t24, t25, t26, t27, t28;
    L[0] = a[0];    /* 文 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    BH_SAFEPOINT();
    bh_line = 827;
    t0 = bh_list_make(1, (Value[]){K[57]});
    if (!L[1].tag && or1) bh_modify_outer("片"); L[1] = t0;
    bh_line = 828;
    {
        T[0] = bh_iter(L[0]);
        int64_t n1;
        for (n1 = 0; n1 < bh_list_len(T[0]); n1++) {
            BH_SAFEPOINT();
            bh_line = 828;
            if (!L[2].tag && or2) bh_modify_outer("字"); L[2] = bh_list_at(T[0], n1);
            bh_line = 829;
            if (L[2].tag) t0 = L[2]; else { or2 = 1; t0 = bh_chk(G[44], "字"); }
            t1 = bi_ord(1, (Value[]){t0});
            if (!L[3].tag && or3) bh_modify_outer("码"); L[3] = t1;
            bh_line = 830;
            t0 = BH_FALSE;
            {
                if (L[2].tag) t1 = L[2]; else { or2 = 1; t1 = bh_chk(G[44], "字"); }
                if (bh_cmp(0, t1, K[88])) {
                    t0 = BH_TRUE;
                }
            }
            if (bh_truthy(t0)) {
                bh_line = 831;
                if (L[1].tag) t2 = L[1]; else { or1 = 1; t2 = bh_chk(G[28], "片"); }
                t3 = bi_append(2, (Value[]){t2, K[184]});
            } else {
                t2 = BH_FALSE;
                {
                    bh_line = 832;
                    if (L[2].tag) t3 = L[2]; else { or2 = 1; t3 = bh_chk(G[44], "字"); }
                    if (bh_cmp(0, t3, K[57])) {
                        t2 = BH_TRUE;
                    }
                }
                if (bh_truthy(t2)) {
                    bh_line = 833;
                    if (L[1].tag) t4 = L[1]; else { or1 = 1; t4 = bh_chk(G[28], "片"); }
                    t5 = bi_append(2, (Value[]){t4, K[185]});
                } else {
                    t4 = BH_FALSE;
                    {
                        bh_line = 834;
                        if (L[2].tag) t5 = L[2]; else { or2 = 1; t5 = bh_chk(G[44], "字"); }
                        if (bh_cmp(0, t5, K[186])) {
                            t4 = BH_TRUE;
                        }
                    }
                    if (bh_truthy(t4)) {
                        bh_line = 835;
                        if (L[1].tag) t6 = L[1]; else { or1 = 1; t6 = bh_chk(G[28], "片"); }
                        t7 = bi_append(2, (Value[]){t6, K[187]});
                    } else {
                        t6 = BH_FALSE;
                        {
                            bh_line = 836;
                            if (L[2].tag) t7 = L[2]; else { or2 = 1; t7 = bh_chk(G[44], "字"); }
                            if (bh_cmp(0, t7, K[56])) {
                                t6 = BH_TRUE;
                            }
                        }
                        if (bh_truthy(t6)) {
                            bh_line = 837;
                            if (L[1].tag) t8 = L[1]; else { or1 = 1; t8 = bh_chk(G[28], "片"); }
                            t9 = bi_append(2, (Value[]){t8, K[188]});
                        } else {
                            t8 = BH_FALSE;
                            {
                                bh_line = 838;
                                if (L[2].tag) t9 = L[2]; else { or2 = 1; t9 = bh_chk(G[44], "字"); }
                                if (bh_cmp(0, t9, K[55])) {
                                    t8 = BH_TRUE;
                                }
                            }
                            if (bh_truthy(t8)) {
                                bh_line = 839;
                                if (L[1].tag) t10 = L[1]; else { or1 = 1; t10 = bh_chk(G[28], "片"); }
                                t11 = bi_append(2, (Value[]){t10, K[189]});
                            } else {
                                {
                                    t11 = BH_FALSE;
                                    {
                                        bh_line = 840;
                                        if (L[3].tag) t12 = L[3]; else { or3 = 1; t12 = bh_chk(G[20], "码"); }
                                        if (bh_cmp(2, t12, bh_int(32LL))) {
                                            t11 = BH_TRUE;
                                        }
                                    }
                                    if (bh_truthy(t11)) {
                                        t10 = t11;
                                    } else {
                                        t13 = BH_FALSE;
                                        {
                                            bh_line = 840;
                                            if (L[3].tag) t14 = L[3]; else { or3 = 1; t14 = bh_chk(G[20], "码"); }
                                            if (bh_cmp(0, t14, bh_int(127LL))) {
                                                t13 = BH_TRUE;
                                            }
                                        }
                                        t10 = t13;
                                    }
                                }
                                if (bh_truthy(t10)) {
                                    bh_line = 841;
                                    if (L[1].tag) t15 = L[1]; else { or1 = 1; t15 = bh_chk(G[28], "片"); }
                                    if (L[3].tag) t16 = L[3]; else { or3 = 1; t16 = bh_chk(G[20], "码"); }
                                    t17 = bh_div(t16, bh_int(64LL));
                                    t18 = bi_int(1, (Value[]){t17});
                                    t19 = bh_add(K[88], t18);
                                    if (L[3].tag) t20 = L[3]; else { or3 = 1; t20 = bh_chk(G[20], "码"); }
                                    t21 = bh_div(t20, bh_int(8LL));
                                    t22 = bi_int(1, (Value[]){t21});
                                    t23 = bh_mod(t22, bh_int(8LL));
                                    t24 = bh_add(t19, t23);
                                    if (L[3].tag) t25 = L[3]; else { or3 = 1; t25 = bh_chk(G[20], "码"); }
                                    t26 = bh_mod(t25, bh_int(8LL));
                                    t27 = bh_add(t24, t26);
                                    t28 = bi_append(2, (Value[]){t15, t27});
                                } else {
                                    bh_line = 843;
                                    if (L[1].tag) t15 = L[1]; else { or1 = 1; t15 = bh_chk(G[28], "片"); }
                                    if (L[2].tag) t16 = L[2]; else { or2 = 1; t16 = bh_chk(G[44], "字"); }
                                    t17 = bi_append(2, (Value[]){t15, t16});
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    bh_line = 844;
    if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[28], "片"); }
    t1 = bi_append(2, (Value[]){t0, K[57]});
    bh_line = 845;
    if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[28], "片"); }
    t1 = bi_join(1, (Value[]){t0});
    BH_RETURN(fr, t1);
    BH_RETURN(fr, BH_NIL);
}

static Value fn57(Frame *up, Value *a) {    /* 函数 数字常量，第 848 行 */
    struct { BhRoots h; Value v[1]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 1);
    L = fr.v; T = fr.v + 1;
    Value t0, t1, t2, t3, t4, t5, t6;
    L[0] = a[0];    /* 文本 */
    char or0 = 0;
    BH_SAFEPOINT();
    bh_line = 849;
    t0 = BH_FALSE;
    {
        t1 = bi_find(2, (Value[]){L[0], K[95]});
        if (bh_cmp(5, t1, bh_int(0LL))) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 850;
        t2 = bh_add(K[181], L[0]);
        t3 = bh_add(t2, K[102]);
        BH_RETURN(fr, t3);
    } else {
    }
    bh_line = 851;
    t0 = BH_FALSE;
    {
        t1 = bi_len(1, (Value[]){L[0]});
        if (bh_cmp(3, t1, bh_int(18LL))) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 852;
        t2 = bh_chk(G[159], "常量槽");
        t3 = bh_add(K[190], L[0]);
        t4 = bh_call(t2, 1, (Value[]){t3}, "常量槽");
        t5 = bh_add(K[178], t4);
        t6 = bh_add(t5, K[50]);
        BH_RETURN(fr, t6);
    } else {
    }
    bh_line = 853;
    t0 = bh_add(K[180], L[0]);
    t1 = bh_add(t0, K[191]);
    BH_RETURN(fr, t1);
    BH_RETURN(fr, BH_NIL);
}

static Value fn58(Frame *up, Value *a) {    /* 函数 至少一，第 856 行 */
    Value L[1] = {{0}};
    Value t0;
    L[0] = a[0];    /* 数 */
    char or0 = 0;
    bh_line = 857;
    t0 = BH_FALSE;
    {
        if (bh_cmp(2, L[0], bh_int(1LL))) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 858;
        return bh_int(1LL);
    } else {
    }
    bh_line = 859;
    return L[0];
    return BH_NIL;
}

static Value fn59(Frame *up, Value *a) {    /* 函数 记下，第 862 行 */
    Value L[2] = {{0}};
    Value t0, t1, t2;
    L[0] = a[0];    /* 表 */
    char or0 = 0;
    L[1] = a[1];    /* 名 */
    char or1 = 0;
    bh_line = 863;
    t0 = BH_FALSE;
    {
        t1 = bi_find(2, (Value[]){L[0], L[1]});
        if (bh_cmp(2, t1, bh_int(0LL))) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 864;
        t2 = bi_append(2, (Value[]){L[0], L[1]});
    } else {
    }
    return BH_NIL;
}

static Value fn60(Frame *up, Value *a) {    /* 函数 收集，第 867 行 */
    struct { BhRoots h; Value v[10]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 10);
    L = fr.v; T = fr.v + 8;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17, t18, t19;
    L[0] = a[0];    /* 语句表 */
    char or0 = 0;
    L[1] = a[1];    /* 局部 */
    char or1 = 0;
    L[2] = a[2];    /* 声明 */
    char or2 = 0;
    L[3] = a[3];    /* 标记 */
    char or3 = 0;
    char or4 = 0;
    char or5 = 0;
    char or6 = 0;
    char or7 = 0;
    BH_SAFEPOINT();
    bh_line = 870;
    {
        T[0] = bh_iter(L[0]);
        int64_t n1;
        for (n1 = 0; n1 < bh_list_len(T[0]); n1++) {
            BH_SAFEPOINT();
            bh_line = 870;
            if (!L[4].tag && or4) bh_modify_outer("句"); L[4] = bh_list_at(T[0], n1);
            bh_line = 871;
            if (L[4].tag) t0 = L[4]; else { or4 = 1; t0 = bh_chk(G[164], "句"); }
            t1 = bh_index(t0, bh_int(0LL));
            if (!L[5].tag && or5) bh_modify_outer("种类"); L[5] = t1;
            bh_line = 872;
            t0 = BH_FALSE;
            {
                if (L[5].tag) t1 = L[5]; else { or5 = 1; t1 = bh_chk(G[65], "种类"); }
                if (bh_cmp(0, t1, K[143])) {
                    t0 = BH_TRUE;
                }
            }
            if (bh_truthy(t0)) {
                bh_line = 873;
                t2 = BH_FALSE;
                {
                    if (L[4].tag) t3 = L[4]; else { or4 = 1; t3 = bh_chk(G[164], "句"); }
                    t4 = bh_index(t3, bh_int(2LL));
                    t5 = bh_index(t4, bh_int(0LL));
                    if (bh_cmp(0, t5, K[128])) {
                        t2 = BH_TRUE;
                    }
                }
                if (bh_truthy(t2)) {
                    bh_line = 874;
                    t6 = bh_chk(G[163], "记下");
                    if (L[4].tag) t7 = L[4]; else { or4 = 1; t7 = bh_chk(G[164], "句"); }
                    t8 = bh_index(t7, bh_int(2LL));
                    t9 = bh_index(t8, bh_int(2LL));
                    t10 = bh_call(t6, 2, (Value[]){L[1], t9}, "记下");
                } else {
                }
            } else {
                t2 = BH_FALSE;
                {
                    bh_line = 875;
                    if (L[5].tag) t3 = L[5]; else { or5 = 1; t3 = bh_chk(G[65], "种类"); }
                    if (bh_cmp(0, t3, K[146])) {
                        t2 = BH_TRUE;
                    }
                }
                if (bh_truthy(t2)) {
                    bh_line = 876;
                    {
                        if (L[4].tag) t4 = L[4]; else { or4 = 1; t4 = bh_chk(G[164], "句"); }
                        t5 = bh_index(t4, bh_int(2LL));
                        T[1] = bh_iter(t5);
                        int64_t n2;
                        for (n2 = 0; n2 < bh_list_len(T[1]); n2++) {
                            BH_SAFEPOINT();
                            bh_line = 876;
                            if (!L[6].tag && or6) bh_modify_outer("分支"); L[6] = bh_list_at(T[1], n2);
                            bh_line = 877;
                            t6 = bh_chk(G[165], "收集");
                            if (L[6].tag) t7 = L[6]; else { or6 = 1; t7 = bh_chk(G[166], "分支"); }
                            t8 = bh_index(t7, bh_int(1LL));
                            t9 = bh_call(t6, 4, (Value[]){t8, L[1], L[2], L[3]}, "收集");
                        }
                    }
                    bh_line = 878;
                    t4 = BH_FALSE;
                    {
                        if (L[4].tag) t5 = L[4]; else { or4 = 1; t5 = bh_chk(G[164], "句"); }
                        t6 = bh_index(t5, bh_int(3LL));
                        if (bh_cmp(1, t6, BH_NIL)) {
                            t4 = BH_TRUE;
                        }
                    }
                    if (bh_truthy(t4)) {
                        bh_line = 879;
                        t7 = bh_chk(G[165], "收集");
                        if (L[4].tag) t8 = L[4]; else { or4 = 1; t8 = bh_chk(G[164], "句"); }
                        t9 = bh_index(t8, bh_int(3LL));
                        t10 = bh_call(t7, 4, (Value[]){t9, L[1], L[2], L[3]}, "收集");
                    } else {
                    }
                } else {
                    {
                        t5 = BH_FALSE;
                        {
                            bh_line = 880;
                            if (L[5].tag) t6 = L[5]; else { or5 = 1; t6 = bh_chk(G[65], "种类"); }
                            if (bh_cmp(0, t6, K[147])) {
                                t5 = BH_TRUE;
                            }
                        }
                        if (bh_truthy(t5)) {
                            t4 = t5;
                        } else {
                            t7 = BH_FALSE;
                            {
                                bh_line = 880;
                                if (L[5].tag) t8 = L[5]; else { or5 = 1; t8 = bh_chk(G[65], "种类"); }
                                if (bh_cmp(0, t8, K[151])) {
                                    t7 = BH_TRUE;
                                }
                            }
                            t4 = t7;
                        }
                    }
                    if (bh_truthy(t4)) {
                        bh_line = 881;
                        t9 = bh_chk(G[165], "收集");
                        if (L[4].tag) t10 = L[4]; else { or4 = 1; t10 = bh_chk(G[164], "句"); }
                        t11 = bh_index(t10, bh_int(3LL));
                        t12 = bh_call(t9, 4, (Value[]){t11, L[1], L[2], L[3]}, "收集");
                    } else {
                        t9 = BH_FALSE;
                        {
                            bh_line = 882;
                            if (L[5].tag) t10 = L[5]; else { or5 = 1; t10 = bh_chk(G[65], "种类"); }
                            if (bh_cmp(0, t10, K[149])) {
                                t9 = BH_TRUE;
                            }
                        }
                        if (bh_truthy(t9)) {
                            bh_line = 883;
                            t11 = bh_chk(G[163], "记下");
                            if (L[4].tag) t12 = L[4]; else { or4 = 1; t12 = bh_chk(G[164], "句"); }
                            t13 = bh_index(t12, bh_int(3LL));
                            t14 = bh_call(t11, 2, (Value[]){L[1], t13}, "记下");
                            bh_line = 884;
                            t11 = bh_chk(G[165], "收集");
                            if (L[4].tag) t12 = L[4]; else { or4 = 1; t12 = bh_chk(G[164], "句"); }
                            t13 = bh_index(t12, bh_int(4LL));
                            t14 = bh_call(t11, 4, (Value[]){t13, L[1], L[2], L[3]}, "收集");
                        } else {
                            t11 = BH_FALSE;
                            {
                                bh_line = 885;
                                if (L[5].tag) t12 = L[5]; else { or5 = 1; t12 = bh_chk(G[65], "种类"); }
                                if (bh_cmp(0, t12, K[156])) {
                                    t11 = BH_TRUE;
                                }
                            }
                            if (bh_truthy(t11)) {
                                bh_line = 886;
                                t13 = bh_chk(G[163], "记下");
                                if (L[4].tag) t14 = L[4]; else { or4 = 1; t14 = bh_chk(G[164], "句"); }
                                t15 = bh_index(t14, bh_int(2LL));
                                t16 = bh_call(t13, 2, (Value[]){L[1], t15}, "记下");
                                bh_line = 887;
                                bh_set_index(L[3], bh_int(0LL), BH_TRUE);
                            } else {
                                t13 = BH_FALSE;
                                {
                                    bh_line = 888;
                                    if (L[5].tag) t14 = L[5]; else { or5 = 1; t14 = bh_chk(G[65], "种类"); }
                                    if (bh_cmp(0, t14, K[140])) {
                                        t13 = BH_TRUE;
                                    }
                                }
                                if (bh_truthy(t13)) {
                                    bh_line = 889;
                                    {
                                        if (L[4].tag) t15 = L[4]; else { or4 = 1; t15 = bh_chk(G[164], "句"); }
                                        t16 = bh_index(t15, bh_int(2LL));
                                        T[1] = bh_iter(t16);
                                        int64_t n3;
                                        for (n3 = 0; n3 < bh_list_len(T[1]); n3++) {
                                            BH_SAFEPOINT();
                                            bh_line = 889;
                                            if (!L[7].tag && or7) bh_modify_outer("名"); L[7] = bh_list_at(T[1], n3);
                                            bh_line = 890;
                                            t17 = bh_chk(G[163], "记下");
                                            if (L[7].tag) t18 = L[7]; else { or7 = 1; t18 = bh_chk(G[89], "名"); }
                                            t19 = bh_call(t17, 2, (Value[]){L[2], t18}, "记下");
                                        }
                                    }
                                } else {
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    BH_RETURN(fr, BH_NIL);
}

static Value fn61(Frame *up, Value *a) {    /* 函数 读内置表，第 893 行 */
    struct { BhRoots h; Value v[11]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 11);
    L = fr.v; T = fr.v + 9;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17, t18;
    L[0] = a[0];    /* 运行时 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    char or5 = 0;
    char or6 = 0;
    char or7 = 0;
    char or8 = 0;
    char gf0 = 0;
    char gf1 = 0;
    BH_SAFEPOINT();
    bh_line = 895;
    gf0 = 1;
    gf1 = 1;
    bh_line = 896;
    t0 = bh_list_make(0, NULL);
    if (gf0) G[127] = t0; else { if (!L[1].tag && or1) bh_modify_outer("内置名表"); L[1] = t0; }
    bh_line = 897;
    t0 = bh_list_make(0, NULL);
    if (gf1) G[128] = t0; else { if (!L[2].tag && or2) bh_modify_outer("内置表"); L[2] = t0; }
    bh_line = 898;
    t0 = bi_find(2, (Value[]){L[0], K[192]});
    if (!L[3].tag && or3) bh_modify_outer("起"); L[3] = t0;
    bh_line = 899;
    t0 = bi_find(2, (Value[]){L[0], K[193]});
    if (!L[4].tag && or4) bh_modify_outer("止"); L[4] = t0;
    bh_line = 900;
    {
        t1 = BH_FALSE;
        {
            if (L[3].tag) t2 = L[3]; else { or3 = 1; t2 = bh_chk(G[167], "起"); }
            if (bh_cmp(2, t2, bh_int(0LL))) {
                t1 = BH_TRUE;
            }
        }
        if (bh_truthy(t1)) {
            t0 = t1;
        } else {
            t3 = BH_FALSE;
            {
                bh_line = 900;
                if (L[4].tag) t4 = L[4]; else { or4 = 1; t4 = bh_chk(G[168], "止"); }
                if (L[3].tag) t5 = L[3]; else { or3 = 1; t5 = bh_chk(G[167], "起"); }
                if (bh_cmp(2, t4, t5)) {
                    t3 = BH_TRUE;
                }
            }
            t0 = t3;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 901;
        BH_RETURN(fr, BH_NIL);
    } else {
    }
    bh_line = 902;
    if (L[3].tag) t0 = L[3]; else { or3 = 1; t0 = bh_chk(G[167], "起"); }
    if (L[4].tag) t1 = L[4]; else { or4 = 1; t1 = bh_chk(G[168], "止"); }
    if (L[3].tag) t2 = L[3]; else { or3 = 1; t2 = bh_chk(G[167], "起"); }
    t3 = bh_sub(t1, t2);
    t4 = bi_slice(3, (Value[]){L[0], t0, t3});
    t5 = bi_split(2, (Value[]){t4, K[194]});
    if (!L[5].tag && or5) bh_modify_outer("片们"); L[5] = t5;
    bh_line = 903;
    if (!L[6].tag && or6) bh_modify_outer("下标"); L[6] = bh_int(1LL);
    bh_line = 904;
    for (;;) {
        BH_SAFEPOINT();
        t0 = BH_FALSE;
        {
            bh_line = 904;
            if (L[6].tag) t1 = L[6]; else { or6 = 1; t1 = bh_chk(G[19], "下标"); }
            if (L[5].tag) t2 = L[5]; else { or5 = 1; t2 = bh_chk(G[169], "片们"); }
            t3 = bi_len(1, (Value[]){t2});
            if (bh_cmp(2, t1, t3)) {
                t0 = BH_TRUE;
            }
        }
        if (!bh_truthy(t0)) break;
        bh_line = 905;
        if (L[5].tag) t4 = L[5]; else { or5 = 1; t4 = bh_chk(G[169], "片们"); }
        if (L[6].tag) t5 = L[6]; else { or6 = 1; t5 = bh_chk(G[19], "下标"); }
        t6 = bh_index(t4, t5);
        t7 = bi_split(2, (Value[]){t6, K[57]});
        if (!L[7].tag && or7) bh_modify_outer("两半"); L[7] = t7;
        bh_line = 906;
        if (L[7].tag) t4 = L[7]; else { or7 = 1; t4 = bh_chk(G[170], "两半"); }
        t5 = bh_index(t4, bh_int(1LL));
        t6 = bi_split(2, (Value[]){t5, K[173]});
        t7 = bh_index(t6, bh_int(0LL));
        t8 = bi_split(2, (Value[]){t7, K[51]});
        if (!L[8].tag && or8) bh_modify_outer("项们"); L[8] = t8;
        bh_line = 907;
        if (gf0) t4 = bh_chk(G[127], "内置名表"); else if (L[1].tag) t4 = L[1]; else { or1 = 1; t4 = bh_chk(G[127], "内置名表"); }
        if (L[7].tag) t5 = L[7]; else { or7 = 1; t5 = bh_chk(G[170], "两半"); }
        t6 = bh_index(t5, bh_int(0LL));
        t7 = bi_append(2, (Value[]){t4, t6});
        bh_line = 908;
        if (gf1) t4 = bh_chk(G[128], "内置表"); else if (L[2].tag) t4 = L[2]; else { or2 = 1; t4 = bh_chk(G[128], "内置表"); }
        T[0] = t4;
        if (L[7].tag) t5 = L[7]; else { or7 = 1; t5 = bh_chk(G[170], "两半"); }
        t6 = bh_index(t5, bh_int(0LL));
        T[1] = t6;
        t7 = bh_chk(G[27], "去空白");
        if (L[8].tag) t8 = L[8]; else { or8 = 1; t8 = bh_chk(G[119], "项们"); }
        t9 = bh_index(t8, bh_int(1LL));
        t10 = bh_call(t7, 1, (Value[]){t9}, "去空白");
        if (L[8].tag) t11 = L[8]; else { or8 = 1; t11 = bh_chk(G[119], "项们"); }
        t12 = bh_index(t11, bh_int(2LL));
        t13 = bi_int(1, (Value[]){t12});
        if (L[8].tag) t14 = L[8]; else { or8 = 1; t14 = bh_chk(G[119], "项们"); }
        t15 = bh_index(t14, bh_int(3LL));
        t16 = bi_int(1, (Value[]){t15});
        t17 = bh_list_make(4, (Value[]){T[1], t10, t13, t16});
        t18 = bi_append(2, (Value[]){T[0], t17});
        bh_line = 909;
        if (L[6].tag) t4 = L[6]; else { or6 = 1; t4 = bh_chk(G[19], "下标"); }
        t5 = bh_add(t4, bh_int(1LL));
        if (!L[6].tag && or6) bh_modify_outer("下标"); L[6] = t5;
    }
    BH_RETURN(fr, BH_NIL);
}

static Value fn62(Frame *up, Value *a) {    /* 函数 找函数，第 912 行 */
    struct { BhRoots h; Value v[7]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 7);
    L = fr.v; T = fr.v + 5;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14;
    L[0] = a[0];    /* 语句表 */
    char or0 = 0;
    L[1] = a[1];    /* 结果 */
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    BH_SAFEPOINT();
    bh_line = 914;
    {
        T[0] = bh_iter(L[0]);
        int64_t n1;
        for (n1 = 0; n1 < bh_list_len(T[0]); n1++) {
            BH_SAFEPOINT();
            bh_line = 914;
            if (!L[2].tag && or2) bh_modify_outer("句"); L[2] = bh_list_at(T[0], n1);
            bh_line = 915;
            if (L[2].tag) t0 = L[2]; else { or2 = 1; t0 = bh_chk(G[164], "句"); }
            t1 = bh_index(t0, bh_int(0LL));
            if (!L[3].tag && or3) bh_modify_outer("种类"); L[3] = t1;
            bh_line = 916;
            t0 = BH_FALSE;
            {
                if (L[3].tag) t1 = L[3]; else { or3 = 1; t1 = bh_chk(G[65], "种类"); }
                if (bh_cmp(0, t1, K[156])) {
                    t0 = BH_TRUE;
                }
            }
            if (bh_truthy(t0)) {
                bh_line = 917;
                if (L[2].tag) t2 = L[2]; else { or2 = 1; t2 = bh_chk(G[164], "句"); }
                t3 = bi_append(2, (Value[]){L[1], t2});
            } else {
                t2 = BH_FALSE;
                {
                    bh_line = 918;
                    if (L[3].tag) t3 = L[3]; else { or3 = 1; t3 = bh_chk(G[65], "种类"); }
                    if (bh_cmp(0, t3, K[146])) {
                        t2 = BH_TRUE;
                    }
                }
                if (bh_truthy(t2)) {
                    bh_line = 919;
                    {
                        if (L[2].tag) t4 = L[2]; else { or2 = 1; t4 = bh_chk(G[164], "句"); }
                        t5 = bh_index(t4, bh_int(2LL));
                        T[1] = bh_iter(t5);
                        int64_t n2;
                        for (n2 = 0; n2 < bh_list_len(T[1]); n2++) {
                            BH_SAFEPOINT();
                            bh_line = 919;
                            if (!L[4].tag && or4) bh_modify_outer("分支"); L[4] = bh_list_at(T[1], n2);
                            bh_line = 920;
                            t6 = bh_chk(G[172], "找函数");
                            if (L[4].tag) t7 = L[4]; else { or4 = 1; t7 = bh_chk(G[166], "分支"); }
                            t8 = bh_index(t7, bh_int(1LL));
                            t9 = bh_call(t6, 2, (Value[]){t8, L[1]}, "找函数");
                        }
                    }
                    bh_line = 921;
                    t4 = BH_FALSE;
                    {
                        if (L[2].tag) t5 = L[2]; else { or2 = 1; t5 = bh_chk(G[164], "句"); }
                        t6 = bh_index(t5, bh_int(3LL));
                        if (bh_cmp(1, t6, BH_NIL)) {
                            t4 = BH_TRUE;
                        }
                    }
                    if (bh_truthy(t4)) {
                        bh_line = 922;
                        t7 = bh_chk(G[172], "找函数");
                        if (L[2].tag) t8 = L[2]; else { or2 = 1; t8 = bh_chk(G[164], "句"); }
                        t9 = bh_index(t8, bh_int(3LL));
                        t10 = bh_call(t7, 2, (Value[]){t9, L[1]}, "找函数");
                    } else {
                    }
                } else {
                    {
                        t5 = BH_FALSE;
                        {
                            bh_line = 923;
                            if (L[3].tag) t6 = L[3]; else { or3 = 1; t6 = bh_chk(G[65], "种类"); }
                            if (bh_cmp(0, t6, K[147])) {
                                t5 = BH_TRUE;
                            }
                        }
                        if (bh_truthy(t5)) {
                            t4 = t5;
                        } else {
                            t7 = BH_FALSE;
                            {
                                bh_line = 923;
                                if (L[3].tag) t8 = L[3]; else { or3 = 1; t8 = bh_chk(G[65], "种类"); }
                                if (bh_cmp(0, t8, K[151])) {
                                    t7 = BH_TRUE;
                                }
                            }
                            t4 = t7;
                        }
                    }
                    if (bh_truthy(t4)) {
                        bh_line = 924;
                        t9 = bh_chk(G[172], "找函数");
                        if (L[2].tag) t10 = L[2]; else { or2 = 1; t10 = bh_chk(G[164], "句"); }
                        t11 = bh_index(t10, bh_int(3LL));
                        t12 = bh_call(t9, 2, (Value[]){t11, L[1]}, "找函数");
                    } else {
                        t9 = BH_FALSE;
                        {
                            bh_line = 925;
                            if (L[3].tag) t10 = L[3]; else { or3 = 1; t10 = bh_chk(G[65], "种类"); }
                            if (bh_cmp(0, t10, K[149])) {
                                t9 = BH_TRUE;
                            }
                        }
                        if (bh_truthy(t9)) {
                            bh_line = 926;
                            t11 = bh_chk(G[172], "找函数");
                            if (L[2].tag) t12 = L[2]; else { or2 = 1; t12 = bh_chk(G[164], "句"); }
                            t13 = bh_index(t12, bh_int(4LL));
                            t14 = bh_call(t11, 2, (Value[]){t13, L[1]}, "找函数");
                        } else {
                        }
                    }
                }
            }
        }
    }
    BH_RETURN(fr, BH_NIL);
}

static Value fn63(Frame *up, Value *a) {    /* 函数 找全局赋值，第 929 行 */
    struct { BhRoots h; Value v[9]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 9);
    L = fr.v; T = fr.v + 8;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9;
    L[0] = a[0];    /* 语句表 */
    char or0 = 0;
    L[1] = a[1];    /* 是模块 */
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    char or5 = 0;
    char or6 = 0;
    char or7 = 0;
    BH_SAFEPOINT();
    bh_line = 931;
    t0 = bh_list_make(0, NULL);
    if (!L[2].tag && or2) bh_modify_outer("局部"); L[2] = t0;
    bh_line = 932;
    t0 = bh_list_make(0, NULL);
    if (!L[3].tag && or3) bh_modify_outer("声明"); L[3] = t0;
    bh_line = 933;
    t0 = bh_list_make(1, (Value[]){BH_FALSE});
    if (!L[4].tag && or4) bh_modify_outer("标记"); L[4] = t0;
    bh_line = 934;
    t0 = bh_chk(G[165], "收集");
    if (L[2].tag) t1 = L[2]; else { or2 = 1; t1 = bh_chk(G[173], "局部"); }
    if (L[3].tag) t2 = L[3]; else { or3 = 1; t2 = bh_chk(G[174], "声明"); }
    if (L[4].tag) t3 = L[4]; else { or4 = 1; t3 = bh_chk(G[175], "标记"); }
    t4 = bh_call(t0, 4, (Value[]){L[0], t1, t2, t3}, "收集");
    bh_line = 935;
    {
        if (L[2].tag) t0 = L[2]; else { or2 = 1; t0 = bh_chk(G[173], "局部"); }
        T[0] = bh_iter(t0);
        int64_t n1;
        for (n1 = 0; n1 < bh_list_len(T[0]); n1++) {
            BH_SAFEPOINT();
            bh_line = 935;
            if (!L[5].tag && or5) bh_modify_outer("名"); L[5] = bh_list_at(T[0], n1);
            bh_line = 936;
            {
                if (bh_truthy(L[1])) {
                    t1 = L[1];
                } else {
                    t2 = BH_FALSE;
                    {
                        bh_line = 936;
                        if (L[3].tag) t3 = L[3]; else { or3 = 1; t3 = bh_chk(G[174], "声明"); }
                        if (L[5].tag) t4 = L[5]; else { or5 = 1; t4 = bh_chk(G[89], "名"); }
                        t5 = bi_find(2, (Value[]){t3, t4});
                        if (bh_cmp(5, t5, bh_int(0LL))) {
                            t2 = BH_TRUE;
                        }
                    }
                    t1 = t2;
                }
            }
            if (bh_truthy(t1)) {
                bh_line = 937;
                t6 = bh_chk(G[163], "记下");
                t7 = bh_chk(G[129], "全局被赋值");
                if (L[5].tag) t8 = L[5]; else { or5 = 1; t8 = bh_chk(G[89], "名"); }
                t9 = bh_call(t6, 2, (Value[]){t7, t8}, "记下");
            } else {
            }
        }
    }
    bh_line = 938;
    t0 = bh_list_make(0, NULL);
    if (!L[6].tag && or6) bh_modify_outer("里面的"); L[6] = t0;
    bh_line = 939;
    t0 = bh_chk(G[172], "找函数");
    if (L[6].tag) t1 = L[6]; else { or6 = 1; t1 = bh_chk(G[176], "里面的"); }
    t2 = bh_call(t0, 2, (Value[]){L[0], t1}, "找函数");
    bh_line = 940;
    {
        if (L[6].tag) t0 = L[6]; else { or6 = 1; t0 = bh_chk(G[176], "里面的"); }
        T[0] = bh_iter(t0);
        int64_t n2;
        for (n2 = 0; n2 < bh_list_len(T[0]); n2++) {
            BH_SAFEPOINT();
            bh_line = 940;
            if (!L[7].tag && or7) bh_modify_outer("句"); L[7] = bh_list_at(T[0], n2);
            bh_line = 941;
            t1 = bh_chk(G[177], "找全局赋值");
            if (L[7].tag) t2 = L[7]; else { or7 = 1; t2 = bh_chk(G[164], "句"); }
            t3 = bh_index(t2, bh_int(4LL));
            t4 = bh_call(t1, 2, (Value[]){t3, BH_FALSE}, "找全局赋值");
        }
    }
    BH_RETURN(fr, BH_NIL);
}

static Value fn64(Frame *up, Value *a) {    /* 函数 直接内置，第 944 行 */
    struct { BhRoots h; Value v[7]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 7);
    L = fr.v; T = fr.v + 7;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12;
    L[0] = a[0];    /* 节点 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    char or5 = 0;
    char or6 = 0;
    BH_SAFEPOINT();
    bh_line = 946;
    t0 = bh_index(L[0], bh_int(2LL));
    if (!L[1].tag && or1) bh_modify_outer("被调"); L[1] = t0;
    bh_line = 947;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[178], "被调"); }
        t2 = bh_index(t1, bh_int(0LL));
        if (bh_cmp(1, t2, K[128])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 948;
        BH_RETURN(fr, BH_NIL);
    } else {
    }
    bh_line = 949;
    if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[178], "被调"); }
    t1 = bh_index(t0, bh_int(2LL));
    if (!L[2].tag && or2) bh_modify_outer("名"); L[2] = t1;
    bh_line = 950;
    t0 = bh_chk(G[127], "内置名表");
    if (L[2].tag) t1 = L[2]; else { or2 = 1; t1 = bh_chk(G[89], "名"); }
    t2 = bi_find(2, (Value[]){t0, t1});
    if (!L[3].tag && or3) bh_modify_outer("号"); L[3] = t2;
    bh_line = 951;
    {
        t1 = BH_FALSE;
        {
            if (L[3].tag) t2 = L[3]; else { or3 = 1; t2 = bh_chk(G[45], "号"); }
            if (bh_cmp(2, t2, bh_int(0LL))) {
                t1 = BH_TRUE;
            }
        }
        if (bh_truthy(t1)) {
            t0 = t1;
        } else {
            t3 = BH_FALSE;
            {
                bh_line = 951;
                t4 = bh_chk(G[129], "全局被赋值");
                if (L[2].tag) t5 = L[2]; else { or2 = 1; t5 = bh_chk(G[89], "名"); }
                t6 = bi_find(2, (Value[]){t4, t5});
                if (bh_cmp(5, t6, bh_int(0LL))) {
                    t3 = BH_TRUE;
                }
            }
            t0 = t3;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 952;
        BH_RETURN(fr, BH_NIL);
    } else {
    }
    bh_line = 953;
    t0 = bh_chk(G[126], "环境");
    if (!L[4].tag && or4) bh_modify_outer("外"); L[4] = t0;
    bh_line = 954;
    for (;;) {
        BH_SAFEPOINT();
        {
            t1 = BH_FALSE;
            {
                bh_line = 954;
                if (L[4].tag) t2 = L[4]; else { or4 = 1; t2 = bh_chk(G[179], "外"); }
                if (bh_cmp(1, t2, BH_NIL)) {
                    t1 = BH_TRUE;
                }
            }
            if (bh_truthy(t1)) {
                bh_line = 954;
                if (L[4].tag) t3 = L[4]; else { or4 = 1; t3 = bh_chk(G[179], "外"); }
                t4 = bh_chk(G[139], "模块位");
                t5 = bh_index(t3, t4);
                t6 = bh_bool(!bh_truthy(t5));
                t0 = t6;
            } else {
                t0 = t1;
            }
        }
        if (!bh_truthy(t0)) break;
        bh_line = 955;
        t7 = BH_FALSE;
        {
            if (L[4].tag) t8 = L[4]; else { or4 = 1; t8 = bh_chk(G[179], "外"); }
            t9 = bh_chk(G[132], "局部位");
            t10 = bh_index(t8, t9);
            if (L[2].tag) t11 = L[2]; else { or2 = 1; t11 = bh_chk(G[89], "名"); }
            t12 = bi_find(2, (Value[]){t10, t11});
            if (bh_cmp(5, t12, bh_int(0LL))) {
                t7 = BH_TRUE;
            }
        }
        if (bh_truthy(t7)) {
            bh_line = 956;
            BH_RETURN(fr, BH_NIL);
        } else {
        }
        bh_line = 957;
        if (L[4].tag) t7 = L[4]; else { or4 = 1; t7 = bh_chk(G[179], "外"); }
        t8 = bh_chk(G[138], "外层位");
        t9 = bh_index(t7, t8);
        if (!L[4].tag && or4) bh_modify_outer("外"); L[4] = t9;
    }
    bh_line = 958;
    t0 = bh_chk(G[128], "内置表");
    if (L[3].tag) t1 = L[3]; else { or3 = 1; t1 = bh_chk(G[45], "号"); }
    t2 = bh_index(t0, t1);
    if (!L[5].tag && or5) bh_modify_outer("项"); L[5] = t2;
    bh_line = 959;
    t0 = bh_index(L[0], bh_int(3LL));
    t1 = bi_len(1, (Value[]){t0});
    if (!L[6].tag && or6) bh_modify_outer("个数"); L[6] = t1;
    bh_line = 960;
    {
        t1 = BH_FALSE;
        {
            if (L[6].tag) t2 = L[6]; else { or6 = 1; t2 = bh_chk(G[180], "个数"); }
            if (L[5].tag) t3 = L[5]; else { or5 = 1; t3 = bh_chk(G[75], "项"); }
            t4 = bh_index(t3, bh_int(2LL));
            if (bh_cmp(2, t2, t4)) {
                t1 = BH_TRUE;
            }
        }
        if (bh_truthy(t1)) {
            t0 = t1;
        } else {
            {
                t6 = BH_FALSE;
                {
                    bh_line = 960;
                    if (L[5].tag) t7 = L[5]; else { or5 = 1; t7 = bh_chk(G[75], "项"); }
                    t8 = bh_index(t7, bh_int(3LL));
                    if (bh_cmp(5, t8, bh_int(0LL))) {
                        t6 = BH_TRUE;
                    }
                }
                if (bh_truthy(t6)) {
                    t9 = BH_FALSE;
                    {
                        bh_line = 960;
                        if (L[6].tag) t10 = L[6]; else { or6 = 1; t10 = bh_chk(G[180], "个数"); }
                        if (L[5].tag) t11 = L[5]; else { or5 = 1; t11 = bh_chk(G[75], "项"); }
                        t12 = bh_index(t11, bh_int(3LL));
                        if (bh_cmp(3, t10, t12)) {
                            t9 = BH_TRUE;
                        }
                    }
                    t5 = t9;
                } else {
                    t5 = t6;
                }
            }
            t0 = t5;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 961;
        BH_RETURN(fr, BH_NIL);
    } else {
    }
    bh_line = 962;
    if (L[5].tag) t0 = L[5]; else { or5 = 1; t0 = bh_chk(G[75], "项"); }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn65(Frame *up, Value *a) {    /* 函数 可能回收，第 965 行 */
    struct { BhRoots h; Value v[4]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 4);
    L = fr.v; T = fr.v + 3;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11;
    L[0] = a[0];    /* 节点 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    BH_SAFEPOINT();
    bh_line = 967;
    t0 = bh_index(L[0], bh_int(0LL));
    if (!L[1].tag && or1) bh_modify_outer("种类"); L[1] = t0;
    bh_line = 968;
    {
        {
            {
                t3 = BH_FALSE;
                {
                    if (L[1].tag) t4 = L[1]; else { or1 = 1; t4 = bh_chk(G[65], "种类"); }
                    if (bh_cmp(0, t4, K[165])) {
                        t3 = BH_TRUE;
                    }
                }
                if (bh_truthy(t3)) {
                    t2 = t3;
                } else {
                    t5 = BH_FALSE;
                    {
                        bh_line = 968;
                        if (L[1].tag) t6 = L[1]; else { or1 = 1; t6 = bh_chk(G[65], "种类"); }
                        if (bh_cmp(0, t6, K[166])) {
                            t5 = BH_TRUE;
                        }
                    }
                    t2 = t5;
                }
            }
            if (bh_truthy(t2)) {
                t1 = t2;
            } else {
                t7 = BH_FALSE;
                {
                    bh_line = 968;
                    if (L[1].tag) t8 = L[1]; else { or1 = 1; t8 = bh_chk(G[65], "种类"); }
                    if (bh_cmp(0, t8, K[167])) {
                        t7 = BH_TRUE;
                    }
                }
                t1 = t7;
            }
        }
        if (bh_truthy(t1)) {
            t0 = t1;
        } else {
            t9 = BH_FALSE;
            {
                bh_line = 968;
                if (L[1].tag) t10 = L[1]; else { or1 = 1; t10 = bh_chk(G[65], "种类"); }
                if (bh_cmp(0, t10, K[128])) {
                    t9 = BH_TRUE;
                }
            }
            t0 = t9;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 969;
        BH_RETURN(fr, BH_FALSE);
    } else {
    }
    bh_line = 970;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[169])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 971;
        t2 = bh_chk(G[182], "一串可能回收");
        t3 = bh_index(L[0], bh_int(2LL));
        t4 = bh_call(t2, 1, (Value[]){t3}, "一串可能回收");
        BH_RETURN(fr, t4);
    } else {
    }
    bh_line = 972;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[168])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 973;
        t2 = bh_chk(G[182], "一串可能回收");
        T[0] = t2;
        t3 = bh_chk(G[183], "摊平");
        t4 = bh_index(L[0], bh_int(2LL));
        t5 = bh_call(t3, 1, (Value[]){t4}, "摊平");
        t6 = bh_call(T[0], 1, (Value[]){t5}, "一串可能回收");
        BH_RETURN(fr, t6);
    } else {
    }
    bh_line = 974;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[161])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 975;
        {
            t3 = bh_chk(G[184], "可能回收");
            t4 = bh_index(L[0], bh_int(3LL));
            t5 = bh_call(t3, 1, (Value[]){t4}, "可能回收");
            if (bh_truthy(t5)) {
                t2 = t5;
            } else {
                bh_line = 975;
                t6 = bh_chk(G[184], "可能回收");
                t7 = bh_index(L[0], bh_int(4LL));
                t8 = bh_call(t6, 1, (Value[]){t7}, "可能回收");
                t2 = t8;
            }
        }
        BH_RETURN(fr, t2);
    } else {
    }
    bh_line = 976;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[158])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 977;
        t2 = bh_chk(G[184], "可能回收");
        t3 = bh_index(L[0], bh_int(2LL));
        t4 = bh_call(t2, 1, (Value[]){t3}, "可能回收");
        if (bh_truthy(t4)) {
            bh_line = 978;
            BH_RETURN(fr, BH_TRUE);
        } else {
        }
        bh_line = 979;
        {
            t2 = bh_index(L[0], bh_int(3LL));
            T[0] = bh_iter(t2);
            int64_t n1;
            for (n1 = 0; n1 < bh_list_len(T[0]); n1++) {
                BH_SAFEPOINT();
                bh_line = 979;
                if (!L[2].tag && or2) bh_modify_outer("对"); L[2] = bh_list_at(T[0], n1);
                bh_line = 980;
                t3 = bh_chk(G[184], "可能回收");
                if (L[2].tag) t4 = L[2]; else { or2 = 1; t4 = bh_chk(G[185], "对"); }
                t5 = bh_index(t4, bh_int(1LL));
                t6 = bh_call(t3, 1, (Value[]){t5}, "可能回收");
                if (bh_truthy(t6)) {
                    bh_line = 981;
                    BH_RETURN(fr, BH_TRUE);
                } else {
                }
            }
        }
        bh_line = 982;
        BH_RETURN(fr, BH_FALSE);
    } else {
    }
    bh_line = 983;
    {
        t1 = BH_FALSE;
        {
            if (L[1].tag) t2 = L[1]; else { or1 = 1; t2 = bh_chk(G[65], "种类"); }
            if (bh_cmp(0, t2, K[35])) {
                t1 = BH_TRUE;
            }
        }
        if (bh_truthy(t1)) {
            t0 = t1;
        } else {
            t3 = BH_FALSE;
            {
                bh_line = 983;
                if (L[1].tag) t4 = L[1]; else { or1 = 1; t4 = bh_chk(G[65], "种类"); }
                if (bh_cmp(0, t4, K[36])) {
                    t3 = BH_TRUE;
                }
            }
            t0 = t3;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 984;
        {
            t6 = bh_chk(G[184], "可能回收");
            t7 = bh_index(L[0], bh_int(2LL));
            t8 = bh_call(t6, 1, (Value[]){t7}, "可能回收");
            if (bh_truthy(t8)) {
                t5 = t8;
            } else {
                bh_line = 984;
                t9 = bh_chk(G[184], "可能回收");
                t10 = bh_index(L[0], bh_int(3LL));
                t11 = bh_call(t9, 1, (Value[]){t10}, "可能回收");
                t5 = t11;
            }
        }
        BH_RETURN(fr, t5);
    } else {
    }
    bh_line = 985;
    {
        t1 = BH_FALSE;
        {
            if (L[1].tag) t2 = L[1]; else { or1 = 1; t2 = bh_chk(G[65], "种类"); }
            if (bh_cmp(0, t2, K[37])) {
                t1 = BH_TRUE;
            }
        }
        if (bh_truthy(t1)) {
            t0 = t1;
        } else {
            t3 = BH_FALSE;
            {
                bh_line = 985;
                if (L[1].tag) t4 = L[1]; else { or1 = 1; t4 = bh_chk(G[65], "种类"); }
                if (bh_cmp(0, t4, K[163])) {
                    t3 = BH_TRUE;
                }
            }
            t0 = t3;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 986;
        t5 = bh_chk(G[184], "可能回收");
        t6 = bh_index(L[0], bh_int(2LL));
        t7 = bh_call(t5, 1, (Value[]){t6}, "可能回收");
        BH_RETURN(fr, t7);
    } else {
    }
    bh_line = 987;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[141])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 988;
        {
            t3 = bh_chk(G[184], "可能回收");
            t4 = bh_index(L[0], bh_int(2LL));
            t5 = bh_call(t3, 1, (Value[]){t4}, "可能回收");
            if (bh_truthy(t5)) {
                t2 = t5;
            } else {
                bh_line = 988;
                t6 = bh_chk(G[184], "可能回收");
                t7 = bh_index(L[0], bh_int(3LL));
                t8 = bh_call(t6, 1, (Value[]){t7}, "可能回收");
                t2 = t8;
            }
        }
        BH_RETURN(fr, t2);
    } else {
    }
    bh_line = 989;
    t0 = BH_FALSE;
    {
        t1 = bh_chk(G[181], "直接内置");
        t2 = bh_call(t1, 1, (Value[]){L[0]}, "直接内置");
        if (bh_cmp(1, t2, BH_NIL)) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 990;
        t3 = bh_chk(G[182], "一串可能回收");
        t4 = bh_index(L[0], bh_int(3LL));
        t5 = bh_call(t3, 1, (Value[]){t4}, "一串可能回收");
        BH_RETURN(fr, t5);
    } else {
    }
    bh_line = 991;
    BH_RETURN(fr, BH_TRUE);
    BH_RETURN(fr, BH_NIL);
}

static Value fn66(Frame *up, Value *a) {    /* 函数 摊平，第 994 行 */
    struct { BhRoots h; Value v[4]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 4);
    L = fr.v; T = fr.v + 3;
    Value t0, t1, t2, t3;
    L[0] = a[0];    /* 对们 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    BH_SAFEPOINT();
    bh_line = 996;
    t0 = bh_list_make(0, NULL);
    if (!L[1].tag && or1) bh_modify_outer("结果"); L[1] = t0;
    bh_line = 997;
    {
        T[0] = bh_iter(L[0]);
        int64_t n1;
        for (n1 = 0; n1 < bh_list_len(T[0]); n1++) {
            BH_SAFEPOINT();
            bh_line = 997;
            if (!L[2].tag && or2) bh_modify_outer("对"); L[2] = bh_list_at(T[0], n1);
            bh_line = 998;
            if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[186], "结果"); }
            if (L[2].tag) t1 = L[2]; else { or2 = 1; t1 = bh_chk(G[185], "对"); }
            t2 = bh_index(t1, bh_int(0LL));
            t3 = bi_append(2, (Value[]){t0, t2});
            bh_line = 999;
            if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[186], "结果"); }
            if (L[2].tag) t1 = L[2]; else { or2 = 1; t1 = bh_chk(G[185], "对"); }
            t2 = bh_index(t1, bh_int(1LL));
            t3 = bi_append(2, (Value[]){t0, t2});
        }
    }
    bh_line = 1000;
    if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[186], "结果"); }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn67(Frame *up, Value *a) {    /* 函数 一串可能回收，第 1003 行 */
    struct { BhRoots h; Value v[3]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 3);
    L = fr.v; T = fr.v + 2;
    Value t0, t1, t2;
    L[0] = a[0];    /* 节点们 */
    char or0 = 0;
    char or1 = 0;
    BH_SAFEPOINT();
    bh_line = 1004;
    {
        T[0] = bh_iter(L[0]);
        int64_t n1;
        for (n1 = 0; n1 < bh_list_len(T[0]); n1++) {
            BH_SAFEPOINT();
            bh_line = 1004;
            if (!L[1].tag && or1) bh_modify_outer("节点"); L[1] = bh_list_at(T[0], n1);
            bh_line = 1005;
            t0 = bh_chk(G[184], "可能回收");
            if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[117], "节点"); }
            t2 = bh_call(t0, 1, (Value[]){t1}, "可能回收");
            if (bh_truthy(t2)) {
                bh_line = 1006;
                BH_RETURN(fr, BH_TRUE);
            } else {
            }
        }
    }
    bh_line = 1007;
    BH_RETURN(fr, BH_FALSE);
    BH_RETURN(fr, BH_NIL);
}

static Value fn68(Frame *up, Value *a) {    /* 函数 后面可能回收，第 1010 行 */
    struct { BhRoots h; Value v[4]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 4);
    L = fr.v; T = fr.v + 4;
    Value t0, t1, t2, t3, t4, t5;
    L[0] = a[0];    /* 节点们 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    BH_SAFEPOINT();
    bh_line = 1012;
    t0 = bh_list_make(1, (Value[]){BH_FALSE});
    t1 = bi_len(1, (Value[]){L[0]});
    t2 = bh_mul(t0, t1);
    if (!L[1].tag && or1) bh_modify_outer("结果"); L[1] = t2;
    bh_line = 1013;
    if (!L[2].tag && or2) bh_modify_outer("有"); L[2] = BH_FALSE;
    bh_line = 1014;
    t0 = bi_len(1, (Value[]){L[0]});
    t1 = bh_sub(t0, bh_int(1LL));
    if (!L[3].tag && or3) bh_modify_outer("下标"); L[3] = t1;
    bh_line = 1015;
    for (;;) {
        BH_SAFEPOINT();
        t0 = BH_FALSE;
        {
            bh_line = 1015;
            if (L[3].tag) t1 = L[3]; else { or3 = 1; t1 = bh_chk(G[19], "下标"); }
            if (bh_cmp(5, t1, bh_int(0LL))) {
                t0 = BH_TRUE;
            }
        }
        if (!bh_truthy(t0)) break;
        bh_line = 1016;
        if (L[2].tag) t2 = L[2]; else { or2 = 1; t2 = bh_chk(G[187], "有"); }
        if (L[1].tag) t3 = L[1]; else { or1 = 1; t3 = bh_chk(G[186], "结果"); }
        if (L[3].tag) t4 = L[3]; else { or3 = 1; t4 = bh_chk(G[19], "下标"); }
        bh_set_index(t3, t4, t2);
        bh_line = 1017;
        t2 = bh_chk(G[184], "可能回收");
        if (L[3].tag) t3 = L[3]; else { or3 = 1; t3 = bh_chk(G[19], "下标"); }
        t4 = bh_index(L[0], t3);
        t5 = bh_call(t2, 1, (Value[]){t4}, "可能回收");
        if (bh_truthy(t5)) {
            bh_line = 1018;
            if (!L[2].tag && or2) bh_modify_outer("有"); L[2] = BH_TRUE;
        } else {
        }
        bh_line = 1019;
        if (L[3].tag) t2 = L[3]; else { or3 = 1; t2 = bh_chk(G[19], "下标"); }
        t3 = bh_sub(t2, bh_int(1LL));
        if (!L[3].tag && or3) bh_modify_outer("下标"); L[3] = t3;
    }
    bh_line = 1020;
    if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[186], "结果"); }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn69(Frame *up, Value *a) {    /* 函数 是叶子，第 1023 行 */
    struct { BhRoots h; Value v[6]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 6);
    L = fr.v; T = fr.v + 4;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17, t18, t19;
    L[0] = a[0];    /* 语句表 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    BH_SAFEPOINT();
    bh_line = 1025;
    {
        T[0] = bh_iter(L[0]);
        int64_t n1;
        for (n1 = 0; n1 < bh_list_len(T[0]); n1++) {
            BH_SAFEPOINT();
            bh_line = 1025;
            if (!L[1].tag && or1) bh_modify_outer("句"); L[1] = bh_list_at(T[0], n1);
            bh_line = 1026;
            if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[164], "句"); }
            t1 = bh_index(t0, bh_int(0LL));
            if (!L[2].tag && or2) bh_modify_outer("种类"); L[2] = t1;
            bh_line = 1027;
            {
                {
                    {
                        t3 = BH_FALSE;
                        {
                            if (L[2].tag) t4 = L[2]; else { or2 = 1; t4 = bh_chk(G[65], "种类"); }
                            if (bh_cmp(0, t4, K[147])) {
                                t3 = BH_TRUE;
                            }
                        }
                        if (bh_truthy(t3)) {
                            t2 = t3;
                        } else {
                            t5 = BH_FALSE;
                            {
                                bh_line = 1027;
                                if (L[2].tag) t6 = L[2]; else { or2 = 1; t6 = bh_chk(G[65], "种类"); }
                                if (bh_cmp(0, t6, K[149])) {
                                    t5 = BH_TRUE;
                                }
                            }
                            t2 = t5;
                        }
                    }
                    if (bh_truthy(t2)) {
                        t1 = t2;
                    } else {
                        t7 = BH_FALSE;
                        {
                            bh_line = 1027;
                            if (L[2].tag) t8 = L[2]; else { or2 = 1; t8 = bh_chk(G[65], "种类"); }
                            if (bh_cmp(0, t8, K[151])) {
                                t7 = BH_TRUE;
                            }
                        }
                        t1 = t7;
                    }
                }
                if (bh_truthy(t1)) {
                    t0 = t1;
                } else {
                    t9 = BH_FALSE;
                    {
                        bh_line = 1027;
                        if (L[2].tag) t10 = L[2]; else { or2 = 1; t10 = bh_chk(G[65], "种类"); }
                        if (bh_cmp(0, t10, K[156])) {
                            t9 = BH_TRUE;
                        }
                    }
                    t0 = t9;
                }
            }
            if (bh_truthy(t0)) {
                bh_line = 1028;
                BH_RETURN(fr, BH_FALSE);
            } else {
            }
            bh_line = 1029;
            t0 = BH_FALSE;
            {
                if (L[2].tag) t1 = L[2]; else { or2 = 1; t1 = bh_chk(G[65], "种类"); }
                if (bh_cmp(0, t1, K[127])) {
                    t0 = BH_TRUE;
                }
            }
            if (bh_truthy(t0)) {
                bh_line = 1030;
                t2 = bh_chk(G[184], "可能回收");
                if (L[1].tag) t3 = L[1]; else { or1 = 1; t3 = bh_chk(G[164], "句"); }
                t4 = bh_index(t3, bh_int(2LL));
                t5 = bh_call(t2, 1, (Value[]){t4}, "可能回收");
                if (bh_truthy(t5)) {
                    bh_line = 1031;
                    BH_RETURN(fr, BH_FALSE);
                } else {
                }
            } else {
                t2 = BH_FALSE;
                {
                    bh_line = 1032;
                    if (L[2].tag) t3 = L[2]; else { or2 = 1; t3 = bh_chk(G[65], "种类"); }
                    if (bh_cmp(0, t3, K[143])) {
                        t2 = BH_TRUE;
                    }
                }
                if (bh_truthy(t2)) {
                    bh_line = 1033;
                    {
                        t5 = bh_chk(G[184], "可能回收");
                        if (L[1].tag) t6 = L[1]; else { or1 = 1; t6 = bh_chk(G[164], "句"); }
                        t7 = bh_index(t6, bh_int(4LL));
                        t8 = bh_call(t5, 1, (Value[]){t7}, "可能回收");
                        if (bh_truthy(t8)) {
                            t4 = t8;
                        } else {
                            bh_line = 1033;
                            t9 = bh_chk(G[184], "可能回收");
                            if (L[1].tag) t10 = L[1]; else { or1 = 1; t10 = bh_chk(G[164], "句"); }
                            t11 = bh_index(t10, bh_int(2LL));
                            t12 = bh_call(t9, 1, (Value[]){t11}, "可能回收");
                            t4 = t12;
                        }
                    }
                    if (bh_truthy(t4)) {
                        bh_line = 1034;
                        BH_RETURN(fr, BH_FALSE);
                    } else {
                    }
                } else {
                    t4 = BH_FALSE;
                    {
                        bh_line = 1035;
                        if (L[2].tag) t5 = L[2]; else { or2 = 1; t5 = bh_chk(G[65], "种类"); }
                        if (bh_cmp(0, t5, K[135])) {
                            t4 = BH_TRUE;
                        }
                    }
                    if (bh_truthy(t4)) {
                        bh_line = 1036;
                        {
                            t7 = BH_FALSE;
                            {
                                if (L[1].tag) t8 = L[1]; else { or1 = 1; t8 = bh_chk(G[164], "句"); }
                                t9 = bh_index(t8, bh_int(2LL));
                                if (bh_cmp(1, t9, BH_NIL)) {
                                    t7 = BH_TRUE;
                                }
                            }
                            if (bh_truthy(t7)) {
                                bh_line = 1036;
                                t10 = bh_chk(G[184], "可能回收");
                                if (L[1].tag) t11 = L[1]; else { or1 = 1; t11 = bh_chk(G[164], "句"); }
                                t12 = bh_index(t11, bh_int(2LL));
                                t13 = bh_call(t10, 1, (Value[]){t12}, "可能回收");
                                t6 = t13;
                            } else {
                                t6 = t7;
                            }
                        }
                        if (bh_truthy(t6)) {
                            bh_line = 1037;
                            BH_RETURN(fr, BH_FALSE);
                        } else {
                        }
                    } else {
                        t6 = BH_FALSE;
                        {
                            bh_line = 1038;
                            if (L[2].tag) t7 = L[2]; else { or2 = 1; t7 = bh_chk(G[65], "种类"); }
                            if (bh_cmp(0, t7, K[146])) {
                                t6 = BH_TRUE;
                            }
                        }
                        if (bh_truthy(t6)) {
                            bh_line = 1039;
                            {
                                if (L[1].tag) t8 = L[1]; else { or1 = 1; t8 = bh_chk(G[164], "句"); }
                                t9 = bh_index(t8, bh_int(2LL));
                                T[1] = bh_iter(t9);
                                int64_t n2;
                                for (n2 = 0; n2 < bh_list_len(T[1]); n2++) {
                                    BH_SAFEPOINT();
                                    bh_line = 1039;
                                    if (!L[3].tag && or3) bh_modify_outer("分支"); L[3] = bh_list_at(T[1], n2);
                                    bh_line = 1040;
                                    {
                                        t11 = bh_chk(G[184], "可能回收");
                                        if (L[3].tag) t12 = L[3]; else { or3 = 1; t12 = bh_chk(G[166], "分支"); }
                                        t13 = bh_index(t12, bh_int(0LL));
                                        t14 = bh_call(t11, 1, (Value[]){t13}, "可能回收");
                                        if (bh_truthy(t14)) {
                                            t10 = t14;
                                        } else {
                                            bh_line = 1040;
                                            t15 = bh_chk(G[189], "是叶子");
                                            if (L[3].tag) t16 = L[3]; else { or3 = 1; t16 = bh_chk(G[166], "分支"); }
                                            t17 = bh_index(t16, bh_int(1LL));
                                            t18 = bh_call(t15, 1, (Value[]){t17}, "是叶子");
                                            t19 = bh_bool(!bh_truthy(t18));
                                            t10 = t19;
                                        }
                                    }
                                    if (bh_truthy(t10)) {
                                        bh_line = 1041;
                                        BH_RETURN(fr, BH_FALSE);
                                    } else {
                                    }
                                }
                            }
                            bh_line = 1042;
                            {
                                t9 = BH_FALSE;
                                {
                                    if (L[1].tag) t10 = L[1]; else { or1 = 1; t10 = bh_chk(G[164], "句"); }
                                    t11 = bh_index(t10, bh_int(3LL));
                                    if (bh_cmp(1, t11, BH_NIL)) {
                                        t9 = BH_TRUE;
                                    }
                                }
                                if (bh_truthy(t9)) {
                                    bh_line = 1042;
                                    t12 = bh_chk(G[189], "是叶子");
                                    if (L[1].tag) t13 = L[1]; else { or1 = 1; t13 = bh_chk(G[164], "句"); }
                                    t14 = bh_index(t13, bh_int(3LL));
                                    t15 = bh_call(t12, 1, (Value[]){t14}, "是叶子");
                                    t16 = bh_bool(!bh_truthy(t15));
                                    t8 = t16;
                                } else {
                                    t8 = t9;
                                }
                            }
                            if (bh_truthy(t8)) {
                                bh_line = 1043;
                                BH_RETURN(fr, BH_FALSE);
                            } else {
                            }
                        } else {
                        }
                    }
                }
            }
        }
    }
    bh_line = 1044;
    BH_RETURN(fr, BH_TRUE);
    BH_RETURN(fr, BH_NIL);
}

static Value fn70(Frame *up, Value *a) {    /* 函数 全局取值，第 1049 行 */
    struct { BhRoots h; Value v[2]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 2);
    L = fr.v; T = fr.v + 1;
    Value t0, t1, t2, t3, t4, t5, t6, t7;
    L[0] = a[0];    /* 名 */
    char or0 = 0;
    BH_SAFEPOINT();
    bh_line = 1050;
    t0 = bh_chk(G[158], "全局槽");
    t1 = bh_call(t0, 1, (Value[]){L[0]}, "全局槽");
    t2 = bh_add(K[195], t1);
    t3 = bh_add(t2, K[196]);
    T[0] = t3;
    t4 = bh_chk(G[160], "C文字");
    t5 = bh_call(t4, 1, (Value[]){L[0]}, "C文字");
    t6 = bh_add(T[0], t5);
    t7 = bh_add(t6, K[102]);
    BH_RETURN(fr, t7);
    BH_RETURN(fr, BH_NIL);
}

static Value fn71(Frame *up, Value *a) {    /* 函数 外层取值，第 1053 行 */
    struct { BhRoots h; Value v[7]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 7);
    L = fr.v; T = fr.v + 7;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15;
    L[0] = a[0];    /* 名 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    char or5 = 0;
    char or6 = 0;
    BH_SAFEPOINT();
    bh_line = 1055;
    if (!L[1].tag && or1) bh_modify_outer("路径"); L[1] = K[197];
    bh_line = 1056;
    t0 = bh_chk(G[126], "环境");
    t1 = bh_chk(G[138], "外层位");
    t2 = bh_index(t0, t1);
    if (!L[2].tag && or2) bh_modify_outer("外"); L[2] = t2;
    bh_line = 1057;
    t0 = bh_list_make(0, NULL);
    if (!L[3].tag && or3) bh_modify_outer("候选"); L[3] = t0;
    bh_line = 1058;
    for (;;) {
        BH_SAFEPOINT();
        {
            t1 = BH_FALSE;
            {
                bh_line = 1058;
                if (L[2].tag) t2 = L[2]; else { or2 = 1; t2 = bh_chk(G[179], "外"); }
                if (bh_cmp(1, t2, BH_NIL)) {
                    t1 = BH_TRUE;
                }
            }
            if (bh_truthy(t1)) {
                bh_line = 1058;
                if (L[2].tag) t3 = L[2]; else { or2 = 1; t3 = bh_chk(G[179], "外"); }
                t4 = bh_chk(G[139], "模块位");
                t5 = bh_index(t3, t4);
                t6 = bh_bool(!bh_truthy(t5));
                t0 = t6;
            } else {
                t0 = t1;
            }
        }
        if (!bh_truthy(t0)) break;
        bh_line = 1059;
        if (L[2].tag) t7 = L[2]; else { or2 = 1; t7 = bh_chk(G[179], "外"); }
        t8 = bh_chk(G[132], "局部位");
        t9 = bh_index(t7, t8);
        t10 = bi_find(2, (Value[]){t9, L[0]});
        if (!L[4].tag && or4) bh_modify_outer("槽"); L[4] = t10;
        bh_line = 1060;
        t7 = BH_FALSE;
        {
            if (L[4].tag) t8 = L[4]; else { or4 = 1; t8 = bh_chk(G[191], "槽"); }
            if (bh_cmp(5, t8, bh_int(0LL))) {
                t7 = BH_TRUE;
            }
        }
        if (bh_truthy(t7)) {
            bh_line = 1061;
            if (L[3].tag) t9 = L[3]; else { or3 = 1; t9 = bh_chk(G[192], "候选"); }
            if (L[1].tag) t10 = L[1]; else { or1 = 1; t10 = bh_chk(G[193], "路径"); }
            t11 = bh_add(t10, K[198]);
            if (L[4].tag) t12 = L[4]; else { or4 = 1; t12 = bh_chk(G[191], "槽"); }
            t13 = bh_add(t11, t12);
            t14 = bh_add(t13, K[50]);
            t15 = bi_append(2, (Value[]){t9, t14});
        } else {
        }
        bh_line = 1062;
        if (L[1].tag) t7 = L[1]; else { or1 = 1; t7 = bh_chk(G[193], "路径"); }
        t8 = bh_add(t7, K[199]);
        if (!L[1].tag && or1) bh_modify_outer("路径"); L[1] = t8;
        bh_line = 1063;
        if (L[2].tag) t7 = L[2]; else { or2 = 1; t7 = bh_chk(G[179], "外"); }
        t8 = bh_chk(G[138], "外层位");
        t9 = bh_index(t7, t8);
        if (!L[2].tag && or2) bh_modify_outer("外"); L[2] = t9;
    }
    bh_line = 1064;
    t0 = bh_chk(G[190], "全局取值");
    t1 = bh_call(t0, 1, (Value[]){L[0]}, "全局取值");
    if (!L[5].tag && or5) bh_modify_outer("结果"); L[5] = t1;
    bh_line = 1065;
    if (L[3].tag) t0 = L[3]; else { or3 = 1; t0 = bh_chk(G[192], "候选"); }
    t1 = bi_len(1, (Value[]){t0});
    t2 = bh_sub(t1, bh_int(1LL));
    if (!L[6].tag && or6) bh_modify_outer("下标"); L[6] = t2;
    bh_line = 1066;
    for (;;) {
        BH_SAFEPOINT();
        t0 = BH_FALSE;
        {
            bh_line = 1066;
            if (L[6].tag) t1 = L[6]; else { or6 = 1; t1 = bh_chk(G[19], "下标"); }
            if (bh_cmp(5, t1, bh_int(0LL))) {
                t0 = BH_TRUE;
            }
        }
        if (!bh_truthy(t0)) break;
        bh_line = 1067;
        if (L[3].tag) t2 = L[3]; else { or3 = 1; t2 = bh_chk(G[192], "候选"); }
        if (L[6].tag) t3 = L[6]; else { or6 = 1; t3 = bh_chk(G[19], "下标"); }
        t4 = bh_index(t2, t3);
        t5 = bh_add(K[101], t4);
        t6 = bh_add(t5, K[200]);
        if (L[3].tag) t7 = L[3]; else { or3 = 1; t7 = bh_chk(G[192], "候选"); }
        if (L[6].tag) t8 = L[6]; else { or6 = 1; t8 = bh_chk(G[19], "下标"); }
        t9 = bh_index(t7, t8);
        t10 = bh_add(t6, t9);
        t11 = bh_add(t10, K[201]);
        if (L[5].tag) t12 = L[5]; else { or5 = 1; t12 = bh_chk(G[186], "结果"); }
        t13 = bh_add(t11, t12);
        t14 = bh_add(t13, K[102]);
        if (!L[5].tag && or5) bh_modify_outer("结果"); L[5] = t14;
        bh_line = 1068;
        if (L[6].tag) t2 = L[6]; else { or6 = 1; t2 = bh_chk(G[19], "下标"); }
        t3 = bh_sub(t2, bh_int(1LL));
        if (!L[6].tag && or6) bh_modify_outer("下标"); L[6] = t3;
    }
    bh_line = 1069;
    if (L[5].tag) t0 = L[5]; else { or5 = 1; t0 = bh_chk(G[186], "结果"); }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn72(Frame *up, Value *a) {    /* 函数 读变量，第 1072 行 */
    struct { BhRoots h; Value v[8]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 8);
    L = fr.v; T = fr.v + 5;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17, t18, t19, t20, t21, t22, t23, t24;
    L[0] = a[0];    /* 名 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    BH_SAFEPOINT();
    bh_line = 1073;
    t0 = bh_chk(G[126], "环境");
    t1 = bh_chk(G[139], "模块位");
    t2 = bh_index(t0, t1);
    if (bh_truthy(t2)) {
        bh_line = 1074;
        t3 = bh_chk(G[152], "新临时");
        t4 = bh_call(t3, 0, NULL, "新临时");
        if (!L[1].tag && or1) bh_modify_outer("临"); L[1] = t4;
        bh_line = 1075;
        t3 = bh_chk(G[147], "出");
        T[0] = t3;
        if (L[1].tag) t4 = L[1]; else { or1 = 1; t4 = bh_chk(G[195], "临"); }
        t5 = bh_add(t4, K[182]);
        T[1] = t5;
        t6 = bh_chk(G[190], "全局取值");
        t7 = bh_call(t6, 1, (Value[]){L[0]}, "全局取值");
        t8 = bh_add(T[1], t7);
        t9 = bh_add(t8, K[52]);
        t10 = bh_call(T[0], 1, (Value[]){t9}, "出");
        bh_line = 1076;
        if (L[1].tag) t3 = L[1]; else { or1 = 1; t3 = bh_chk(G[195], "临"); }
        BH_RETURN(fr, t3);
    } else {
    }
    bh_line = 1077;
    t0 = bh_chk(G[126], "环境");
    t1 = bh_chk(G[132], "局部位");
    t2 = bh_index(t0, t1);
    t3 = bi_find(2, (Value[]){t2, L[0]});
    if (!L[2].tag && or2) bh_modify_outer("槽"); L[2] = t3;
    bh_line = 1078;
    t0 = bh_chk(G[126], "环境");
    t1 = bh_chk(G[134], "声明位");
    t2 = bh_index(t0, t1);
    t3 = bi_find(2, (Value[]){t2, L[0]});
    if (!L[3].tag && or3) bh_modify_outer("声"); L[3] = t3;
    bh_line = 1079;
    {
        {
            t2 = BH_FALSE;
            {
                if (L[2].tag) t3 = L[2]; else { or2 = 1; t3 = bh_chk(G[191], "槽"); }
                if (bh_cmp(5, t3, bh_int(0LL))) {
                    t2 = BH_TRUE;
                }
            }
            if (bh_truthy(t2)) {
                t4 = BH_FALSE;
                {
                    bh_line = 1079;
                    if (L[2].tag) t5 = L[2]; else { or2 = 1; t5 = bh_chk(G[191], "槽"); }
                    t6 = bh_chk(G[126], "环境");
                    t7 = bh_chk(G[133], "参数位");
                    t8 = bh_index(t6, t7);
                    if (bh_cmp(2, t5, t8)) {
                        t4 = BH_TRUE;
                    }
                }
                t1 = t4;
            } else {
                t1 = t2;
            }
        }
        if (bh_truthy(t1)) {
            t9 = BH_FALSE;
            {
                bh_line = 1079;
                if (L[3].tag) t10 = L[3]; else { or3 = 1; t10 = bh_chk(G[196], "声"); }
                if (bh_cmp(2, t10, bh_int(0LL))) {
                    t9 = BH_TRUE;
                }
            }
            t0 = t9;
        } else {
            t0 = t1;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1080;
        if (L[2].tag) t11 = L[2]; else { or2 = 1; t11 = bh_chk(G[191], "槽"); }
        t12 = bh_add(K[177], t11);
        t13 = bh_add(t12, K[50]);
        BH_RETURN(fr, t13);
    } else {
    }
    bh_line = 1081;
    t0 = bh_chk(G[152], "新临时");
    t1 = bh_call(t0, 0, NULL, "新临时");
    if (!L[1].tag && or1) bh_modify_outer("临"); L[1] = t1;
    bh_line = 1082;
    t0 = BH_FALSE;
    {
        if (L[2].tag) t1 = L[2]; else { or2 = 1; t1 = bh_chk(G[191], "槽"); }
        if (bh_cmp(5, t1, bh_int(0LL))) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1083;
        if (!L[4].tag && or4) bh_modify_outer("前缀"); L[4] = K[79];
        bh_line = 1084;
        t2 = BH_FALSE;
        {
            if (L[3].tag) t3 = L[3]; else { or3 = 1; t3 = bh_chk(G[196], "声"); }
            if (bh_cmp(5, t3, bh_int(0LL))) {
                t2 = BH_TRUE;
            }
        }
        if (bh_truthy(t2)) {
            bh_line = 1085;
            if (L[3].tag) t4 = L[3]; else { or3 = 1; t4 = bh_chk(G[196], "声"); }
            t5 = bh_add(K[202], t4);
            t6 = bh_add(t5, K[203]);
            if (L[1].tag) t7 = L[1]; else { or1 = 1; t7 = bh_chk(G[195], "临"); }
            t8 = bh_add(t6, t7);
            t9 = bh_add(t8, K[182]);
            T[0] = t9;
            t10 = bh_chk(G[190], "全局取值");
            t11 = bh_call(t10, 1, (Value[]){L[0]}, "全局取值");
            t12 = bh_add(T[0], t11);
            t13 = bh_add(t12, K[204]);
            if (!L[4].tag && or4) bh_modify_outer("前缀"); L[4] = t13;
        } else {
        }
        bh_line = 1086;
        t2 = bh_chk(G[147], "出");
        T[0] = t2;
        if (L[4].tag) t3 = L[4]; else { or4 = 1; t3 = bh_chk(G[197], "前缀"); }
        t4 = bh_add(t3, K[205]);
        if (L[2].tag) t5 = L[2]; else { or2 = 1; t5 = bh_chk(G[191], "槽"); }
        t6 = bh_add(t4, t5);
        t7 = bh_add(t6, K[206]);
        if (L[1].tag) t8 = L[1]; else { or1 = 1; t8 = bh_chk(G[195], "临"); }
        t9 = bh_add(t7, t8);
        t10 = bh_add(t9, K[207]);
        if (L[2].tag) t11 = L[2]; else { or2 = 1; t11 = bh_chk(G[191], "槽"); }
        t12 = bh_add(t10, t11);
        t13 = bh_add(t12, K[208]);
        if (L[2].tag) t14 = L[2]; else { or2 = 1; t14 = bh_chk(G[191], "槽"); }
        t15 = bh_add(t13, t14);
        t16 = bh_add(t15, K[209]);
        if (L[1].tag) t17 = L[1]; else { or1 = 1; t17 = bh_chk(G[195], "临"); }
        t18 = bh_add(t16, t17);
        t19 = bh_add(t18, K[182]);
        T[1] = t19;
        t20 = bh_chk(G[194], "外层取值");
        t21 = bh_call(t20, 1, (Value[]){L[0]}, "外层取值");
        t22 = bh_add(T[1], t21);
        t23 = bh_add(t22, K[210]);
        t24 = bh_call(T[0], 1, (Value[]){t23}, "出");
    } else {
        t2 = BH_FALSE;
        {
            bh_line = 1087;
            if (L[3].tag) t3 = L[3]; else { or3 = 1; t3 = bh_chk(G[196], "声"); }
            if (bh_cmp(5, t3, bh_int(0LL))) {
                t2 = BH_TRUE;
            }
        }
        if (bh_truthy(t2)) {
            bh_line = 1088;
            t4 = bh_chk(G[147], "出");
            T[0] = t4;
            if (L[1].tag) t5 = L[1]; else { or1 = 1; t5 = bh_chk(G[195], "临"); }
            t6 = bh_add(t5, K[211]);
            if (L[3].tag) t7 = L[3]; else { or3 = 1; t7 = bh_chk(G[196], "声"); }
            t8 = bh_add(t6, t7);
            t9 = bh_add(t8, K[212]);
            T[1] = t9;
            t10 = bh_chk(G[190], "全局取值");
            t11 = bh_call(t10, 1, (Value[]){L[0]}, "全局取值");
            t12 = bh_add(T[1], t11);
            t13 = bh_add(t12, K[201]);
            T[2] = t13;
            t14 = bh_chk(G[194], "外层取值");
            t15 = bh_call(t14, 1, (Value[]){L[0]}, "外层取值");
            t16 = bh_add(T[2], t15);
            t17 = bh_add(t16, K[52]);
            t18 = bh_call(T[0], 1, (Value[]){t17}, "出");
        } else {
            bh_line = 1090;
            t4 = bh_chk(G[147], "出");
            T[0] = t4;
            if (L[1].tag) t5 = L[1]; else { or1 = 1; t5 = bh_chk(G[195], "临"); }
            t6 = bh_add(t5, K[182]);
            T[1] = t6;
            t7 = bh_chk(G[194], "外层取值");
            t8 = bh_call(t7, 1, (Value[]){L[0]}, "外层取值");
            t9 = bh_add(T[1], t8);
            t10 = bh_add(t9, K[52]);
            t11 = bh_call(T[0], 1, (Value[]){t10}, "出");
        }
    }
    bh_line = 1091;
    if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[195], "临"); }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn73(Frame *up, Value *a) {    /* 函数 写变量，第 1094 行 */
    struct { BhRoots h; Value v[7]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 7);
    L = fr.v; T = fr.v + 5;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17, t18, t19, t20, t21, t22, t23;
    L[0] = a[0];    /* 名 */
    char or0 = 0;
    L[1] = a[1];    /* 值式 */
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    BH_SAFEPOINT();
    bh_line = 1095;
    t0 = bh_chk(G[126], "环境");
    t1 = bh_chk(G[139], "模块位");
    t2 = bh_index(t0, t1);
    if (bh_truthy(t2)) {
        bh_line = 1096;
        t3 = bh_chk(G[147], "出");
        T[0] = t3;
        t4 = bh_chk(G[158], "全局槽");
        t5 = bh_call(t4, 1, (Value[]){L[0]}, "全局槽");
        t6 = bh_add(K[213], t5);
        t7 = bh_add(t6, K[214]);
        t8 = bh_add(t7, L[1]);
        t9 = bh_add(t8, K[52]);
        t10 = bh_call(T[0], 1, (Value[]){t9}, "出");
        bh_line = 1097;
        BH_RETURN(fr, BH_NIL);
    } else {
    }
    bh_line = 1098;
    t0 = bh_chk(G[126], "环境");
    t1 = bh_chk(G[132], "局部位");
    t2 = bh_index(t0, t1);
    t3 = bi_find(2, (Value[]){t2, L[0]});
    if (!L[2].tag && or2) bh_modify_outer("槽"); L[2] = t3;
    bh_line = 1099;
    t0 = bh_chk(G[126], "环境");
    t1 = bh_chk(G[134], "声明位");
    t2 = bh_index(t0, t1);
    t3 = bi_find(2, (Value[]){t2, L[0]});
    if (!L[3].tag && or3) bh_modify_outer("声"); L[3] = t3;
    bh_line = 1100;
    if (L[2].tag) t0 = L[2]; else { or2 = 1; t0 = bh_chk(G[191], "槽"); }
    t1 = bh_add(K[215], t0);
    t2 = bh_add(t1, K[216]);
    if (L[2].tag) t3 = L[2]; else { or2 = 1; t3 = bh_chk(G[191], "槽"); }
    t4 = bh_add(t2, t3);
    t5 = bh_add(t4, K[217]);
    T[0] = t5;
    t6 = bh_chk(G[160], "C文字");
    t7 = bh_call(t6, 1, (Value[]){L[0]}, "C文字");
    t8 = bh_add(T[0], t7);
    t9 = bh_add(t8, K[218]);
    if (L[2].tag) t10 = L[2]; else { or2 = 1; t10 = bh_chk(G[191], "槽"); }
    t11 = bh_add(t9, t10);
    t12 = bh_add(t11, K[214]);
    t13 = bh_add(t12, L[1]);
    t14 = bh_add(t13, K[52]);
    if (!L[4].tag && or4) bh_modify_outer("本地"); L[4] = t14;
    bh_line = 1101;
    {
        t1 = BH_FALSE;
        {
            if (L[2].tag) t2 = L[2]; else { or2 = 1; t2 = bh_chk(G[191], "槽"); }
            t3 = bh_chk(G[126], "环境");
            t4 = bh_chk(G[133], "参数位");
            t5 = bh_index(t3, t4);
            if (bh_cmp(2, t2, t5)) {
                t1 = BH_TRUE;
            }
        }
        if (bh_truthy(t1)) {
            t6 = BH_FALSE;
            {
                bh_line = 1101;
                if (L[3].tag) t7 = L[3]; else { or3 = 1; t7 = bh_chk(G[196], "声"); }
                if (bh_cmp(2, t7, bh_int(0LL))) {
                    t6 = BH_TRUE;
                }
            }
            t0 = t6;
        } else {
            t0 = t1;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1102;
        t8 = bh_chk(G[147], "出");
        if (L[2].tag) t9 = L[2]; else { or2 = 1; t9 = bh_chk(G[191], "槽"); }
        t10 = bh_add(K[177], t9);
        t11 = bh_add(t10, K[214]);
        t12 = bh_add(t11, L[1]);
        t13 = bh_add(t12, K[52]);
        t14 = bh_call(t8, 1, (Value[]){t13}, "出");
    } else {
        t8 = BH_FALSE;
        {
            bh_line = 1103;
            if (L[3].tag) t9 = L[3]; else { or3 = 1; t9 = bh_chk(G[196], "声"); }
            if (bh_cmp(5, t9, bh_int(0LL))) {
                t8 = BH_TRUE;
            }
        }
        if (bh_truthy(t8)) {
            bh_line = 1104;
            t10 = bh_chk(G[147], "出");
            T[0] = t10;
            if (L[3].tag) t11 = L[3]; else { or3 = 1; t11 = bh_chk(G[196], "声"); }
            t12 = bh_add(K[202], t11);
            t13 = bh_add(t12, K[219]);
            T[1] = t13;
            t14 = bh_chk(G[158], "全局槽");
            t15 = bh_call(t14, 1, (Value[]){L[0]}, "全局槽");
            t16 = bh_add(T[1], t15);
            t17 = bh_add(t16, K[214]);
            t18 = bh_add(t17, L[1]);
            t19 = bh_add(t18, K[220]);
            if (L[4].tag) t20 = L[4]; else { or4 = 1; t20 = bh_chk(G[199], "本地"); }
            t21 = bh_add(t19, t20);
            t22 = bh_add(t21, K[221]);
            t23 = bh_call(T[0], 1, (Value[]){t22}, "出");
        } else {
            bh_line = 1106;
            t10 = bh_chk(G[147], "出");
            if (L[4].tag) t11 = L[4]; else { or4 = 1; t11 = bh_chk(G[199], "本地"); }
            t12 = bh_call(t10, 1, (Value[]){t11}, "出");
        }
    }
    BH_RETURN(fr, BH_NIL);
}

static Value fn74(Frame *up, Value *a) {    /* 函数 运算函数，第 1111 行 */
    Value L[1] = {{0}};
    Value t0;
    L[0] = a[0];    /* 符 */
    char or0 = 0;
    bh_line = 1112;
    t0 = BH_FALSE;
    {
        if (bh_cmp(0, L[0], K[159])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1112;
        return K[222];
    } else {
    }
    bh_line = 1113;
    t0 = BH_FALSE;
    {
        if (bh_cmp(0, L[0], K[160])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1113;
        return K[223];
    } else {
    }
    bh_line = 1114;
    t0 = BH_FALSE;
    {
        if (bh_cmp(0, L[0], K[53])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1114;
        return K[224];
    } else {
    }
    bh_line = 1115;
    t0 = BH_FALSE;
    {
        if (bh_cmp(0, L[0], K[28])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1115;
        return K[225];
    } else {
    }
    bh_line = 1116;
    return K[226];
    return BH_NIL;
}

static Value fn75(Frame *up, Value *a) {    /* 函数 算一串，第 1119 行 */
    struct { BhRoots h; Value v[7]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 7);
    L = fr.v; T = fr.v + 6;
    Value t0, t1, t2, t3, t4, t5;
    L[0] = a[0];    /* 节点们 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    char or5 = 0;
    BH_SAFEPOINT();
    bh_line = 1121;
    t0 = bh_list_make(0, NULL);
    if (!L[1].tag && or1) bh_modify_outer("式们"); L[1] = t0;
    bh_line = 1122;
    t0 = bh_chk(G[188], "后面可能回收");
    t1 = bh_call(t0, 1, (Value[]){L[0]}, "后面可能回收");
    if (!L[2].tag && or2) bh_modify_outer("后面"); L[2] = t1;
    bh_line = 1123;
    if (!L[3].tag && or3) bh_modify_outer("下标"); L[3] = bh_int(0LL);
    bh_line = 1124;
    {
        T[0] = bh_iter(L[0]);
        int64_t n1;
        for (n1 = 0; n1 < bh_list_len(T[0]); n1++) {
            BH_SAFEPOINT();
            bh_line = 1124;
            if (!L[4].tag && or4) bh_modify_outer("节点"); L[4] = bh_list_at(T[0], n1);
            bh_line = 1125;
            t0 = bh_chk(G[202], "算");
            if (L[4].tag) t1 = L[4]; else { or4 = 1; t1 = bh_chk(G[117], "节点"); }
            t2 = bh_call(t0, 1, (Value[]){t1}, "算");
            if (!L[5].tag && or5) bh_modify_outer("式"); L[5] = t2;
            bh_line = 1126;
            if (L[2].tag) t0 = L[2]; else { or2 = 1; t0 = bh_chk(G[203], "后面"); }
            if (L[3].tag) t1 = L[3]; else { or3 = 1; t1 = bh_chk(G[19], "下标"); }
            t2 = bh_index(t0, t1);
            if (bh_truthy(t2)) {
                bh_line = 1127;
                t3 = bh_chk(G[155], "保根");
                if (L[5].tag) t4 = L[5]; else { or5 = 1; t4 = bh_chk(G[93], "式"); }
                t5 = bh_call(t3, 1, (Value[]){t4}, "保根");
                if (!L[5].tag && or5) bh_modify_outer("式"); L[5] = t5;
            } else {
            }
            bh_line = 1128;
            if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[204], "式们"); }
            if (L[5].tag) t1 = L[5]; else { or5 = 1; t1 = bh_chk(G[93], "式"); }
            t2 = bi_append(2, (Value[]){t0, t1});
            bh_line = 1129;
            if (L[3].tag) t0 = L[3]; else { or3 = 1; t0 = bh_chk(G[19], "下标"); }
            t1 = bh_add(t0, bh_int(1LL));
            if (!L[3].tag && or3) bh_modify_outer("下标"); L[3] = t1;
        }
    }
    bh_line = 1130;
    if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[204], "式们"); }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn76(Frame *up, Value *a) {    /* 函数 数组式，第 1133 行 */
    Value L[1] = {{0}};
    Value t0, t1, t2;
    L[0] = a[0];    /* 式们 */
    char or0 = 0;
    bh_line = 1134;
    t0 = BH_FALSE;
    {
        t1 = bi_len(1, (Value[]){L[0]});
        if (bh_cmp(0, t1, bh_int(0LL))) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1135;
        return K[227];
    } else {
    }
    bh_line = 1136;
    t0 = bi_join(2, (Value[]){L[0], K[229]});
    t1 = bh_add(K[228], t0);
    t2 = bh_add(t1, K[173]);
    return t2;
    return BH_NIL;
}

static Value fn77(Frame *up, Value *a) {    /* 函数 算，第 1139 行 */
    struct { BhRoots h; Value v[16]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 16);
    L = fr.v; T = fr.v + 14;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17, t18;
    L[0] = a[0];    /* 节点 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    char or5 = 0;
    char or6 = 0;
    char or7 = 0;
    char or8 = 0;
    char or9 = 0;
    char or10 = 0;
    char or11 = 0;
    char or12 = 0;
    char or13 = 0;
    BH_SAFEPOINT();
    bh_line = 1141;
    t0 = bh_index(L[0], bh_int(0LL));
    if (!L[1].tag && or1) bh_modify_outer("种类"); L[1] = t0;
    bh_line = 1142;
    t0 = bh_index(L[0], bh_int(1LL));
    if (!L[2].tag && or2) bh_modify_outer("行"); L[2] = t0;
    bh_line = 1143;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[165])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1144;
        t2 = bh_chk(G[161], "数字常量");
        t3 = bh_index(L[0], bh_int(2LL));
        t4 = bh_call(t2, 1, (Value[]){t3}, "数字常量");
        BH_RETURN(fr, t4);
    } else {
    }
    bh_line = 1145;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[166])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1146;
        t2 = bh_chk(G[159], "常量槽");
        t3 = bh_index(L[0], bh_int(2LL));
        t4 = bh_add(K[230], t3);
        t5 = bh_call(t2, 1, (Value[]){t4}, "常量槽");
        t6 = bh_add(K[178], t5);
        t7 = bh_add(t6, K[50]);
        BH_RETURN(fr, t7);
    } else {
    }
    bh_line = 1147;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[167])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1148;
        t2 = BH_FALSE;
        {
            t3 = bh_index(L[0], bh_int(2LL));
            if (bh_cmp(0, t3, K[14])) {
                t2 = BH_TRUE;
            }
        }
        if (bh_truthy(t2)) {
            bh_line = 1148;
            BH_RETURN(fr, K[231]);
        } else {
        }
        bh_line = 1149;
        t2 = BH_FALSE;
        {
            t3 = bh_index(L[0], bh_int(2LL));
            if (bh_cmp(0, t3, K[15])) {
                t2 = BH_TRUE;
            }
        }
        if (bh_truthy(t2)) {
            bh_line = 1149;
            BH_RETURN(fr, K[232]);
        } else {
        }
        bh_line = 1150;
        BH_RETURN(fr, K[233]);
    } else {
    }
    bh_line = 1151;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[128])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1152;
        t2 = bh_chk(G[157], "定行");
        if (L[2].tag) t3 = L[2]; else { or2 = 1; t3 = bh_chk(G[68], "行"); }
        t4 = bh_call(t2, 1, (Value[]){t3}, "定行");
        bh_line = 1153;
        t2 = bh_chk(G[198], "读变量");
        t3 = bh_index(L[0], bh_int(2LL));
        t4 = bh_call(t2, 1, (Value[]){t3}, "读变量");
        BH_RETURN(fr, t4);
    } else {
    }
    bh_line = 1154;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[169])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1155;
        t2 = bh_chk(G[205], "算一串");
        t3 = bh_index(L[0], bh_int(2LL));
        t4 = bh_call(t2, 1, (Value[]){t3}, "算一串");
        if (!L[3].tag && or3) bh_modify_outer("项们"); L[3] = t4;
        bh_line = 1156;
        t2 = bh_chk(G[152], "新临时");
        t3 = bh_call(t2, 0, NULL, "新临时");
        if (!L[4].tag && or4) bh_modify_outer("临"); L[4] = t3;
        bh_line = 1157;
        t2 = bh_chk(G[147], "出");
        T[0] = t2;
        if (L[4].tag) t3 = L[4]; else { or4 = 1; t3 = bh_chk(G[195], "临"); }
        t4 = bh_add(t3, K[234]);
        if (L[3].tag) t5 = L[3]; else { or3 = 1; t5 = bh_chk(G[119], "项们"); }
        t6 = bi_len(1, (Value[]){t5});
        t7 = bh_add(t4, t6);
        t8 = bh_add(t7, K[229]);
        T[1] = t8;
        t9 = bh_chk(G[206], "数组式");
        if (L[3].tag) t10 = L[3]; else { or3 = 1; t10 = bh_chk(G[119], "项们"); }
        t11 = bh_call(t9, 1, (Value[]){t10}, "数组式");
        t12 = bh_add(T[1], t11);
        t13 = bh_add(t12, K[235]);
        t14 = bh_call(T[0], 1, (Value[]){t13}, "出");
        bh_line = 1158;
        if (L[4].tag) t2 = L[4]; else { or4 = 1; t2 = bh_chk(G[195], "临"); }
        BH_RETURN(fr, t2);
    } else {
    }
    bh_line = 1159;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[168])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1160;
        t2 = bh_chk(G[205], "算一串");
        T[0] = t2;
        t3 = bh_chk(G[183], "摊平");
        t4 = bh_index(L[0], bh_int(2LL));
        t5 = bh_call(t3, 1, (Value[]){t4}, "摊平");
        t6 = bh_call(T[0], 1, (Value[]){t5}, "算一串");
        if (!L[3].tag && or3) bh_modify_outer("项们"); L[3] = t6;
        bh_line = 1161;
        t2 = bh_chk(G[157], "定行");
        if (L[2].tag) t3 = L[2]; else { or2 = 1; t3 = bh_chk(G[68], "行"); }
        t4 = bh_call(t2, 1, (Value[]){t3}, "定行");
        bh_line = 1162;
        t2 = bh_chk(G[152], "新临时");
        t3 = bh_call(t2, 0, NULL, "新临时");
        if (!L[4].tag && or4) bh_modify_outer("临"); L[4] = t3;
        bh_line = 1163;
        t2 = bh_chk(G[147], "出");
        T[0] = t2;
        if (L[4].tag) t3 = L[4]; else { or4 = 1; t3 = bh_chk(G[195], "临"); }
        t4 = bh_add(t3, K[236]);
        t5 = bh_index(L[0], bh_int(2LL));
        t6 = bi_len(1, (Value[]){t5});
        t7 = bh_add(t4, t6);
        t8 = bh_add(t7, K[229]);
        T[1] = t8;
        t9 = bh_chk(G[206], "数组式");
        if (L[3].tag) t10 = L[3]; else { or3 = 1; t10 = bh_chk(G[119], "项们"); }
        t11 = bh_call(t9, 1, (Value[]){t10}, "数组式");
        t12 = bh_add(T[1], t11);
        t13 = bh_add(t12, K[235]);
        t14 = bh_call(T[0], 1, (Value[]){t13}, "出");
        bh_line = 1164;
        if (L[4].tag) t2 = L[4]; else { or4 = 1; t2 = bh_chk(G[195], "临"); }
        BH_RETURN(fr, t2);
    } else {
    }
    bh_line = 1165;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[161])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1166;
        t2 = bh_chk(G[202], "算");
        t3 = bh_index(L[0], bh_int(3LL));
        t4 = bh_call(t2, 1, (Value[]){t3}, "算");
        if (!L[5].tag && or5) bh_modify_outer("左"); L[5] = t4;
        bh_line = 1167;
        t2 = bh_chk(G[184], "可能回收");
        t3 = bh_index(L[0], bh_int(4LL));
        t4 = bh_call(t2, 1, (Value[]){t3}, "可能回收");
        if (bh_truthy(t4)) {
            bh_line = 1168;
            t5 = bh_chk(G[155], "保根");
            if (L[5].tag) t6 = L[5]; else { or5 = 1; t6 = bh_chk(G[107], "左"); }
            t7 = bh_call(t5, 1, (Value[]){t6}, "保根");
            if (!L[5].tag && or5) bh_modify_outer("左"); L[5] = t7;
        } else {
        }
        bh_line = 1169;
        t2 = bh_chk(G[202], "算");
        t3 = bh_index(L[0], bh_int(4LL));
        t4 = bh_call(t2, 1, (Value[]){t3}, "算");
        if (!L[6].tag && or6) bh_modify_outer("右"); L[6] = t4;
        bh_line = 1170;
        t2 = bh_chk(G[157], "定行");
        if (L[2].tag) t3 = L[2]; else { or2 = 1; t3 = bh_chk(G[68], "行"); }
        t4 = bh_call(t2, 1, (Value[]){t3}, "定行");
        bh_line = 1171;
        t2 = bh_chk(G[152], "新临时");
        t3 = bh_call(t2, 0, NULL, "新临时");
        if (!L[4].tag && or4) bh_modify_outer("临"); L[4] = t3;
        bh_line = 1172;
        t2 = bh_chk(G[147], "出");
        T[0] = t2;
        if (L[4].tag) t3 = L[4]; else { or4 = 1; t3 = bh_chk(G[195], "临"); }
        t4 = bh_add(t3, K[182]);
        T[1] = t4;
        t5 = bh_chk(G[201], "运算函数");
        t6 = bh_index(L[0], bh_int(2LL));
        t7 = bh_call(t5, 1, (Value[]){t6}, "运算函数");
        t8 = bh_add(T[1], t7);
        t9 = bh_add(t8, K[101]);
        if (L[5].tag) t10 = L[5]; else { or5 = 1; t10 = bh_chk(G[107], "左"); }
        t11 = bh_add(t9, t10);
        t12 = bh_add(t11, K[229]);
        if (L[6].tag) t13 = L[6]; else { or6 = 1; t13 = bh_chk(G[207], "右"); }
        t14 = bh_add(t12, t13);
        t15 = bh_add(t14, K[235]);
        t16 = bh_call(T[0], 1, (Value[]){t15}, "出");
        bh_line = 1173;
        if (L[4].tag) t2 = L[4]; else { or4 = 1; t2 = bh_chk(G[195], "临"); }
        BH_RETURN(fr, t2);
    } else {
    }
    bh_line = 1174;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[158])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1175;
        t2 = bh_chk(G[208], "算比较");
        t3 = bh_call(t2, 1, (Value[]){L[0]}, "算比较");
        BH_RETURN(fr, t3);
    } else {
    }
    bh_line = 1176;
    {
        t1 = BH_FALSE;
        {
            if (L[1].tag) t2 = L[1]; else { or1 = 1; t2 = bh_chk(G[65], "种类"); }
            if (bh_cmp(0, t2, K[35])) {
                t1 = BH_TRUE;
            }
        }
        if (bh_truthy(t1)) {
            t0 = t1;
        } else {
            t3 = BH_FALSE;
            {
                bh_line = 1176;
                if (L[1].tag) t4 = L[1]; else { or1 = 1; t4 = bh_chk(G[65], "种类"); }
                if (bh_cmp(0, t4, K[36])) {
                    t3 = BH_TRUE;
                }
            }
            t0 = t3;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1177;
        t5 = bh_chk(G[209], "算逻辑");
        t6 = bh_call(t5, 1, (Value[]){L[0]}, "算逻辑");
        BH_RETURN(fr, t6);
    } else {
    }
    bh_line = 1178;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[37])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1179;
        t2 = bh_chk(G[202], "算");
        t3 = bh_index(L[0], bh_int(2LL));
        t4 = bh_call(t2, 1, (Value[]){t3}, "算");
        if (!L[7].tag && or7) bh_modify_outer("值"); L[7] = t4;
        bh_line = 1180;
        t2 = bh_chk(G[152], "新临时");
        t3 = bh_call(t2, 0, NULL, "新临时");
        if (!L[4].tag && or4) bh_modify_outer("临"); L[4] = t3;
        bh_line = 1181;
        t2 = bh_chk(G[147], "出");
        if (L[4].tag) t3 = L[4]; else { or4 = 1; t3 = bh_chk(G[195], "临"); }
        t4 = bh_add(t3, K[237]);
        if (L[7].tag) t5 = L[7]; else { or7 = 1; t5 = bh_chk(G[91], "值"); }
        t6 = bh_add(t4, t5);
        t7 = bh_add(t6, K[238]);
        t8 = bh_call(t2, 1, (Value[]){t7}, "出");
        bh_line = 1182;
        if (L[4].tag) t2 = L[4]; else { or4 = 1; t2 = bh_chk(G[195], "临"); }
        BH_RETURN(fr, t2);
    } else {
    }
    bh_line = 1183;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[163])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1184;
        t2 = bh_chk(G[202], "算");
        t3 = bh_index(L[0], bh_int(2LL));
        t4 = bh_call(t2, 1, (Value[]){t3}, "算");
        if (!L[7].tag && or7) bh_modify_outer("值"); L[7] = t4;
        bh_line = 1185;
        t2 = bh_chk(G[157], "定行");
        if (L[2].tag) t3 = L[2]; else { or2 = 1; t3 = bh_chk(G[68], "行"); }
        t4 = bh_call(t2, 1, (Value[]){t3}, "定行");
        bh_line = 1186;
        t2 = bh_chk(G[152], "新临时");
        t3 = bh_call(t2, 0, NULL, "新临时");
        if (!L[4].tag && or4) bh_modify_outer("临"); L[4] = t3;
        bh_line = 1187;
        t2 = bh_chk(G[147], "出");
        if (L[4].tag) t3 = L[4]; else { or4 = 1; t3 = bh_chk(G[195], "临"); }
        t4 = bh_add(t3, K[239]);
        if (L[7].tag) t5 = L[7]; else { or7 = 1; t5 = bh_chk(G[91], "值"); }
        t6 = bh_add(t4, t5);
        t7 = bh_add(t6, K[235]);
        t8 = bh_call(t2, 1, (Value[]){t7}, "出");
        bh_line = 1188;
        if (L[4].tag) t2 = L[4]; else { or4 = 1; t2 = bh_chk(G[195], "临"); }
        BH_RETURN(fr, t2);
    } else {
    }
    bh_line = 1189;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[141])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1190;
        t2 = bh_chk(G[202], "算");
        t3 = bh_index(L[0], bh_int(2LL));
        t4 = bh_call(t2, 1, (Value[]){t3}, "算");
        if (!L[8].tag && or8) bh_modify_outer("容器"); L[8] = t4;
        bh_line = 1191;
        t2 = bh_chk(G[184], "可能回收");
        t3 = bh_index(L[0], bh_int(3LL));
        t4 = bh_call(t2, 1, (Value[]){t3}, "可能回收");
        if (bh_truthy(t4)) {
            bh_line = 1192;
            t5 = bh_chk(G[155], "保根");
            if (L[8].tag) t6 = L[8]; else { or8 = 1; t6 = bh_chk(G[210], "容器"); }
            t7 = bh_call(t5, 1, (Value[]){t6}, "保根");
            if (!L[8].tag && or8) bh_modify_outer("容器"); L[8] = t7;
        } else {
        }
        bh_line = 1193;
        t2 = bh_chk(G[202], "算");
        t3 = bh_index(L[0], bh_int(3LL));
        t4 = bh_call(t2, 1, (Value[]){t3}, "算");
        if (!L[9].tag && or9) bh_modify_outer("下标"); L[9] = t4;
        bh_line = 1194;
        t2 = bh_chk(G[157], "定行");
        if (L[2].tag) t3 = L[2]; else { or2 = 1; t3 = bh_chk(G[68], "行"); }
        t4 = bh_call(t2, 1, (Value[]){t3}, "定行");
        bh_line = 1195;
        t2 = bh_chk(G[152], "新临时");
        t3 = bh_call(t2, 0, NULL, "新临时");
        if (!L[4].tag && or4) bh_modify_outer("临"); L[4] = t3;
        bh_line = 1196;
        t2 = bh_chk(G[147], "出");
        if (L[4].tag) t3 = L[4]; else { or4 = 1; t3 = bh_chk(G[195], "临"); }
        t4 = bh_add(t3, K[240]);
        if (L[8].tag) t5 = L[8]; else { or8 = 1; t5 = bh_chk(G[210], "容器"); }
        t6 = bh_add(t4, t5);
        t7 = bh_add(t6, K[229]);
        if (L[9].tag) t8 = L[9]; else { or9 = 1; t8 = bh_chk(G[19], "下标"); }
        t9 = bh_add(t7, t8);
        t10 = bh_add(t9, K[235]);
        t11 = bh_call(t2, 1, (Value[]){t10}, "出");
        bh_line = 1197;
        if (L[4].tag) t2 = L[4]; else { or4 = 1; t2 = bh_chk(G[195], "临"); }
        BH_RETURN(fr, t2);
    } else {
    }
    bh_line = 1199;
    t0 = bh_chk(G[181], "直接内置");
    t1 = bh_call(t0, 1, (Value[]){L[0]}, "直接内置");
    if (!L[10].tag && or10) bh_modify_outer("项"); L[10] = t1;
    bh_line = 1200;
    t0 = BH_FALSE;
    {
        if (L[10].tag) t1 = L[10]; else { or10 = 1; t1 = bh_chk(G[75], "项"); }
        if (bh_cmp(1, t1, BH_NIL)) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1201;
        t2 = bh_chk(G[163], "记下");
        t3 = bh_chk(G[130], "直接调用过");
        if (L[10].tag) t4 = L[10]; else { or10 = 1; t4 = bh_chk(G[75], "项"); }
        t5 = bh_index(t4, bh_int(0LL));
        t6 = bh_call(t2, 2, (Value[]){t3, t5}, "记下");
        bh_line = 1202;
        t2 = bh_chk(G[205], "算一串");
        t3 = bh_index(L[0], bh_int(3LL));
        t4 = bh_call(t2, 1, (Value[]){t3}, "算一串");
        if (!L[11].tag && or11) bh_modify_outer("参数们"); L[11] = t4;
        bh_line = 1203;
        t2 = bh_chk(G[157], "定行");
        if (L[2].tag) t3 = L[2]; else { or2 = 1; t3 = bh_chk(G[68], "行"); }
        t4 = bh_call(t2, 1, (Value[]){t3}, "定行");
        bh_line = 1204;
        t2 = bh_chk(G[152], "新临时");
        t3 = bh_call(t2, 0, NULL, "新临时");
        if (!L[4].tag && or4) bh_modify_outer("临"); L[4] = t3;
        bh_line = 1205;
        t2 = bh_chk(G[147], "出");
        T[0] = t2;
        if (L[4].tag) t3 = L[4]; else { or4 = 1; t3 = bh_chk(G[195], "临"); }
        t4 = bh_add(t3, K[182]);
        if (L[10].tag) t5 = L[10]; else { or10 = 1; t5 = bh_chk(G[75], "项"); }
        t6 = bh_index(t5, bh_int(1LL));
        t7 = bh_add(t4, t6);
        t8 = bh_add(t7, K[101]);
        if (L[11].tag) t9 = L[11]; else { or11 = 1; t9 = bh_chk(G[102], "参数们"); }
        t10 = bi_len(1, (Value[]){t9});
        t11 = bh_add(t8, t10);
        t12 = bh_add(t11, K[229]);
        T[1] = t12;
        t13 = bh_chk(G[206], "数组式");
        if (L[11].tag) t14 = L[11]; else { or11 = 1; t14 = bh_chk(G[102], "参数们"); }
        t15 = bh_call(t13, 1, (Value[]){t14}, "数组式");
        t16 = bh_add(T[1], t15);
        t17 = bh_add(t16, K[235]);
        t18 = bh_call(T[0], 1, (Value[]){t17}, "出");
        bh_line = 1206;
        if (L[4].tag) t2 = L[4]; else { or4 = 1; t2 = bh_chk(G[195], "临"); }
        BH_RETURN(fr, t2);
    } else {
    }
    bh_line = 1208;
    t0 = bh_chk(G[202], "算");
    t1 = bh_index(L[0], bh_int(2LL));
    t2 = bh_call(t0, 1, (Value[]){t1}, "算");
    if (!L[12].tag && or12) bh_modify_outer("被调"); L[12] = t2;
    bh_line = 1209;
    t0 = bh_chk(G[182], "一串可能回收");
    t1 = bh_index(L[0], bh_int(3LL));
    t2 = bh_call(t0, 1, (Value[]){t1}, "一串可能回收");
    if (bh_truthy(t2)) {
        bh_line = 1210;
        t3 = bh_chk(G[155], "保根");
        if (L[12].tag) t4 = L[12]; else { or12 = 1; t4 = bh_chk(G[178], "被调"); }
        t5 = bh_call(t3, 1, (Value[]){t4}, "保根");
        if (!L[12].tag && or12) bh_modify_outer("被调"); L[12] = t5;
    } else {
    }
    bh_line = 1211;
    t0 = bh_chk(G[205], "算一串");
    t1 = bh_index(L[0], bh_int(3LL));
    t2 = bh_call(t0, 1, (Value[]){t1}, "算一串");
    if (!L[11].tag && or11) bh_modify_outer("参数们"); L[11] = t2;
    bh_line = 1212;
    t0 = bh_chk(G[157], "定行");
    if (L[2].tag) t1 = L[2]; else { or2 = 1; t1 = bh_chk(G[68], "行"); }
    t2 = bh_call(t0, 1, (Value[]){t1}, "定行");
    bh_line = 1213;
    if (!L[13].tag && or13) bh_modify_outer("名"); L[13] = K[227];
    bh_line = 1214;
    t0 = BH_FALSE;
    {
        t1 = bh_index(L[0], bh_int(2LL));
        t2 = bh_index(t1, bh_int(0LL));
        if (bh_cmp(0, t2, K[128])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1215;
        t3 = bh_chk(G[160], "C文字");
        t4 = bh_index(L[0], bh_int(2LL));
        t5 = bh_index(t4, bh_int(2LL));
        t6 = bh_call(t3, 1, (Value[]){t5}, "C文字");
        if (!L[13].tag && or13) bh_modify_outer("名"); L[13] = t6;
    } else {
    }
    bh_line = 1216;
    t0 = bh_chk(G[152], "新临时");
    t1 = bh_call(t0, 0, NULL, "新临时");
    if (!L[4].tag && or4) bh_modify_outer("临"); L[4] = t1;
    bh_line = 1217;
    t0 = bh_chk(G[147], "出");
    T[0] = t0;
    if (L[4].tag) t1 = L[4]; else { or4 = 1; t1 = bh_chk(G[195], "临"); }
    t2 = bh_add(t1, K[241]);
    if (L[12].tag) t3 = L[12]; else { or12 = 1; t3 = bh_chk(G[178], "被调"); }
    t4 = bh_add(t2, t3);
    t5 = bh_add(t4, K[229]);
    if (L[11].tag) t6 = L[11]; else { or11 = 1; t6 = bh_chk(G[102], "参数们"); }
    t7 = bi_len(1, (Value[]){t6});
    t8 = bh_add(t5, t7);
    t9 = bh_add(t8, K[229]);
    T[1] = t9;
    t10 = bh_chk(G[206], "数组式");
    if (L[11].tag) t11 = L[11]; else { or11 = 1; t11 = bh_chk(G[102], "参数们"); }
    t12 = bh_call(t10, 1, (Value[]){t11}, "数组式");
    t13 = bh_add(T[1], t12);
    t14 = bh_add(t13, K[229]);
    if (L[13].tag) t15 = L[13]; else { or13 = 1; t15 = bh_chk(G[89], "名"); }
    t16 = bh_add(t14, t15);
    t17 = bh_add(t16, K[235]);
    t18 = bh_call(T[0], 1, (Value[]){t17}, "出");
    bh_line = 1218;
    if (L[4].tag) t0 = L[4]; else { or4 = 1; t0 = bh_chk(G[195], "临"); }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn78(Frame *up, Value *a) {    /* 函数 算比较，第 1221 行 */
    struct { BhRoots h; Value v[7]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 7);
    L = fr.v; T = fr.v + 6;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14;
    L[0] = a[0];    /* 节点 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    char or5 = 0;
    BH_SAFEPOINT();
    bh_line = 1223;
    t0 = bh_chk(G[152], "新临时");
    t1 = bh_call(t0, 0, NULL, "新临时");
    if (!L[1].tag && or1) bh_modify_outer("临"); L[1] = t1;
    bh_line = 1224;
    t0 = bh_chk(G[147], "出");
    if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[195], "临"); }
    t2 = bh_add(t1, K[242]);
    t3 = bh_call(t0, 1, (Value[]){t2}, "出");
    bh_line = 1225;
    t0 = bh_chk(G[148], "开块");
    t1 = bh_call(t0, 1, (Value[]){K[243]}, "开块");
    bh_line = 1226;
    t0 = bh_chk(G[202], "算");
    t1 = bh_index(L[0], bh_int(2LL));
    t2 = bh_call(t0, 1, (Value[]){t1}, "算");
    if (!L[2].tag && or2) bh_modify_outer("左"); L[2] = t2;
    bh_line = 1227;
    if (!L[3].tag && or3) bh_modify_outer("层数"); L[3] = bh_int(0LL);
    bh_line = 1228;
    {
        t0 = bh_index(L[0], bh_int(3LL));
        T[0] = bh_iter(t0);
        int64_t n1;
        for (n1 = 0; n1 < bh_list_len(T[0]); n1++) {
            BH_SAFEPOINT();
            bh_line = 1228;
            if (!L[4].tag && or4) bh_modify_outer("对"); L[4] = bh_list_at(T[0], n1);
            bh_line = 1229;
            t1 = bh_chk(G[184], "可能回收");
            if (L[4].tag) t2 = L[4]; else { or4 = 1; t2 = bh_chk(G[185], "对"); }
            t3 = bh_index(t2, bh_int(1LL));
            t4 = bh_call(t1, 1, (Value[]){t3}, "可能回收");
            if (bh_truthy(t4)) {
                bh_line = 1230;
                t5 = bh_chk(G[155], "保根");
                if (L[2].tag) t6 = L[2]; else { or2 = 1; t6 = bh_chk(G[107], "左"); }
                t7 = bh_call(t5, 1, (Value[]){t6}, "保根");
                if (!L[2].tag && or2) bh_modify_outer("左"); L[2] = t7;
            } else {
            }
            bh_line = 1231;
            t1 = bh_chk(G[202], "算");
            if (L[4].tag) t2 = L[4]; else { or4 = 1; t2 = bh_chk(G[185], "对"); }
            t3 = bh_index(t2, bh_int(1LL));
            t4 = bh_call(t1, 1, (Value[]){t3}, "算");
            if (!L[5].tag && or5) bh_modify_outer("右"); L[5] = t4;
            bh_line = 1232;
            t1 = bh_chk(G[157], "定行");
            t2 = bh_index(L[0], bh_int(1LL));
            t3 = bh_call(t1, 1, (Value[]){t2}, "定行");
            bh_line = 1233;
            t1 = bh_chk(G[148], "开块");
            t2 = bh_chk(G[16], "比较符号");
            if (L[4].tag) t3 = L[4]; else { or4 = 1; t3 = bh_chk(G[185], "对"); }
            t4 = bh_index(t3, bh_int(0LL));
            t5 = bi_find(2, (Value[]){t2, t4});
            t6 = bh_add(K[244], t5);
            t7 = bh_add(t6, K[229]);
            if (L[2].tag) t8 = L[2]; else { or2 = 1; t8 = bh_chk(G[107], "左"); }
            t9 = bh_add(t7, t8);
            t10 = bh_add(t9, K[229]);
            if (L[5].tag) t11 = L[5]; else { or5 = 1; t11 = bh_chk(G[207], "右"); }
            t12 = bh_add(t10, t11);
            t13 = bh_add(t12, K[245]);
            t14 = bh_call(t1, 1, (Value[]){t13}, "开块");
            bh_line = 1234;
            if (L[3].tag) t1 = L[3]; else { or3 = 1; t1 = bh_chk(G[211], "层数"); }
            t2 = bh_add(t1, bh_int(1LL));
            if (!L[3].tag && or3) bh_modify_outer("层数"); L[3] = t2;
            bh_line = 1235;
            if (L[5].tag) t1 = L[5]; else { or5 = 1; t1 = bh_chk(G[207], "右"); }
            if (!L[2].tag && or2) bh_modify_outer("左"); L[2] = t1;
        }
    }
    bh_line = 1236;
    t0 = bh_chk(G[147], "出");
    if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[195], "临"); }
    t2 = bh_add(t1, K[246]);
    t3 = bh_call(t0, 1, (Value[]){t2}, "出");
    bh_line = 1237;
    {
        if (L[3].tag) t0 = L[3]; else { or3 = 1; t0 = bh_chk(G[211], "层数"); }
        t1 = bh_add(t0, bh_int(1LL));
        int64_t n2 = bh_count(t1), n3;
        for (n3 = 0; n3 < n2; n3++) {
            BH_SAFEPOINT();
            bh_line = 1238;
            t2 = bh_chk(G[149], "关块");
            t3 = bh_call(t2, 0, NULL, "关块");
        }
    }
    bh_line = 1239;
    if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[195], "临"); }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn79(Frame *up, Value *a) {    /* 函数 算逻辑，第 1242 行 */
    struct { BhRoots h; Value v[4]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 4);
    L = fr.v; T = fr.v + 4;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8;
    L[0] = a[0];    /* 节点 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    BH_SAFEPOINT();
    bh_line = 1244;
    t0 = bh_chk(G[152], "新临时");
    t1 = bh_call(t0, 0, NULL, "新临时");
    if (!L[1].tag && or1) bh_modify_outer("临"); L[1] = t1;
    bh_line = 1245;
    t0 = bh_chk(G[148], "开块");
    t1 = bh_call(t0, 1, (Value[]){K[243]}, "开块");
    bh_line = 1246;
    t0 = bh_chk(G[202], "算");
    t1 = bh_index(L[0], bh_int(2LL));
    t2 = bh_call(t0, 1, (Value[]){t1}, "算");
    if (!L[2].tag && or2) bh_modify_outer("左"); L[2] = t2;
    bh_line = 1247;
    t0 = bh_chk(G[148], "开块");
    if (L[2].tag) t1 = L[2]; else { or2 = 1; t1 = bh_chk(G[107], "左"); }
    t2 = bh_add(K[247], t1);
    t3 = bh_add(t2, K[245]);
    t4 = bh_call(t0, 1, (Value[]){t3}, "开块");
    bh_line = 1248;
    t0 = BH_FALSE;
    {
        t1 = bh_index(L[0], bh_int(0LL));
        if (bh_cmp(0, t1, K[35])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1249;
        t2 = bh_chk(G[202], "算");
        t3 = bh_index(L[0], bh_int(3LL));
        t4 = bh_call(t2, 1, (Value[]){t3}, "算");
        if (!L[3].tag && or3) bh_modify_outer("右"); L[3] = t4;
        bh_line = 1250;
        t2 = bh_chk(G[147], "出");
        if (L[1].tag) t3 = L[1]; else { or1 = 1; t3 = bh_chk(G[195], "临"); }
        t4 = bh_add(t3, K[182]);
        if (L[3].tag) t5 = L[3]; else { or3 = 1; t5 = bh_chk(G[207], "右"); }
        t6 = bh_add(t4, t5);
        t7 = bh_add(t6, K[52]);
        t8 = bh_call(t2, 1, (Value[]){t7}, "出");
        bh_line = 1251;
        t2 = bh_chk(G[150], "否则块");
        t3 = bh_call(t2, 0, NULL, "否则块");
        bh_line = 1252;
        t2 = bh_chk(G[147], "出");
        if (L[1].tag) t3 = L[1]; else { or1 = 1; t3 = bh_chk(G[195], "临"); }
        t4 = bh_add(t3, K[182]);
        if (L[2].tag) t5 = L[2]; else { or2 = 1; t5 = bh_chk(G[107], "左"); }
        t6 = bh_add(t4, t5);
        t7 = bh_add(t6, K[52]);
        t8 = bh_call(t2, 1, (Value[]){t7}, "出");
    } else {
        bh_line = 1254;
        t2 = bh_chk(G[147], "出");
        if (L[1].tag) t3 = L[1]; else { or1 = 1; t3 = bh_chk(G[195], "临"); }
        t4 = bh_add(t3, K[182]);
        if (L[2].tag) t5 = L[2]; else { or2 = 1; t5 = bh_chk(G[107], "左"); }
        t6 = bh_add(t4, t5);
        t7 = bh_add(t6, K[52]);
        t8 = bh_call(t2, 1, (Value[]){t7}, "出");
        bh_line = 1255;
        t2 = bh_chk(G[150], "否则块");
        t3 = bh_call(t2, 0, NULL, "否则块");
        bh_line = 1256;
        t2 = bh_chk(G[202], "算");
        t3 = bh_index(L[0], bh_int(3LL));
        t4 = bh_call(t2, 1, (Value[]){t3}, "算");
        if (!L[3].tag && or3) bh_modify_outer("右"); L[3] = t4;
        bh_line = 1257;
        t2 = bh_chk(G[147], "出");
        if (L[1].tag) t3 = L[1]; else { or1 = 1; t3 = bh_chk(G[195], "临"); }
        t4 = bh_add(t3, K[182]);
        if (L[3].tag) t5 = L[3]; else { or3 = 1; t5 = bh_chk(G[207], "右"); }
        t6 = bh_add(t4, t5);
        t7 = bh_add(t6, K[52]);
        t8 = bh_call(t2, 1, (Value[]){t7}, "出");
    }
    bh_line = 1258;
    t0 = bh_chk(G[149], "关块");
    t1 = bh_call(t0, 0, NULL, "关块");
    bh_line = 1259;
    t0 = bh_chk(G[149], "关块");
    t1 = bh_call(t0, 0, NULL, "关块");
    bh_line = 1260;
    if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[195], "临"); }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn80(Frame *up, Value *a) {    /* 函数 编语句表，第 1265 行 */
    struct { BhRoots h; Value v[3]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 3);
    L = fr.v; T = fr.v + 2;
    Value t0, t1, t2;
    L[0] = a[0];    /* 语句表 */
    char or0 = 0;
    char or1 = 0;
    BH_SAFEPOINT();
    bh_line = 1266;
    {
        T[0] = bh_iter(L[0]);
        int64_t n1;
        for (n1 = 0; n1 < bh_list_len(T[0]); n1++) {
            BH_SAFEPOINT();
            bh_line = 1266;
            if (!L[1].tag && or1) bh_modify_outer("句"); L[1] = bh_list_at(T[0], n1);
            bh_line = 1267;
            t0 = bh_chk(G[212], "编语句");
            if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[164], "句"); }
            t2 = bh_call(t0, 1, (Value[]){t1}, "编语句");
        }
    }
    BH_RETURN(fr, BH_NIL);
}

static Value fn81(Frame *up, Value *a) {    /* 函数 编语句，第 1270 行 */
    struct { BhRoots h; Value v[18]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 18);
    L = fr.v; T = fr.v + 16;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17, t18, t19, t20, t21, t22, t23, t24, t25, t26, t27, t28, t29, t30, t31, t32, t33, t34, t35, t36, t37, t38, t39, t40, t41;
    L[0] = a[0];    /* 句 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    char or5 = 0;
    char or6 = 0;
    char or7 = 0;
    char or8 = 0;
    char or9 = 0;
    char or10 = 0;
    char or11 = 0;
    char or12 = 0;
    char or13 = 0;
    char or14 = 0;
    char or15 = 0;
    BH_SAFEPOINT();
    bh_line = 1271;
    t0 = bh_index(L[0], bh_int(0LL));
    if (!L[1].tag && or1) bh_modify_outer("种类"); L[1] = t0;
    bh_line = 1272;
    t0 = bh_index(L[0], bh_int(1LL));
    if (!L[2].tag && or2) bh_modify_outer("行"); L[2] = t0;
    bh_line = 1273;
    t0 = bh_chk(G[157], "定行");
    if (L[2].tag) t1 = L[2]; else { or2 = 1; t1 = bh_chk(G[68], "行"); }
    t2 = bh_call(t0, 1, (Value[]){t1}, "定行");
    bh_line = 1274;
    t0 = bh_chk(G[126], "环境");
    t1 = bh_chk(G[136], "临时位");
    t2 = bh_index(t0, t1);
    if (!L[3].tag && or3) bh_modify_outer("用到这里"); L[3] = t2;
    bh_line = 1275;
    t0 = bh_chk(G[126], "环境");
    t1 = bh_chk(G[143], "根位");
    t2 = bh_index(t0, t1);
    if (!L[4].tag && or4) bh_modify_outer("根用到这里"); L[4] = t2;
    bh_line = 1276;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[65], "种类"); }
        if (bh_cmp(0, t1, K[127])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1277;
        t2 = bh_chk(G[202], "算");
        t3 = bh_index(L[0], bh_int(2LL));
        t4 = bh_call(t2, 1, (Value[]){t3}, "算");
    } else {
        t2 = BH_FALSE;
        {
            bh_line = 1278;
            if (L[1].tag) t3 = L[1]; else { or1 = 1; t3 = bh_chk(G[65], "种类"); }
            if (bh_cmp(0, t3, K[143])) {
                t2 = BH_TRUE;
            }
        }
        if (bh_truthy(t2)) {
            bh_line = 1279;
            t4 = bh_chk(G[214], "编赋值");
            t5 = bh_call(t4, 1, (Value[]){L[0]}, "编赋值");
        } else {
            t4 = BH_FALSE;
            {
                bh_line = 1280;
                if (L[1].tag) t5 = L[1]; else { or1 = 1; t5 = bh_chk(G[65], "种类"); }
                if (bh_cmp(0, t5, K[146])) {
                    t4 = BH_TRUE;
                }
            }
            if (bh_truthy(t4)) {
                bh_line = 1281;
                t6 = bh_chk(G[215], "编如果");
                t7 = bh_call(t6, 1, (Value[]){L[0]}, "编如果");
            } else {
                t6 = BH_FALSE;
                {
                    bh_line = 1282;
                    if (L[1].tag) t7 = L[1]; else { or1 = 1; t7 = bh_chk(G[65], "种类"); }
                    if (bh_cmp(0, t7, K[147])) {
                        t6 = BH_TRUE;
                    }
                }
                if (bh_truthy(t6)) {
                    bh_line = 1283;
                    t8 = bh_chk(G[148], "开块");
                    t9 = bh_call(t8, 1, (Value[]){K[248]}, "开块");
                    bh_line = 1284;
                    t8 = bh_chk(G[147], "出");
                    t9 = bh_call(t8, 1, (Value[]){K[249]}, "出");
                    bh_line = 1285;
                    t8 = bh_neg(bh_int(1LL));
                    t9 = bh_chk(G[126], "环境");
                    t10 = bh_chk(G[137], "行位");
                    bh_set_index(t9, t10, t8);
                    bh_line = 1286;
                    t8 = bh_chk(G[202], "算");
                    t9 = bh_index(L[0], bh_int(2LL));
                    t10 = bh_call(t8, 1, (Value[]){t9}, "算");
                    if (!L[5].tag && or5) bh_modify_outer("条件"); L[5] = t10;
                    bh_line = 1287;
                    t8 = bh_chk(G[147], "出");
                    if (L[5].tag) t9 = L[5]; else { or5 = 1; t9 = bh_chk(G[97], "条件"); }
                    t10 = bh_add(K[250], t9);
                    t11 = bh_add(t10, K[251]);
                    t12 = bh_call(t8, 1, (Value[]){t11}, "出");
                    bh_line = 1288;
                    t8 = bh_chk(G[213], "编语句表");
                    t9 = bh_index(L[0], bh_int(3LL));
                    t10 = bh_call(t8, 1, (Value[]){t9}, "编语句表");
                    bh_line = 1289;
                    t8 = bh_chk(G[149], "关块");
                    t9 = bh_call(t8, 0, NULL, "关块");
                } else {
                    {
                        t9 = BH_FALSE;
                        {
                            bh_line = 1290;
                            if (L[1].tag) t10 = L[1]; else { or1 = 1; t10 = bh_chk(G[65], "种类"); }
                            if (bh_cmp(0, t10, K[149])) {
                                t9 = BH_TRUE;
                            }
                        }
                        if (bh_truthy(t9)) {
                            bh_line = 1290;
                            t11 = bh_chk(G[216], "是范围调用");
                            t12 = bh_index(L[0], bh_int(2LL));
                            t13 = bh_call(t11, 1, (Value[]){t12}, "是范围调用");
                            t8 = t13;
                        } else {
                            t8 = t9;
                        }
                    }
                    if (bh_truthy(t8)) {
                        bh_line = 1292;
                        t14 = bh_chk(G[148], "开块");
                        t15 = bh_call(t14, 1, (Value[]){K[243]}, "开块");
                        bh_line = 1293;
                        t14 = bh_chk(G[205], "算一串");
                        t15 = bh_index(L[0], bh_int(2LL));
                        t16 = bh_index(t15, bh_int(3LL));
                        t17 = bh_call(t14, 1, (Value[]){t16}, "算一串");
                        if (!L[6].tag && or6) bh_modify_outer("参数们"); L[6] = t17;
                        bh_line = 1294;
                        t14 = bh_chk(G[157], "定行");
                        t15 = bh_index(L[0], bh_int(2LL));
                        t16 = bh_index(t15, bh_int(1LL));
                        t17 = bh_call(t14, 1, (Value[]){t16}, "定行");
                        bh_line = 1295;
                        t14 = bh_chk(G[153], "根临时");
                        t15 = bh_call(t14, 0, NULL, "根临时");
                        if (!L[7].tag && or7) bh_modify_outer("份"); L[7] = t15;
                        bh_line = 1296;
                        t14 = bh_chk(G[156], "新整型");
                        t15 = bh_call(t14, 0, NULL, "新整型");
                        if (!L[8].tag && or8) bh_modify_outer("号"); L[8] = t15;
                        bh_line = 1297;
                        if (L[8].tag) t14 = L[8]; else { or8 = 1; t14 = bh_chk(G[45], "号"); }
                        t15 = bh_add(K[252], t14);
                        if (!L[9].tag && or9) bh_modify_outer("区"); L[9] = t15;
                        bh_line = 1298;
                        t14 = bh_chk(G[147], "出");
                        if (L[9].tag) t15 = L[9]; else { or9 = 1; t15 = bh_chk(G[217], "区"); }
                        t16 = bh_add(K[253], t15);
                        t17 = bh_add(t16, K[52]);
                        t18 = bh_call(t14, 1, (Value[]){t17}, "出");
                        bh_line = 1299;
                        t14 = bh_chk(G[147], "出");
                        if (L[8].tag) t15 = L[8]; else { or8 = 1; t15 = bh_chk(G[45], "号"); }
                        t16 = bh_add(K[254], t15);
                        t17 = bh_add(t16, K[52]);
                        t18 = bh_call(t14, 1, (Value[]){t17}, "出");
                        bh_line = 1300;
                        t14 = bh_chk(G[147], "出");
                        T[0] = t14;
                        if (L[7].tag) t15 = L[7]; else { or7 = 1; t15 = bh_chk(G[218], "份"); }
                        t16 = bh_add(t15, K[255]);
                        if (L[9].tag) t17 = L[9]; else { or9 = 1; t17 = bh_chk(G[217], "区"); }
                        t18 = bh_add(t16, t17);
                        t19 = bh_add(t18, K[229]);
                        if (L[6].tag) t20 = L[6]; else { or6 = 1; t20 = bh_chk(G[102], "参数们"); }
                        t21 = bi_len(1, (Value[]){t20});
                        t22 = bh_add(t19, t21);
                        t23 = bh_add(t22, K[229]);
                        T[1] = t23;
                        t24 = bh_chk(G[206], "数组式");
                        if (L[6].tag) t25 = L[6]; else { or6 = 1; t25 = bh_chk(G[102], "参数们"); }
                        t26 = bh_call(t24, 1, (Value[]){t25}, "数组式");
                        t27 = bh_add(T[1], t26);
                        t28 = bh_add(t27, K[235]);
                        t29 = bh_call(T[0], 1, (Value[]){t28}, "出");
                        bh_line = 1301;
                        t14 = bh_chk(G[148], "开块");
                        if (L[8].tag) t15 = L[8]; else { or8 = 1; t15 = bh_chk(G[45], "号"); }
                        t16 = bh_add(K[256], t15);
                        t17 = bh_add(t16, K[257]);
                        if (L[8].tag) t18 = L[8]; else { or8 = 1; t18 = bh_chk(G[45], "号"); }
                        t19 = bh_add(t17, t18);
                        t20 = bh_add(t19, K[258]);
                        if (L[9].tag) t21 = L[9]; else { or9 = 1; t21 = bh_chk(G[217], "区"); }
                        t22 = bh_add(t20, t21);
                        t23 = bh_add(t22, K[259]);
                        if (L[8].tag) t24 = L[8]; else { or8 = 1; t24 = bh_chk(G[45], "号"); }
                        t25 = bh_add(t23, t24);
                        t26 = bh_add(t25, K[260]);
                        t27 = bh_call(t14, 1, (Value[]){t26}, "开块");
                        bh_line = 1302;
                        t14 = bh_chk(G[147], "出");
                        t15 = bh_call(t14, 1, (Value[]){K[249]}, "出");
                        bh_line = 1303;
                        t14 = bh_neg(bh_int(1LL));
                        t15 = bh_chk(G[126], "环境");
                        t16 = bh_chk(G[137], "行位");
                        bh_set_index(t15, t16, t14);
                        bh_line = 1304;
                        t14 = bh_chk(G[157], "定行");
                        if (L[2].tag) t15 = L[2]; else { or2 = 1; t15 = bh_chk(G[68], "行"); }
                        t16 = bh_call(t14, 1, (Value[]){t15}, "定行");
                        bh_line = 1305;
                        t14 = bh_chk(G[200], "写变量");
                        t15 = bh_index(L[0], bh_int(3LL));
                        if (L[9].tag) t16 = L[9]; else { or9 = 1; t16 = bh_chk(G[217], "区"); }
                        t17 = bh_add(K[261], t16);
                        t18 = bh_add(t17, K[229]);
                        if (L[7].tag) t19 = L[7]; else { or7 = 1; t19 = bh_chk(G[218], "份"); }
                        t20 = bh_add(t18, t19);
                        t21 = bh_add(t20, K[229]);
                        if (L[8].tag) t22 = L[8]; else { or8 = 1; t22 = bh_chk(G[45], "号"); }
                        t23 = bh_add(t21, t22);
                        t24 = bh_add(t23, K[102]);
                        t25 = bh_call(t14, 2, (Value[]){t15, t24}, "写变量");
                        bh_line = 1306;
                        t14 = bh_chk(G[213], "编语句表");
                        t15 = bh_index(L[0], bh_int(4LL));
                        t16 = bh_call(t14, 1, (Value[]){t15}, "编语句表");
                        bh_line = 1307;
                        t14 = bh_chk(G[149], "关块");
                        t15 = bh_call(t14, 0, NULL, "关块");
                        bh_line = 1308;
                        t14 = bh_chk(G[149], "关块");
                        t15 = bh_call(t14, 0, NULL, "关块");
                    } else {
                        t14 = BH_FALSE;
                        {
                            bh_line = 1309;
                            if (L[1].tag) t15 = L[1]; else { or1 = 1; t15 = bh_chk(G[65], "种类"); }
                            if (bh_cmp(0, t15, K[149])) {
                                t14 = BH_TRUE;
                            }
                        }
                        if (bh_truthy(t14)) {
                            bh_line = 1311;
                            t16 = bh_chk(G[148], "开块");
                            t17 = bh_call(t16, 1, (Value[]){K[243]}, "开块");
                            bh_line = 1312;
                            t16 = bh_chk(G[202], "算");
                            t17 = bh_index(L[0], bh_int(2LL));
                            t18 = bh_call(t16, 1, (Value[]){t17}, "算");
                            if (!L[10].tag && or10) bh_modify_outer("来源"); L[10] = t18;
                            bh_line = 1313;
                            t16 = bh_chk(G[157], "定行");
                            if (L[2].tag) t17 = L[2]; else { or2 = 1; t17 = bh_chk(G[68], "行"); }
                            t18 = bh_call(t16, 1, (Value[]){t17}, "定行");
                            bh_line = 1314;
                            t16 = bh_chk(G[153], "根临时");
                            t17 = bh_call(t16, 0, NULL, "根临时");
                            if (!L[7].tag && or7) bh_modify_outer("份"); L[7] = t17;
                            bh_line = 1315;
                            t16 = bh_chk(G[156], "新整型");
                            t17 = bh_call(t16, 0, NULL, "新整型");
                            if (!L[8].tag && or8) bh_modify_outer("号"); L[8] = t17;
                            bh_line = 1316;
                            t16 = bh_chk(G[147], "出");
                            if (L[7].tag) t17 = L[7]; else { or7 = 1; t17 = bh_chk(G[218], "份"); }
                            t18 = bh_add(t17, K[262]);
                            if (L[10].tag) t19 = L[10]; else { or10 = 1; t19 = bh_chk(G[100], "来源"); }
                            t20 = bh_add(t18, t19);
                            t21 = bh_add(t20, K[235]);
                            t22 = bh_call(t16, 1, (Value[]){t21}, "出");
                            bh_line = 1317;
                            t16 = bh_chk(G[147], "出");
                            if (L[8].tag) t17 = L[8]; else { or8 = 1; t17 = bh_chk(G[45], "号"); }
                            t18 = bh_add(K[254], t17);
                            t19 = bh_add(t18, K[52]);
                            t20 = bh_call(t16, 1, (Value[]){t19}, "出");
                            bh_line = 1318;
                            t16 = bh_chk(G[148], "开块");
                            if (L[8].tag) t17 = L[8]; else { or8 = 1; t17 = bh_chk(G[45], "号"); }
                            t18 = bh_add(K[256], t17);
                            t19 = bh_add(t18, K[257]);
                            if (L[8].tag) t20 = L[8]; else { or8 = 1; t20 = bh_chk(G[45], "号"); }
                            t21 = bh_add(t19, t20);
                            t22 = bh_add(t21, K[263]);
                            if (L[7].tag) t23 = L[7]; else { or7 = 1; t23 = bh_chk(G[218], "份"); }
                            t24 = bh_add(t22, t23);
                            t25 = bh_add(t24, K[264]);
                            if (L[8].tag) t26 = L[8]; else { or8 = 1; t26 = bh_chk(G[45], "号"); }
                            t27 = bh_add(t25, t26);
                            t28 = bh_add(t27, K[260]);
                            t29 = bh_call(t16, 1, (Value[]){t28}, "开块");
                            bh_line = 1319;
                            t16 = bh_chk(G[147], "出");
                            t17 = bh_call(t16, 1, (Value[]){K[249]}, "出");
                            bh_line = 1320;
                            t16 = bh_neg(bh_int(1LL));
                            t17 = bh_chk(G[126], "环境");
                            t18 = bh_chk(G[137], "行位");
                            bh_set_index(t17, t18, t16);
                            bh_line = 1321;
                            t16 = bh_chk(G[157], "定行");
                            if (L[2].tag) t17 = L[2]; else { or2 = 1; t17 = bh_chk(G[68], "行"); }
                            t18 = bh_call(t16, 1, (Value[]){t17}, "定行");
                            bh_line = 1322;
                            t16 = bh_chk(G[200], "写变量");
                            t17 = bh_index(L[0], bh_int(3LL));
                            if (L[7].tag) t18 = L[7]; else { or7 = 1; t18 = bh_chk(G[218], "份"); }
                            t19 = bh_add(K[265], t18);
                            t20 = bh_add(t19, K[229]);
                            if (L[8].tag) t21 = L[8]; else { or8 = 1; t21 = bh_chk(G[45], "号"); }
                            t22 = bh_add(t20, t21);
                            t23 = bh_add(t22, K[102]);
                            t24 = bh_call(t16, 2, (Value[]){t17, t23}, "写变量");
                            bh_line = 1323;
                            t16 = bh_chk(G[213], "编语句表");
                            t17 = bh_index(L[0], bh_int(4LL));
                            t18 = bh_call(t16, 1, (Value[]){t17}, "编语句表");
                            bh_line = 1324;
                            t16 = bh_chk(G[149], "关块");
                            t17 = bh_call(t16, 0, NULL, "关块");
                            bh_line = 1325;
                            t16 = bh_chk(G[149], "关块");
                            t17 = bh_call(t16, 0, NULL, "关块");
                        } else {
                            t16 = BH_FALSE;
                            {
                                bh_line = 1326;
                                if (L[1].tag) t17 = L[1]; else { or1 = 1; t17 = bh_chk(G[65], "种类"); }
                                if (bh_cmp(0, t17, K[151])) {
                                    t16 = BH_TRUE;
                                }
                            }
                            if (bh_truthy(t16)) {
                                bh_line = 1327;
                                t18 = bh_chk(G[148], "开块");
                                t19 = bh_call(t18, 1, (Value[]){K[243]}, "开块");
                                bh_line = 1328;
                                t18 = bh_chk(G[202], "算");
                                t19 = bh_index(L[0], bh_int(2LL));
                                t20 = bh_call(t18, 1, (Value[]){t19}, "算");
                                if (!L[11].tag && or11) bh_modify_outer("次数"); L[11] = t20;
                                bh_line = 1329;
                                t18 = bh_chk(G[157], "定行");
                                if (L[2].tag) t19 = L[2]; else { or2 = 1; t19 = bh_chk(G[68], "行"); }
                                t20 = bh_call(t18, 1, (Value[]){t19}, "定行");
                                bh_line = 1330;
                                t18 = bh_chk(G[156], "新整型");
                                t19 = bh_call(t18, 0, NULL, "新整型");
                                if (!L[12].tag && or12) bh_modify_outer("总"); L[12] = t19;
                                bh_line = 1331;
                                t18 = bh_chk(G[156], "新整型");
                                t19 = bh_call(t18, 0, NULL, "新整型");
                                if (!L[8].tag && or8) bh_modify_outer("号"); L[8] = t19;
                                bh_line = 1332;
                                t18 = bh_chk(G[147], "出");
                                if (L[12].tag) t19 = L[12]; else { or12 = 1; t19 = bh_chk(G[219], "总"); }
                                t20 = bh_add(K[254], t19);
                                t21 = bh_add(t20, K[266]);
                                if (L[11].tag) t22 = L[11]; else { or11 = 1; t22 = bh_chk(G[101], "次数"); }
                                t23 = bh_add(t21, t22);
                                t24 = bh_add(t23, K[267]);
                                if (L[8].tag) t25 = L[8]; else { or8 = 1; t25 = bh_chk(G[45], "号"); }
                                t26 = bh_add(t24, t25);
                                t27 = bh_add(t26, K[52]);
                                t28 = bh_call(t18, 1, (Value[]){t27}, "出");
                                bh_line = 1333;
                                t18 = bh_chk(G[148], "开块");
                                if (L[8].tag) t19 = L[8]; else { or8 = 1; t19 = bh_chk(G[45], "号"); }
                                t20 = bh_add(K[256], t19);
                                t21 = bh_add(t20, K[257]);
                                if (L[8].tag) t22 = L[8]; else { or8 = 1; t22 = bh_chk(G[45], "号"); }
                                t23 = bh_add(t21, t22);
                                t24 = bh_add(t23, K[258]);
                                if (L[12].tag) t25 = L[12]; else { or12 = 1; t25 = bh_chk(G[219], "总"); }
                                t26 = bh_add(t24, t25);
                                t27 = bh_add(t26, K[268]);
                                if (L[8].tag) t28 = L[8]; else { or8 = 1; t28 = bh_chk(G[45], "号"); }
                                t29 = bh_add(t27, t28);
                                t30 = bh_add(t29, K[260]);
                                t31 = bh_call(t18, 1, (Value[]){t30}, "开块");
                                bh_line = 1334;
                                t18 = bh_chk(G[147], "出");
                                t19 = bh_call(t18, 1, (Value[]){K[249]}, "出");
                                bh_line = 1335;
                                t18 = bh_neg(bh_int(1LL));
                                t19 = bh_chk(G[126], "环境");
                                t20 = bh_chk(G[137], "行位");
                                bh_set_index(t19, t20, t18);
                                bh_line = 1336;
                                t18 = bh_chk(G[213], "编语句表");
                                t19 = bh_index(L[0], bh_int(3LL));
                                t20 = bh_call(t18, 1, (Value[]){t19}, "编语句表");
                                bh_line = 1337;
                                t18 = bh_chk(G[149], "关块");
                                t19 = bh_call(t18, 0, NULL, "关块");
                                bh_line = 1338;
                                t18 = bh_chk(G[149], "关块");
                                t19 = bh_call(t18, 0, NULL, "关块");
                            } else {
                                t18 = BH_FALSE;
                                {
                                    bh_line = 1339;
                                    if (L[1].tag) t19 = L[1]; else { or1 = 1; t19 = bh_chk(G[65], "种类"); }
                                    if (bh_cmp(0, t19, K[156])) {
                                        t18 = BH_TRUE;
                                    }
                                }
                                if (bh_truthy(t18)) {
                                    bh_line = 1340;
                                    t20 = bh_chk(G[220], "编函数");
                                    t21 = bh_call(t20, 1, (Value[]){L[0]}, "编函数");
                                    if (!L[13].tag && or13) bh_modify_outer("式"); L[13] = t21;
                                    bh_line = 1341;
                                    t20 = bh_chk(G[157], "定行");
                                    if (L[2].tag) t21 = L[2]; else { or2 = 1; t21 = bh_chk(G[68], "行"); }
                                    t22 = bh_call(t20, 1, (Value[]){t21}, "定行");
                                    bh_line = 1342;
                                    t20 = bh_chk(G[200], "写变量");
                                    t21 = bh_index(L[0], bh_int(2LL));
                                    if (L[13].tag) t22 = L[13]; else { or13 = 1; t22 = bh_chk(G[93], "式"); }
                                    t23 = bh_call(t20, 2, (Value[]){t21, t22}, "写变量");
                                } else {
                                    t20 = BH_FALSE;
                                    {
                                        bh_line = 1343;
                                        if (L[1].tag) t21 = L[1]; else { or1 = 1; t21 = bh_chk(G[65], "种类"); }
                                        if (bh_cmp(0, t21, K[135])) {
                                            t20 = BH_TRUE;
                                        }
                                    }
                                    if (bh_truthy(t20)) {
                                        bh_line = 1344;
                                        if (!L[14].tag && or14) bh_modify_outer("值"); L[14] = K[233];
                                        bh_line = 1345;
                                        t22 = BH_FALSE;
                                        {
                                            t23 = bh_index(L[0], bh_int(2LL));
                                            if (bh_cmp(1, t23, BH_NIL)) {
                                                t22 = BH_TRUE;
                                            }
                                        }
                                        if (bh_truthy(t22)) {
                                            bh_line = 1346;
                                            t24 = bh_chk(G[202], "算");
                                            t25 = bh_index(L[0], bh_int(2LL));
                                            t26 = bh_call(t24, 1, (Value[]){t25}, "算");
                                            if (!L[14].tag && or14) bh_modify_outer("值"); L[14] = t26;
                                        } else {
                                        }
                                        bh_line = 1347;
                                        t22 = bh_chk(G[126], "环境");
                                        t23 = bh_chk(G[145], "叶子位");
                                        t24 = bh_index(t22, t23);
                                        if (bh_truthy(t24)) {
                                            bh_line = 1348;
                                            t25 = bh_chk(G[147], "出");
                                            if (L[14].tag) t26 = L[14]; else { or14 = 1; t26 = bh_chk(G[91], "值"); }
                                            t27 = bh_add(K[269], t26);
                                            t28 = bh_add(t27, K[52]);
                                            t29 = bh_call(t25, 1, (Value[]){t28}, "出");
                                        } else {
                                            bh_line = 1350;
                                            t25 = bh_chk(G[147], "出");
                                            if (L[14].tag) t26 = L[14]; else { or14 = 1; t26 = bh_chk(G[91], "值"); }
                                            t27 = bh_add(K[270], t26);
                                            t28 = bh_add(t27, K[235]);
                                            t29 = bh_call(t25, 1, (Value[]){t28}, "出");
                                        }
                                    } else {
                                        t22 = BH_FALSE;
                                        {
                                            bh_line = 1351;
                                            if (L[1].tag) t23 = L[1]; else { or1 = 1; t23 = bh_chk(G[65], "种类"); }
                                            if (bh_cmp(0, t23, K[137])) {
                                                t22 = BH_TRUE;
                                            }
                                        }
                                        if (bh_truthy(t22)) {
                                            bh_line = 1352;
                                            t24 = bh_chk(G[147], "出");
                                            t25 = bh_call(t24, 1, (Value[]){K[271]}, "出");
                                        } else {
                                            t24 = BH_FALSE;
                                            {
                                                bh_line = 1353;
                                                if (L[1].tag) t25 = L[1]; else { or1 = 1; t25 = bh_chk(G[65], "种类"); }
                                                if (bh_cmp(0, t25, K[138])) {
                                                    t24 = BH_TRUE;
                                                }
                                            }
                                            if (bh_truthy(t24)) {
                                                bh_line = 1354;
                                                t26 = bh_chk(G[147], "出");
                                                t27 = bh_call(t26, 1, (Value[]){K[272]}, "出");
                                            } else {
                                                t26 = BH_FALSE;
                                                {
                                                    bh_line = 1355;
                                                    if (L[1].tag) t27 = L[1]; else { or1 = 1; t27 = bh_chk(G[65], "种类"); }
                                                    if (bh_cmp(0, t27, K[140])) {
                                                        t26 = BH_TRUE;
                                                    }
                                                }
                                                if (bh_truthy(t26)) {
                                                    bh_line = 1356;
                                                    t28 = bh_chk(G[126], "环境");
                                                    t29 = bh_chk(G[139], "模块位");
                                                    t30 = bh_index(t28, t29);
                                                    t31 = bh_bool(!bh_truthy(t30));
                                                    if (bh_truthy(t31)) {
                                                        bh_line = 1357;
                                                        {
                                                            t32 = bh_index(L[0], bh_int(2LL));
                                                            T[0] = bh_iter(t32);
                                                            int64_t n1;
                                                            for (n1 = 0; n1 < bh_list_len(T[0]); n1++) {
                                                                BH_SAFEPOINT();
                                                                bh_line = 1357;
                                                                if (!L[15].tag && or15) bh_modify_outer("名"); L[15] = bh_list_at(T[0], n1);
                                                                bh_line = 1358;
                                                                t33 = bh_chk(G[147], "出");
                                                                t34 = bh_chk(G[126], "环境");
                                                                t35 = bh_chk(G[134], "声明位");
                                                                t36 = bh_index(t34, t35);
                                                                if (L[15].tag) t37 = L[15]; else { or15 = 1; t37 = bh_chk(G[89], "名"); }
                                                                t38 = bi_find(2, (Value[]){t36, t37});
                                                                t39 = bh_add(K[273], t38);
                                                                t40 = bh_add(t39, K[274]);
                                                                t41 = bh_call(t33, 1, (Value[]){t40}, "出");
                                                            }
                                                        }
                                                    } else {
                                                    }
                                                } else {
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    bh_line = 1359;
    if (L[3].tag) t0 = L[3]; else { or3 = 1; t0 = bh_chk(G[221], "用到这里"); }
    t1 = bh_chk(G[126], "环境");
    t2 = bh_chk(G[136], "临时位");
    bh_set_index(t1, t2, t0);
    bh_line = 1360;
    if (L[4].tag) t0 = L[4]; else { or4 = 1; t0 = bh_chk(G[222], "根用到这里"); }
    t1 = bh_chk(G[126], "环境");
    t2 = bh_chk(G[143], "根位");
    bh_set_index(t1, t2, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn82(Frame *up, Value *a) {    /* 函数 是范围调用，第 1363 行 */
    struct { BhRoots h; Value v[2]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 2);
    L = fr.v; T = fr.v + 2;
    Value t0, t1, t2, t3, t4, t5;
    L[0] = a[0];    /* 式 */
    char or0 = 0;
    char or1 = 0;
    BH_SAFEPOINT();
    bh_line = 1365;
    t0 = BH_FALSE;
    {
        t1 = bh_index(L[0], bh_int(0LL));
        if (bh_cmp(1, t1, K[164])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1366;
        BH_RETURN(fr, BH_FALSE);
    } else {
    }
    bh_line = 1367;
    t0 = bh_chk(G[181], "直接内置");
    t1 = bh_call(t0, 1, (Value[]){L[0]}, "直接内置");
    if (!L[1].tag && or1) bh_modify_outer("项"); L[1] = t1;
    bh_line = 1368;
    {
        t1 = BH_FALSE;
        {
            if (L[1].tag) t2 = L[1]; else { or1 = 1; t2 = bh_chk(G[75], "项"); }
            if (bh_cmp(1, t2, BH_NIL)) {
                t1 = BH_TRUE;
            }
        }
        if (bh_truthy(t1)) {
            t3 = BH_FALSE;
            {
                bh_line = 1368;
                if (L[1].tag) t4 = L[1]; else { or1 = 1; t4 = bh_chk(G[75], "项"); }
                t5 = bh_index(t4, bh_int(0LL));
                if (bh_cmp(0, t5, K[275])) {
                    t3 = BH_TRUE;
                }
            }
            t0 = t3;
        } else {
            t0 = t1;
        }
    }
    BH_RETURN(fr, t0);
    BH_RETURN(fr, BH_NIL);
}

static Value fn83(Frame *up, Value *a) {    /* 函数 编赋值，第 1371 行 */
    struct { BhRoots h; Value v[11]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 11);
    L = fr.v; T = fr.v + 9;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17;
    L[0] = a[0];    /* 句 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    char or5 = 0;
    char or6 = 0;
    char or7 = 0;
    char or8 = 0;
    BH_SAFEPOINT();
    bh_line = 1372;
    t0 = bh_index(L[0], bh_int(1LL));
    if (!L[1].tag && or1) bh_modify_outer("行"); L[1] = t0;
    bh_line = 1373;
    t0 = bh_index(L[0], bh_int(2LL));
    if (!L[2].tag && or2) bh_modify_outer("目标"); L[2] = t0;
    bh_line = 1374;
    t0 = bh_index(L[0], bh_int(3LL));
    if (!L[3].tag && or3) bh_modify_outer("运算"); L[3] = t0;
    bh_line = 1375;
    t0 = bh_chk(G[202], "算");
    t1 = bh_index(L[0], bh_int(4LL));
    t2 = bh_call(t0, 1, (Value[]){t1}, "算");
    if (!L[4].tag && or4) bh_modify_outer("值"); L[4] = t2;
    bh_line = 1376;
    t0 = BH_FALSE;
    {
        if (L[3].tag) t1 = L[3]; else { or3 = 1; t1 = bh_chk(G[223], "运算"); }
        if (bh_cmp(1, t1, K[74])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1377;
        t2 = bh_chk(G[184], "可能回收");
        if (L[2].tag) t3 = L[2]; else { or2 = 1; t3 = bh_chk(G[224], "目标"); }
        t4 = bh_call(t2, 1, (Value[]){t3}, "可能回收");
        if (bh_truthy(t4)) {
            bh_line = 1378;
            t5 = bh_chk(G[155], "保根");
            if (L[4].tag) t6 = L[4]; else { or4 = 1; t6 = bh_chk(G[91], "值"); }
            t7 = bh_call(t5, 1, (Value[]){t6}, "保根");
            if (!L[4].tag && or4) bh_modify_outer("值"); L[4] = t7;
        } else {
        }
        bh_line = 1379;
        t2 = bh_chk(G[202], "算");
        if (L[2].tag) t3 = L[2]; else { or2 = 1; t3 = bh_chk(G[224], "目标"); }
        t4 = bh_call(t2, 1, (Value[]){t3}, "算");
        if (!L[5].tag && or5) bh_modify_outer("旧"); L[5] = t4;
        bh_line = 1380;
        t2 = bh_chk(G[157], "定行");
        if (L[1].tag) t3 = L[1]; else { or1 = 1; t3 = bh_chk(G[68], "行"); }
        t4 = bh_call(t2, 1, (Value[]){t3}, "定行");
        bh_line = 1381;
        t2 = bh_chk(G[152], "新临时");
        t3 = bh_call(t2, 0, NULL, "新临时");
        if (!L[6].tag && or6) bh_modify_outer("临"); L[6] = t3;
        bh_line = 1382;
        t2 = bh_chk(G[147], "出");
        T[0] = t2;
        if (L[6].tag) t3 = L[6]; else { or6 = 1; t3 = bh_chk(G[195], "临"); }
        t4 = bh_add(t3, K[182]);
        T[1] = t4;
        t5 = bh_chk(G[201], "运算函数");
        if (L[3].tag) t6 = L[3]; else { or3 = 1; t6 = bh_chk(G[223], "运算"); }
        t7 = bi_slice(3, (Value[]){t6, bh_int(0LL), bh_int(1LL)});
        t8 = bh_call(t5, 1, (Value[]){t7}, "运算函数");
        t9 = bh_add(T[1], t8);
        t10 = bh_add(t9, K[101]);
        if (L[5].tag) t11 = L[5]; else { or5 = 1; t11 = bh_chk(G[225], "旧"); }
        t12 = bh_add(t10, t11);
        t13 = bh_add(t12, K[229]);
        if (L[4].tag) t14 = L[4]; else { or4 = 1; t14 = bh_chk(G[91], "值"); }
        t15 = bh_add(t13, t14);
        t16 = bh_add(t15, K[235]);
        t17 = bh_call(T[0], 1, (Value[]){t16}, "出");
        bh_line = 1383;
        if (L[6].tag) t2 = L[6]; else { or6 = 1; t2 = bh_chk(G[195], "临"); }
        if (!L[4].tag && or4) bh_modify_outer("值"); L[4] = t2;
    } else {
    }
    bh_line = 1384;
    t0 = BH_FALSE;
    {
        if (L[2].tag) t1 = L[2]; else { or2 = 1; t1 = bh_chk(G[224], "目标"); }
        t2 = bh_index(t1, bh_int(0LL));
        if (bh_cmp(0, t2, K[128])) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1385;
        t3 = bh_chk(G[157], "定行");
        if (L[1].tag) t4 = L[1]; else { or1 = 1; t4 = bh_chk(G[68], "行"); }
        t5 = bh_call(t3, 1, (Value[]){t4}, "定行");
        bh_line = 1386;
        t3 = bh_chk(G[200], "写变量");
        if (L[2].tag) t4 = L[2]; else { or2 = 1; t4 = bh_chk(G[224], "目标"); }
        t5 = bh_index(t4, bh_int(2LL));
        if (L[4].tag) t6 = L[4]; else { or4 = 1; t6 = bh_chk(G[91], "值"); }
        t7 = bh_call(t3, 2, (Value[]){t5, t6}, "写变量");
    } else {
        bh_line = 1388;
        t3 = bh_chk(G[184], "可能回收");
        if (L[2].tag) t4 = L[2]; else { or2 = 1; t4 = bh_chk(G[224], "目标"); }
        t5 = bh_call(t3, 1, (Value[]){t4}, "可能回收");
        if (bh_truthy(t5)) {
            bh_line = 1389;
            t6 = bh_chk(G[155], "保根");
            if (L[4].tag) t7 = L[4]; else { or4 = 1; t7 = bh_chk(G[91], "值"); }
            t8 = bh_call(t6, 1, (Value[]){t7}, "保根");
            if (!L[4].tag && or4) bh_modify_outer("值"); L[4] = t8;
        } else {
        }
        bh_line = 1390;
        t3 = bh_chk(G[202], "算");
        if (L[2].tag) t4 = L[2]; else { or2 = 1; t4 = bh_chk(G[224], "目标"); }
        t5 = bh_index(t4, bh_int(2LL));
        t6 = bh_call(t3, 1, (Value[]){t5}, "算");
        if (!L[7].tag && or7) bh_modify_outer("容器"); L[7] = t6;
        bh_line = 1391;
        t3 = bh_chk(G[184], "可能回收");
        if (L[2].tag) t4 = L[2]; else { or2 = 1; t4 = bh_chk(G[224], "目标"); }
        t5 = bh_index(t4, bh_int(3LL));
        t6 = bh_call(t3, 1, (Value[]){t5}, "可能回收");
        if (bh_truthy(t6)) {
            bh_line = 1392;
            t7 = bh_chk(G[155], "保根");
            if (L[7].tag) t8 = L[7]; else { or7 = 1; t8 = bh_chk(G[210], "容器"); }
            t9 = bh_call(t7, 1, (Value[]){t8}, "保根");
            if (!L[7].tag && or7) bh_modify_outer("容器"); L[7] = t9;
        } else {
        }
        bh_line = 1393;
        t3 = bh_chk(G[202], "算");
        if (L[2].tag) t4 = L[2]; else { or2 = 1; t4 = bh_chk(G[224], "目标"); }
        t5 = bh_index(t4, bh_int(3LL));
        t6 = bh_call(t3, 1, (Value[]){t5}, "算");
        if (!L[8].tag && or8) bh_modify_outer("下标"); L[8] = t6;
        bh_line = 1394;
        t3 = bh_chk(G[157], "定行");
        if (L[1].tag) t4 = L[1]; else { or1 = 1; t4 = bh_chk(G[68], "行"); }
        t5 = bh_call(t3, 1, (Value[]){t4}, "定行");
        bh_line = 1395;
        t3 = bh_chk(G[147], "出");
        if (L[7].tag) t4 = L[7]; else { or7 = 1; t4 = bh_chk(G[210], "容器"); }
        t5 = bh_add(K[276], t4);
        t6 = bh_add(t5, K[229]);
        if (L[8].tag) t7 = L[8]; else { or8 = 1; t7 = bh_chk(G[19], "下标"); }
        t8 = bh_add(t6, t7);
        t9 = bh_add(t8, K[229]);
        if (L[4].tag) t10 = L[4]; else { or4 = 1; t10 = bh_chk(G[91], "值"); }
        t11 = bh_add(t9, t10);
        t12 = bh_add(t11, K[235]);
        t13 = bh_call(t3, 1, (Value[]){t12}, "出");
    }
    BH_RETURN(fr, BH_NIL);
}

static Value fn84(Frame *up, Value *a) {    /* 函数 编如果，第 1398 行 */
    struct { BhRoots h; Value v[5]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 5);
    L = fr.v; T = fr.v + 4;
    Value t0, t1, t2, t3, t4, t5;
    L[0] = a[0];    /* 句 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    BH_SAFEPOINT();
    bh_line = 1399;
    if (!L[1].tag && or1) bh_modify_outer("层数"); L[1] = bh_int(0LL);
    bh_line = 1400;
    {
        t0 = bh_index(L[0], bh_int(2LL));
        T[0] = bh_iter(t0);
        int64_t n1;
        for (n1 = 0; n1 < bh_list_len(T[0]); n1++) {
            BH_SAFEPOINT();
            bh_line = 1400;
            if (!L[2].tag && or2) bh_modify_outer("分支"); L[2] = bh_list_at(T[0], n1);
            bh_line = 1401;
            t1 = bh_chk(G[202], "算");
            if (L[2].tag) t2 = L[2]; else { or2 = 1; t2 = bh_chk(G[166], "分支"); }
            t3 = bh_index(t2, bh_int(0LL));
            t4 = bh_call(t1, 1, (Value[]){t3}, "算");
            if (!L[3].tag && or3) bh_modify_outer("条件"); L[3] = t4;
            bh_line = 1402;
            t1 = bh_chk(G[148], "开块");
            if (L[3].tag) t2 = L[3]; else { or3 = 1; t2 = bh_chk(G[97], "条件"); }
            t3 = bh_add(K[247], t2);
            t4 = bh_add(t3, K[245]);
            t5 = bh_call(t1, 1, (Value[]){t4}, "开块");
            bh_line = 1403;
            t1 = bh_chk(G[213], "编语句表");
            if (L[2].tag) t2 = L[2]; else { or2 = 1; t2 = bh_chk(G[166], "分支"); }
            t3 = bh_index(t2, bh_int(1LL));
            t4 = bh_call(t1, 1, (Value[]){t3}, "编语句表");
            bh_line = 1404;
            t1 = bh_chk(G[150], "否则块");
            t2 = bh_call(t1, 0, NULL, "否则块");
            bh_line = 1405;
            if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[211], "层数"); }
            t2 = bh_add(t1, bh_int(1LL));
            if (!L[1].tag && or1) bh_modify_outer("层数"); L[1] = t2;
        }
    }
    bh_line = 1406;
    t0 = BH_FALSE;
    {
        t1 = bh_index(L[0], bh_int(3LL));
        if (bh_cmp(1, t1, BH_NIL)) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1407;
        t2 = bh_chk(G[213], "编语句表");
        t3 = bh_index(L[0], bh_int(3LL));
        t4 = bh_call(t2, 1, (Value[]){t3}, "编语句表");
    } else {
    }
    bh_line = 1408;
    {
        if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[211], "层数"); }
        int64_t n2 = bh_count(t0), n3;
        for (n3 = 0; n3 < n2; n3++) {
            BH_SAFEPOINT();
            bh_line = 1409;
            t1 = bh_chk(G[149], "关块");
            t2 = bh_call(t1, 0, NULL, "关块");
        }
    }
    BH_RETURN(fr, BH_NIL);
}

static Value fn85(Frame *up, Value *a) {    /* 函数 编函数，第 1412 行 */
    struct { BhRoots h; Value v[16]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 16);
    L = fr.v; T = fr.v + 15;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17, t18, t19, t20;
    L[0] = a[0];    /* 句 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    char or5 = 0;
    char or6 = 0;
    char or7 = 0;
    char or8 = 0;
    char or9 = 0;
    char or10 = 0;
    char or11 = 0;
    char or12 = 0;
    char or13 = 0;
    char or14 = 0;
    char gf0 = 0;
    char gf1 = 0;
    BH_SAFEPOINT();
    bh_line = 1414;
    gf0 = 1;
    gf1 = 1;
    bh_line = 1415;
    if (gf1) t0 = bh_chk(G[125], "函数个数"); else if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[125], "函数个数"); }
    t1 = bh_add(t0, bh_int(1LL));
    if (gf1) G[125] = t1; else { if (!L[1].tag && or1) bh_modify_outer("函数个数"); L[1] = t1; }
    bh_line = 1416;
    if (gf1) t0 = bh_chk(G[125], "函数个数"); else if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[125], "函数个数"); }
    if (!L[2].tag && or2) bh_modify_outer("编号"); L[2] = t0;
    bh_line = 1417;
    t0 = bh_list_make(0, NULL);
    if (!L[3].tag && or3) bh_modify_outer("局部"); L[3] = t0;
    bh_line = 1418;
    t0 = bh_chk(G[76], "并入");
    if (L[3].tag) t1 = L[3]; else { or3 = 1; t1 = bh_chk(G[173], "局部"); }
    t2 = bh_index(L[0], bh_int(3LL));
    t3 = bh_call(t0, 2, (Value[]){t1, t2}, "并入");
    bh_line = 1419;
    t0 = bh_list_make(0, NULL);
    if (!L[4].tag && or4) bh_modify_outer("声明"); L[4] = t0;
    bh_line = 1420;
    t0 = bh_list_make(1, (Value[]){BH_FALSE});
    if (!L[5].tag && or5) bh_modify_outer("标记"); L[5] = t0;
    bh_line = 1421;
    t0 = bh_chk(G[165], "收集");
    t1 = bh_index(L[0], bh_int(4LL));
    if (L[3].tag) t2 = L[3]; else { or3 = 1; t2 = bh_chk(G[173], "局部"); }
    if (L[4].tag) t3 = L[4]; else { or4 = 1; t3 = bh_chk(G[174], "声明"); }
    if (L[5].tag) t4 = L[5]; else { or5 = 1; t4 = bh_chk(G[175], "标记"); }
    t5 = bh_call(t0, 4, (Value[]){t1, t2, t3, t4}, "收集");
    bh_line = 1422;
    if (gf0) t0 = bh_chk(G[126], "环境"); else if (L[7].tag) t0 = L[7]; else { or7 = 1; t0 = bh_chk(G[126], "环境"); }
    if (!L[6].tag && or6) bh_modify_outer("外面"); L[6] = t0;
    bh_line = 1423;
    t0 = bh_chk(G[146], "新环境");
    if (L[6].tag) t1 = L[6]; else { or6 = 1; t1 = bh_chk(G[226], "外面"); }
    if (L[3].tag) t2 = L[3]; else { or3 = 1; t2 = bh_chk(G[173], "局部"); }
    t3 = bh_index(L[0], bh_int(3LL));
    t4 = bi_len(1, (Value[]){t3});
    if (L[4].tag) t5 = L[4]; else { or4 = 1; t5 = bh_chk(G[174], "声明"); }
    if (L[5].tag) t6 = L[5]; else { or5 = 1; t6 = bh_chk(G[175], "标记"); }
    t7 = bh_index(t6, bh_int(0LL));
    t8 = bh_call(t0, 6, (Value[]){t1, t2, t4, t5, t7, BH_FALSE}, "新环境");
    if (gf0) G[126] = t8; else { if (!L[7].tag && or7) bh_modify_outer("环境"); L[7] = t8; }
    bh_line = 1424;
    {
        if (L[5].tag) t1 = L[5]; else { or5 = 1; t1 = bh_chk(G[175], "标记"); }
        t2 = bh_index(t1, bh_int(0LL));
        t3 = bh_bool(!bh_truthy(t2));
        if (bh_truthy(t3)) {
            t4 = bh_chk(G[189], "是叶子");
            t5 = bh_index(L[0], bh_int(4LL));
            t6 = bh_call(t4, 1, (Value[]){t5}, "是叶子");
            t0 = t6;
        } else {
            t0 = t3;
        }
    }
    bh_line = 1424;
    if (gf0) t7 = bh_chk(G[126], "环境"); else if (L[7].tag) t7 = L[7]; else { or7 = 1; t7 = bh_chk(G[126], "环境"); }
    t8 = bh_chk(G[145], "叶子位");
    bh_set_index(t7, t8, t0);
    bh_line = 1425;
    t0 = bh_chk(G[213], "编语句表");
    t1 = bh_index(L[0], bh_int(4LL));
    t2 = bh_call(t0, 1, (Value[]){t1}, "编语句表");
    bh_line = 1426;
    if (L[2].tag) t0 = L[2]; else { or2 = 1; t0 = bh_chk(G[227], "编号"); }
    t1 = bh_add(K[277], t0);
    t2 = bh_add(t1, K[278]);
    t3 = bh_index(L[0], bh_int(2LL));
    t4 = bh_add(t2, t3);
    t5 = bh_add(t4, K[279]);
    t6 = bh_index(L[0], bh_int(1LL));
    t7 = bh_add(t5, t6);
    t8 = bh_add(t7, K[280]);
    t9 = bh_list_make(1, (Value[]){t8});
    if (!L[8].tag && or8) bh_modify_outer("头"); L[8] = t9;
    bh_line = 1427;
    if (gf0) t0 = bh_chk(G[126], "环境"); else if (L[7].tag) t0 = L[7]; else { or7 = 1; t0 = bh_chk(G[126], "环境"); }
    t1 = bh_chk(G[144], "最多根位");
    t2 = bh_index(t0, t1);
    if (!L[9].tag && or9) bh_modify_outer("根数"); L[9] = t2;
    bh_line = 1428;
    if (gf0) t0 = bh_chk(G[126], "环境"); else if (L[7].tag) t0 = L[7]; else { or7 = 1; t0 = bh_chk(G[126], "环境"); }
    t1 = bh_chk(G[145], "叶子位");
    t2 = bh_index(t0, t1);
    if (bh_truthy(t2)) {
        bh_line = 1430;
        if (L[8].tag) t3 = L[8]; else { or8 = 1; t3 = bh_chk(G[25], "头"); }
        T[0] = t3;
        t4 = bh_chk(G[162], "至少一");
        if (L[3].tag) t5 = L[3]; else { or3 = 1; t5 = bh_chk(G[173], "局部"); }
        t6 = bi_len(1, (Value[]){t5});
        t7 = bh_call(t4, 1, (Value[]){t6}, "至少一");
        t8 = bh_add(K[281], t7);
        t9 = bh_add(t8, K[282]);
        t10 = bi_append(2, (Value[]){T[0], t9});
    } else {
        bh_line = 1431;
        if (L[5].tag) t3 = L[5]; else { or5 = 1; t3 = bh_chk(G[175], "标记"); }
        t4 = bh_index(t3, bh_int(0LL));
        if (bh_truthy(t4)) {
            bh_line = 1433;
            if (L[8].tag) t5 = L[8]; else { or8 = 1; t5 = bh_chk(G[25], "头"); }
            if (L[9].tag) t6 = L[9]; else { or9 = 1; t6 = bh_chk(G[228], "根数"); }
            t7 = bh_add(t6, bh_int(1LL));
            t8 = bh_add(K[283], t7);
            t9 = bh_add(t8, K[284]);
            t10 = bi_append(2, (Value[]){t5, t9});
            bh_line = 1434;
            if (L[8].tag) t5 = L[8]; else { or8 = 1; t5 = bh_chk(G[25], "头"); }
            t6 = bi_append(2, (Value[]){t5, K[285]});
            bh_line = 1435;
            if (L[8].tag) t5 = L[8]; else { or8 = 1; t5 = bh_chk(G[25], "头"); }
            if (L[9].tag) t6 = L[9]; else { or9 = 1; t6 = bh_chk(G[228], "根数"); }
            t7 = bh_add(t6, bh_int(1LL));
            t8 = bh_add(K[286], t7);
            t9 = bh_add(t8, K[235]);
            t10 = bi_append(2, (Value[]){t5, t9});
            bh_line = 1436;
            if (L[8].tag) t5 = L[8]; else { or8 = 1; t5 = bh_chk(G[25], "头"); }
            if (L[3].tag) t6 = L[3]; else { or3 = 1; t6 = bh_chk(G[173], "局部"); }
            t7 = bi_len(1, (Value[]){t6});
            t8 = bh_add(K[287], t7);
            t9 = bh_add(t8, K[288]);
            t10 = bi_append(2, (Value[]){t5, t9});
        } else {
            bh_line = 1438;
            if (L[6].tag) t5 = L[6]; else { or6 = 1; t5 = bh_chk(G[226], "外面"); }
            t6 = bh_chk(G[139], "模块位");
            t7 = bh_index(t5, t6);
            t8 = bh_bool(!bh_truthy(t7));
            if (!L[10].tag && or10) bh_modify_outer("内层"); L[10] = t8;
            bh_line = 1439;
            if (L[3].tag) t5 = L[3]; else { or3 = 1; t5 = bh_chk(G[173], "局部"); }
            t6 = bi_len(1, (Value[]){t5});
            if (L[9].tag) t7 = L[9]; else { or9 = 1; t7 = bh_chk(G[228], "根数"); }
            t8 = bh_add(t6, t7);
            if (!L[11].tag && or11) bh_modify_outer("格数"); L[11] = t8;
            bh_line = 1440;
            if (L[10].tag) t5 = L[10]; else { or10 = 1; t5 = bh_chk(G[229], "内层"); }
            if (bh_truthy(t5)) {
                bh_line = 1441;
                if (L[11].tag) t6 = L[11]; else { or11 = 1; t6 = bh_chk(G[230], "格数"); }
                t7 = bh_add(t6, bh_int(1LL));
                if (!L[11].tag && or11) bh_modify_outer("格数"); L[11] = t7;
            } else {
            }
            bh_line = 1442;
            if (L[8].tag) t5 = L[8]; else { or8 = 1; t5 = bh_chk(G[25], "头"); }
            T[0] = t5;
            t6 = bh_chk(G[162], "至少一");
            if (L[11].tag) t7 = L[11]; else { or11 = 1; t7 = bh_chk(G[230], "格数"); }
            t8 = bh_call(t6, 1, (Value[]){t7}, "至少一");
            t9 = bh_add(K[283], t8);
            t10 = bh_add(t9, K[284]);
            t11 = bi_append(2, (Value[]){T[0], t10});
            bh_line = 1443;
            if (L[8].tag) t5 = L[8]; else { or8 = 1; t5 = bh_chk(G[25], "头"); }
            t6 = bi_append(2, (Value[]){t5, K[289]});
            bh_line = 1444;
            if (L[8].tag) t5 = L[8]; else { or8 = 1; t5 = bh_chk(G[25], "头"); }
            T[0] = t5;
            t6 = bh_chk(G[162], "至少一");
            if (L[11].tag) t7 = L[11]; else { or11 = 1; t7 = bh_chk(G[230], "格数"); }
            t8 = bh_call(t6, 1, (Value[]){t7}, "至少一");
            t9 = bh_add(K[286], t8);
            t10 = bh_add(t9, K[235]);
            t11 = bi_append(2, (Value[]){T[0], t10});
            bh_line = 1445;
            if (L[8].tag) t5 = L[8]; else { or8 = 1; t5 = bh_chk(G[25], "头"); }
            if (L[3].tag) t6 = L[3]; else { or3 = 1; t6 = bh_chk(G[173], "局部"); }
            t7 = bi_len(1, (Value[]){t6});
            t8 = bh_add(K[290], t7);
            t9 = bh_add(t8, K[52]);
            t10 = bi_append(2, (Value[]){t5, t9});
            bh_line = 1446;
            if (L[10].tag) t5 = L[10]; else { or10 = 1; t5 = bh_chk(G[229], "内层"); }
            if (bh_truthy(t5)) {
                bh_line = 1447;
                if (L[8].tag) t6 = L[8]; else { or8 = 1; t6 = bh_chk(G[25], "头"); }
                if (L[11].tag) t7 = L[11]; else { or11 = 1; t7 = bh_chk(G[230], "格数"); }
                t8 = bh_sub(t7, bh_int(1LL));
                t9 = bh_add(K[291], t8);
                t10 = bh_add(t9, K[292]);
                t11 = bi_append(2, (Value[]){t6, t10});
            } else {
            }
        }
    }
    bh_line = 1448;
    t0 = bh_chk(G[231], "添加C临时");
    if (L[8].tag) t1 = L[8]; else { or8 = 1; t1 = bh_chk(G[25], "头"); }
    t2 = bh_call(t0, 1, (Value[]){t1}, "添加C临时");
    bh_line = 1449;
    if (!L[12].tag && or12) bh_modify_outer("下标"); L[12] = bh_int(0LL);
    bh_line = 1450;
    for (;;) {
        BH_SAFEPOINT();
        t0 = BH_FALSE;
        {
            bh_line = 1450;
            if (L[12].tag) t1 = L[12]; else { or12 = 1; t1 = bh_chk(G[19], "下标"); }
            if (L[3].tag) t2 = L[3]; else { or3 = 1; t2 = bh_chk(G[173], "局部"); }
            t3 = bi_len(1, (Value[]){t2});
            if (bh_cmp(2, t1, t3)) {
                t0 = BH_TRUE;
            }
        }
        if (!bh_truthy(t0)) break;
        bh_line = 1451;
        t4 = BH_FALSE;
        {
            if (L[12].tag) t5 = L[12]; else { or12 = 1; t5 = bh_chk(G[19], "下标"); }
            t6 = bh_index(L[0], bh_int(3LL));
            t7 = bi_len(1, (Value[]){t6});
            if (bh_cmp(2, t5, t7)) {
                t4 = BH_TRUE;
            }
        }
        if (bh_truthy(t4)) {
            bh_line = 1452;
            if (L[8].tag) t8 = L[8]; else { or8 = 1; t8 = bh_chk(G[25], "头"); }
            if (L[12].tag) t9 = L[12]; else { or12 = 1; t9 = bh_chk(G[19], "下标"); }
            t10 = bh_add(K[293], t9);
            t11 = bh_add(t10, K[294]);
            if (L[12].tag) t12 = L[12]; else { or12 = 1; t12 = bh_chk(G[19], "下标"); }
            t13 = bh_add(t11, t12);
            t14 = bh_add(t13, K[295]);
            if (L[3].tag) t15 = L[3]; else { or3 = 1; t15 = bh_chk(G[173], "局部"); }
            if (L[12].tag) t16 = L[12]; else { or12 = 1; t16 = bh_chk(G[19], "下标"); }
            t17 = bh_index(t15, t16);
            t18 = bh_add(t14, t17);
            t19 = bh_add(t18, K[296]);
            t20 = bi_append(2, (Value[]){t8, t19});
        } else {
        }
        bh_line = 1453;
        if (L[8].tag) t4 = L[8]; else { or8 = 1; t4 = bh_chk(G[25], "头"); }
        if (L[12].tag) t5 = L[12]; else { or12 = 1; t5 = bh_chk(G[19], "下标"); }
        t6 = bh_add(K[297], t5);
        t7 = bh_add(t6, K[298]);
        t8 = bi_append(2, (Value[]){t4, t7});
        bh_line = 1454;
        if (L[12].tag) t4 = L[12]; else { or12 = 1; t4 = bh_chk(G[19], "下标"); }
        t5 = bh_add(t4, bh_int(1LL));
        if (!L[12].tag && or12) bh_modify_outer("下标"); L[12] = t5;
    }
    bh_line = 1455;
    if (!L[12].tag && or12) bh_modify_outer("下标"); L[12] = bh_int(0LL);
    bh_line = 1456;
    for (;;) {
        BH_SAFEPOINT();
        t0 = BH_FALSE;
        {
            bh_line = 1456;
            if (L[12].tag) t1 = L[12]; else { or12 = 1; t1 = bh_chk(G[19], "下标"); }
            if (L[4].tag) t2 = L[4]; else { or4 = 1; t2 = bh_chk(G[174], "声明"); }
            t3 = bi_len(1, (Value[]){t2});
            if (bh_cmp(2, t1, t3)) {
                t0 = BH_TRUE;
            }
        }
        if (!bh_truthy(t0)) break;
        bh_line = 1457;
        if (L[8].tag) t4 = L[8]; else { or8 = 1; t4 = bh_chk(G[25], "头"); }
        if (L[12].tag) t5 = L[12]; else { or12 = 1; t5 = bh_chk(G[19], "下标"); }
        t6 = bh_add(K[299], t5);
        t7 = bh_add(t6, K[298]);
        t8 = bi_append(2, (Value[]){t4, t7});
        bh_line = 1458;
        if (L[12].tag) t4 = L[12]; else { or12 = 1; t4 = bh_chk(G[19], "下标"); }
        t5 = bh_add(t4, bh_int(1LL));
        if (!L[12].tag && or12) bh_modify_outer("下标"); L[12] = t5;
    }
    bh_line = 1459;
    if (!L[13].tag && or13) bh_modify_outer("结尾"); L[13] = K[300];
    bh_line = 1460;
    if (gf0) t0 = bh_chk(G[126], "环境"); else if (L[7].tag) t0 = L[7]; else { or7 = 1; t0 = bh_chk(G[126], "环境"); }
    t1 = bh_chk(G[145], "叶子位");
    t2 = bh_index(t0, t1);
    t3 = bh_bool(!bh_truthy(t2));
    if (bh_truthy(t3)) {
        bh_line = 1461;
        if (L[8].tag) t4 = L[8]; else { or8 = 1; t4 = bh_chk(G[25], "头"); }
        t5 = bi_append(2, (Value[]){t4, K[301]});
        bh_line = 1462;
        if (!L[13].tag && or13) bh_modify_outer("结尾"); L[13] = K[302];
    } else {
    }
    bh_line = 1463;
    t0 = bh_chk(G[124], "函数成品");
    if (L[8].tag) t1 = L[8]; else { or8 = 1; t1 = bh_chk(G[25], "头"); }
    if (gf0) t2 = bh_chk(G[126], "环境"); else if (L[7].tag) t2 = L[7]; else { or7 = 1; t2 = bh_chk(G[126], "环境"); }
    t3 = bh_chk(G[131], "代码位");
    t4 = bh_index(t2, t3);
    t5 = bh_add(t1, t4);
    if (L[13].tag) t6 = L[13]; else { or13 = 1; t6 = bh_chk(G[232], "结尾"); }
    t7 = bh_list_make(3, (Value[]){t6, K[173], K[79]});
    t8 = bh_add(t5, t7);
    t9 = bi_join(2, (Value[]){t8, K[56]});
    t10 = bi_append(2, (Value[]){t0, t9});
    bh_line = 1464;
    if (L[6].tag) t0 = L[6]; else { or6 = 1; t0 = bh_chk(G[226], "外面"); }
    if (gf0) G[126] = t0; else { if (!L[7].tag && or7) bh_modify_outer("环境"); L[7] = t0; }
    bh_line = 1465;
    if (!L[14].tag && or14) bh_modify_outer("框"); L[14] = K[227];
    bh_line = 1466;
    if (L[6].tag) t0 = L[6]; else { or6 = 1; t0 = bh_chk(G[226], "外面"); }
    t1 = bh_chk(G[139], "模块位");
    t2 = bh_index(t0, t1);
    t3 = bh_bool(!bh_truthy(t2));
    if (bh_truthy(t3)) {
        bh_line = 1467;
        if (!L[14].tag && or14) bh_modify_outer("框"); L[14] = K[303];
    } else {
    }
    bh_line = 1468;
    if (L[2].tag) t0 = L[2]; else { or2 = 1; t0 = bh_chk(G[227], "编号"); }
    t1 = bh_add(K[304], t0);
    t2 = bh_add(t1, K[229]);
    t3 = bh_index(L[0], bh_int(3LL));
    t4 = bi_len(1, (Value[]){t3});
    t5 = bh_add(t2, t4);
    t6 = bh_add(t5, K[229]);
    T[0] = t6;
    t7 = bh_chk(G[160], "C文字");
    t8 = bh_index(L[0], bh_int(2LL));
    t9 = bh_call(t7, 1, (Value[]){t8}, "C文字");
    t10 = bh_add(T[0], t9);
    t11 = bh_add(t10, K[229]);
    if (L[14].tag) t12 = L[14]; else { or14 = 1; t12 = bh_chk(G[233], "框"); }
    t13 = bh_add(t11, t12);
    t14 = bh_add(t13, K[102]);
    BH_RETURN(fr, t14);
    BH_RETURN(fr, BH_NIL);
}

static Value fn86(Frame *up, Value *a) {    /* 函数 添加C临时，第 1471 行 */
    struct { BhRoots h; Value v[3]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 3);
    L = fr.v; T = fr.v + 3;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12;
    L[0] = a[0];    /* 头 */
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    BH_SAFEPOINT();
    bh_line = 1473;
    t0 = BH_FALSE;
    {
        t1 = bh_chk(G[126], "环境");
        t2 = bh_chk(G[141], "最多位");
        t3 = bh_index(t1, t2);
        if (bh_cmp(3, t3, bh_int(0LL))) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1474;
        t4 = bh_list_make(0, NULL);
        if (!L[1].tag && or1) bh_modify_outer("名们"); L[1] = t4;
        bh_line = 1475;
        if (!L[2].tag && or2) bh_modify_outer("下标"); L[2] = bh_int(0LL);
        bh_line = 1476;
        for (;;) {
            BH_SAFEPOINT();
            t4 = BH_FALSE;
            {
                bh_line = 1476;
                if (L[2].tag) t5 = L[2]; else { or2 = 1; t5 = bh_chk(G[19], "下标"); }
                t6 = bh_chk(G[126], "环境");
                t7 = bh_chk(G[141], "最多位");
                t8 = bh_index(t6, t7);
                if (bh_cmp(2, t5, t8)) {
                    t4 = BH_TRUE;
                }
            }
            if (!bh_truthy(t4)) break;
            bh_line = 1477;
            if (L[1].tag) t9 = L[1]; else { or1 = 1; t9 = bh_chk(G[234], "名们"); }
            if (L[2].tag) t10 = L[2]; else { or2 = 1; t10 = bh_chk(G[19], "下标"); }
            t11 = bh_add(K[90], t10);
            t12 = bi_append(2, (Value[]){t9, t11});
            bh_line = 1478;
            if (L[2].tag) t9 = L[2]; else { or2 = 1; t9 = bh_chk(G[19], "下标"); }
            t10 = bh_add(t9, bh_int(1LL));
            if (!L[2].tag && or2) bh_modify_outer("下标"); L[2] = t10;
        }
        bh_line = 1479;
        if (L[1].tag) t4 = L[1]; else { or1 = 1; t4 = bh_chk(G[234], "名们"); }
        t5 = bi_join(2, (Value[]){t4, K[229]});
        t6 = bh_add(K[305], t5);
        t7 = bh_add(t6, K[52]);
        t8 = bi_append(2, (Value[]){L[0], t7});
    } else {
    }
    BH_RETURN(fr, BH_NIL);
}

static Value fn87(Frame *up, Value *a) {    /* 函数 编译，第 1482 行 */
    struct { BhRoots h; Value v[23]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 23);
    L = fr.v; T = fr.v + 20;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17, t18;
    L[0] = a[0];    /* 源码 */
    char or0 = 0;
    L[1] = a[1];    /* 文件名 */
    char or1 = 0;
    L[2] = a[2];    /* 运行时 */
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    char or5 = 0;
    char or6 = 0;
    char or7 = 0;
    char or8 = 0;
    char or9 = 0;
    char or10 = 0;
    char or11 = 0;
    char or12 = 0;
    char or13 = 0;
    char or14 = 0;
    char or15 = 0;
    char or16 = 0;
    char or17 = 0;
    char or18 = 0;
    char or19 = 0;
    char gf0 = 0;
    char gf1 = 0;
    char gf2 = 0;
    char gf3 = 0;
    BH_SAFEPOINT();
    bh_line = 1484;
    gf0 = 1;
    gf1 = 1;
    gf2 = 1;
    gf3 = 1;
    bh_line = 1485;
    t0 = bi_chr(1, (Value[]){bh_int(13LL)});
    t1 = bh_add(t0, K[56]);
    t2 = bi_split(2, (Value[]){L[0], t1});
    t3 = bi_join(2, (Value[]){t2, K[56]});
    L[0] = t3;
    bh_line = 1486;
    t0 = bi_chr(1, (Value[]){bh_int(13LL)});
    t1 = bi_split(2, (Value[]){L[0], t0});
    t2 = bi_join(2, (Value[]){t1, K[56]});
    L[0] = t2;
    bh_line = 1487;
    t0 = bi_split(2, (Value[]){L[0], K[56]});
    if (gf0) G[18] = t0; else { if (!L[3].tag && or3) bh_modify_outer("源码行表"); L[3] = t0; }
    bh_line = 1488;
    t0 = bh_chk(G[55], "词法分析");
    t1 = bh_call(t0, 0, NULL, "词法分析");
    if (gf1) G[56] = t1; else { if (!L[4].tag && or4) bh_modify_outer("记号"); L[4] = t1; }
    bh_line = 1489;
    if (gf2) G[57] = bh_int(0LL); else { if (!L[5].tag && or5) bh_modify_outer("游标"); L[5] = bh_int(0LL); }
    bh_line = 1490;
    t0 = bh_chk(G[79], "解析程序");
    t1 = bh_call(t0, 0, NULL, "解析程序");
    if (!L[6].tag && or6) bh_modify_outer("树"); L[6] = t1;
    bh_line = 1491;
    t0 = bh_chk(G[171], "读内置表");
    t1 = bh_call(t0, 1, (Value[]){L[2]}, "读内置表");
    bh_line = 1492;
    t0 = bh_chk(G[177], "找全局赋值");
    if (L[6].tag) t1 = L[6]; else { or6 = 1; t1 = bh_chk(G[235], "树"); }
    t2 = bh_call(t0, 2, (Value[]){t1, BH_TRUE}, "找全局赋值");
    bh_line = 1493;
    t0 = bh_chk(G[146], "新环境");
    t1 = bh_list_make(0, NULL);
    t2 = bh_list_make(0, NULL);
    t3 = bh_call(t0, 6, (Value[]){BH_NIL, t1, bh_int(0LL), t2, BH_FALSE, BH_TRUE}, "新环境");
    if (gf3) G[126] = t3; else { if (!L[7].tag && or7) bh_modify_outer("环境"); L[7] = t3; }
    bh_line = 1494;
    t0 = bh_chk(G[213], "编语句表");
    if (L[6].tag) t1 = L[6]; else { or6 = 1; t1 = bh_chk(G[235], "树"); }
    t2 = bh_call(t0, 1, (Value[]){t1}, "编语句表");
    bh_line = 1495;
    if (gf3) t0 = bh_chk(G[126], "环境"); else if (L[7].tag) t0 = L[7]; else { or7 = 1; t0 = bh_chk(G[126], "环境"); }
    t1 = bh_chk(G[131], "代码位");
    t2 = bh_index(t0, t1);
    if (!L[8].tag && or8) bh_modify_outer("主体"); L[8] = t2;
    bh_line = 1496;
    t0 = bh_list_make(0, NULL);
    if (!L[9].tag && or9) bh_modify_outer("模块头"); L[9] = t0;
    bh_line = 1497;
    t0 = bh_chk(G[231], "添加C临时");
    if (L[9].tag) t1 = L[9]; else { or9 = 1; t1 = bh_chk(G[236], "模块头"); }
    t2 = bh_call(t0, 1, (Value[]){t1}, "添加C临时");
    bh_line = 1499;
    t0 = BH_FALSE;
    {
        t1 = bi_find(2, (Value[]){L[1], K[306]});
        if (bh_cmp(5, t1, bh_int(0LL))) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1500;
        L[1] = K[307];
    } else {
    }
    bh_line = 1501;
    t0 = bh_add(K[308], L[1]);
    t1 = bh_add(t0, K[296]);
    t2 = bh_list_make(2, (Value[]){t1, L[2]});
    if (!L[10].tag && or10) bh_modify_outer("成品"); L[10] = t2;
    bh_line = 1503;
    t0 = bh_list_make(0, NULL);
    if (!L[11].tag && or11) bh_modify_outer("名字们"); L[11] = t0;
    bh_line = 1504;
    {
        t0 = bh_chk(G[122], "全局名表");
        T[0] = bh_iter(t0);
        int64_t n1;
        for (n1 = 0; n1 < bh_list_len(T[0]); n1++) {
            BH_SAFEPOINT();
            bh_line = 1504;
            if (!L[12].tag && or12) bh_modify_outer("名"); L[12] = bh_list_at(T[0], n1);
            bh_line = 1505;
            if (L[11].tag) t1 = L[11]; else { or11 = 1; t1 = bh_chk(G[69], "名字们"); }
            T[1] = t1;
            t2 = bh_chk(G[160], "C文字");
            if (L[12].tag) t3 = L[12]; else { or12 = 1; t3 = bh_chk(G[89], "名"); }
            t4 = bh_call(t2, 1, (Value[]){t3}, "C文字");
            t5 = bi_append(2, (Value[]){T[1], t4});
        }
    }
    bh_line = 1506;
    if (L[11].tag) t0 = L[11]; else { or11 = 1; t0 = bh_chk(G[69], "名字们"); }
    t1 = bi_append(2, (Value[]){t0, K[227]});
    bh_line = 1507;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    T[0] = t0;
    t1 = bh_chk(G[162], "至少一");
    t2 = bh_chk(G[122], "全局名表");
    t3 = bi_len(1, (Value[]){t2});
    t4 = bh_call(t1, 1, (Value[]){t3}, "至少一");
    t5 = bh_add(K[309], t4);
    t6 = bh_add(t5, K[310]);
    t7 = bi_append(2, (Value[]){T[0], t6});
    bh_line = 1508;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    if (L[11].tag) t1 = L[11]; else { or11 = 1; t1 = bh_chk(G[69], "名字们"); }
    t2 = bi_join(2, (Value[]){t1, K[229]});
    t3 = bh_add(K[311], t2);
    t4 = bh_add(t3, K[312]);
    t5 = bi_append(2, (Value[]){t0, t4});
    bh_line = 1509;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    T[0] = t0;
    t1 = bh_chk(G[162], "至少一");
    t2 = bh_chk(G[123], "常量表");
    t3 = bi_len(1, (Value[]){t2});
    t4 = bh_call(t1, 1, (Value[]){t3}, "至少一");
    t5 = bh_add(K[313], t4);
    t6 = bh_add(t5, K[310]);
    t7 = bi_append(2, (Value[]){T[0], t6});
    bh_line = 1511;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    t1 = bi_append(2, (Value[]){t0, K[314]});
    bh_line = 1512;
    {
        if (gf0) t0 = bh_chk(G[18], "源码行表"); else if (L[3].tag) t0 = L[3]; else { or3 = 1; t0 = bh_chk(G[18], "源码行表"); }
        T[0] = bh_iter(t0);
        int64_t n2;
        for (n2 = 0; n2 < bh_list_len(T[0]); n2++) {
            BH_SAFEPOINT();
            bh_line = 1512;
            if (!L[13].tag && or13) bh_modify_outer("一行"); L[13] = bh_list_at(T[0], n2);
            bh_line = 1513;
            if (L[10].tag) t1 = L[10]; else { or10 = 1; t1 = bh_chk(G[237], "成品"); }
            T[1] = t1;
            t2 = bh_chk(G[160], "C文字");
            T[2] = t2;
            t3 = bh_chk(G[27], "去空白");
            if (L[13].tag) t4 = L[13]; else { or13 = 1; t4 = bh_chk(G[238], "一行"); }
            t5 = bh_call(t3, 1, (Value[]){t4}, "去空白");
            t6 = bh_call(T[2], 1, (Value[]){t5}, "C文字");
            t7 = bh_add(K[172], t6);
            t8 = bh_add(t7, K[51]);
            t9 = bi_append(2, (Value[]){T[1], t8});
        }
    }
    bh_line = 1514;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    t1 = bi_append(2, (Value[]){t0, K[315]});
    bh_line = 1516;
    {
        t1 = BH_FALSE;
        {
            t2 = bh_chk(G[122], "全局名表");
            t3 = bi_find(2, (Value[]){t2, K[316]});
            if (bh_cmp(5, t3, bh_int(0LL))) {
                t1 = BH_TRUE;
            }
        }
        if (bh_truthy(t1)) {
            t0 = t1;
        } else {
            t4 = BH_FALSE;
            {
                bh_line = 1516;
                t5 = bh_chk(G[130], "直接调用过");
                t6 = bi_find(2, (Value[]){t5, K[316]});
                if (bh_cmp(5, t6, bh_int(0LL))) {
                    t4 = BH_TRUE;
                }
            }
            t0 = t4;
        }
    }
    bh_line = 1516;
    if (!L[14].tag && or14) bh_modify_outer("带源码"); L[14] = t0;
    bh_line = 1517;
    if (L[14].tag) t0 = L[14]; else { or14 = 1; t0 = bh_chk(G[239], "带源码"); }
    if (bh_truthy(t0)) {
        bh_line = 1518;
        if (L[10].tag) t1 = L[10]; else { or10 = 1; t1 = bh_chk(G[237], "成品"); }
        t2 = bi_append(2, (Value[]){t1, K[317]});
        bh_line = 1519;
        t1 = bi_split(2, (Value[]){L[2], K[56]});
        if (!L[15].tag && or15) bh_modify_outer("片们"); L[15] = t1;
        bh_line = 1520;
        if (!L[16].tag && or16) bh_modify_outer("下标"); L[16] = bh_int(0LL);
        bh_line = 1521;
        for (;;) {
            BH_SAFEPOINT();
            t1 = BH_FALSE;
            {
                bh_line = 1521;
                if (L[16].tag) t2 = L[16]; else { or16 = 1; t2 = bh_chk(G[19], "下标"); }
                if (L[15].tag) t3 = L[15]; else { or15 = 1; t3 = bh_chk(G[169], "片们"); }
                t4 = bi_len(1, (Value[]){t3});
                if (bh_cmp(2, t2, t4)) {
                    t1 = BH_TRUE;
                }
            }
            if (!bh_truthy(t1)) break;
            bh_line = 1522;
            if (L[15].tag) t5 = L[15]; else { or15 = 1; t5 = bh_chk(G[169], "片们"); }
            if (L[16].tag) t6 = L[16]; else { or16 = 1; t6 = bh_chk(G[19], "下标"); }
            t7 = bh_index(t5, t6);
            if (!L[13].tag && or13) bh_modify_outer("一行"); L[13] = t7;
            bh_line = 1523;
            t5 = BH_FALSE;
            {
                if (L[16].tag) t6 = L[16]; else { or16 = 1; t6 = bh_chk(G[19], "下标"); }
                if (L[15].tag) t7 = L[15]; else { or15 = 1; t7 = bh_chk(G[169], "片们"); }
                t8 = bi_len(1, (Value[]){t7});
                t9 = bh_sub(t8, bh_int(1LL));
                if (bh_cmp(2, t6, t9)) {
                    t5 = BH_TRUE;
                }
            }
            if (bh_truthy(t5)) {
                bh_line = 1524;
                if (L[13].tag) t10 = L[13]; else { or13 = 1; t10 = bh_chk(G[238], "一行"); }
                t11 = bh_add(t10, K[56]);
                if (!L[13].tag && or13) bh_modify_outer("一行"); L[13] = t11;
            } else {
            }
            bh_line = 1525;
            if (L[10].tag) t5 = L[10]; else { or10 = 1; t5 = bh_chk(G[237], "成品"); }
            T[0] = t5;
            t6 = bh_chk(G[160], "C文字");
            if (L[13].tag) t7 = L[13]; else { or13 = 1; t7 = bh_chk(G[238], "一行"); }
            t8 = bh_call(t6, 1, (Value[]){t7}, "C文字");
            t9 = bh_add(K[172], t8);
            t10 = bh_add(t9, K[51]);
            t11 = bi_append(2, (Value[]){T[0], t10});
            bh_line = 1526;
            if (L[16].tag) t5 = L[16]; else { or16 = 1; t5 = bh_chk(G[19], "下标"); }
            t6 = bh_add(t5, bh_int(1LL));
            if (!L[16].tag && or16) bh_modify_outer("下标"); L[16] = t6;
        }
        bh_line = 1527;
        if (L[10].tag) t1 = L[10]; else { or10 = 1; t1 = bh_chk(G[237], "成品"); }
        t2 = bi_append(2, (Value[]){t1, K[315]});
    } else {
    }
    bh_line = 1529;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    t1 = bi_append(2, (Value[]){t0, K[79]});
    bh_line = 1530;
    if (!L[16].tag && or16) bh_modify_outer("下标"); L[16] = bh_int(1LL);
    bh_line = 1531;
    for (;;) {
        BH_SAFEPOINT();
        t0 = BH_FALSE;
        {
            bh_line = 1531;
            if (L[16].tag) t1 = L[16]; else { or16 = 1; t1 = bh_chk(G[19], "下标"); }
            t2 = bh_chk(G[125], "函数个数");
            if (bh_cmp(4, t1, t2)) {
                t0 = BH_TRUE;
            }
        }
        if (!bh_truthy(t0)) break;
        bh_line = 1532;
        if (L[10].tag) t3 = L[10]; else { or10 = 1; t3 = bh_chk(G[237], "成品"); }
        if (L[16].tag) t4 = L[16]; else { or16 = 1; t4 = bh_chk(G[19], "下标"); }
        t5 = bh_add(K[277], t4);
        t6 = bh_add(t5, K[318]);
        t7 = bi_append(2, (Value[]){t3, t6});
        bh_line = 1533;
        if (L[16].tag) t3 = L[16]; else { or16 = 1; t3 = bh_chk(G[19], "下标"); }
        t4 = bh_add(t3, bh_int(1LL));
        if (!L[16].tag && or16) bh_modify_outer("下标"); L[16] = t4;
    }
    bh_line = 1534;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    t1 = bi_append(2, (Value[]){t0, K[79]});
    bh_line = 1535;
    t0 = bh_chk(G[76], "并入");
    if (L[10].tag) t1 = L[10]; else { or10 = 1; t1 = bh_chk(G[237], "成品"); }
    t2 = bh_chk(G[124], "函数成品");
    t3 = bh_call(t0, 2, (Value[]){t1, t2}, "并入");
    bh_line = 1536;
    t0 = bh_chk(G[162], "至少一");
    if (gf3) t1 = bh_chk(G[126], "环境"); else if (L[7].tag) t1 = L[7]; else { or7 = 1; t1 = bh_chk(G[126], "环境"); }
    t2 = bh_chk(G[144], "最多根位");
    t3 = bh_index(t1, t2);
    t4 = bh_call(t0, 1, (Value[]){t3}, "至少一");
    if (!L[17].tag && or17) bh_modify_outer("格数"); L[17] = t4;
    bh_line = 1537;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    t1 = bi_append(2, (Value[]){t0, K[319]});
    bh_line = 1538;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    if (L[17].tag) t1 = L[17]; else { or17 = 1; t1 = bh_chk(G[230], "格数"); }
    t2 = bh_add(K[283], t1);
    t3 = bh_add(t2, K[284]);
    t4 = bi_append(2, (Value[]){t0, t3});
    bh_line = 1539;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    t1 = bi_append(2, (Value[]){t0, K[320]});
    bh_line = 1540;
    t0 = bh_chk(G[76], "并入");
    if (L[10].tag) t1 = L[10]; else { or10 = 1; t1 = bh_chk(G[237], "成品"); }
    if (L[9].tag) t2 = L[9]; else { or9 = 1; t2 = bh_chk(G[236], "模块头"); }
    t3 = bh_call(t0, 2, (Value[]){t1, t2}, "并入");
    bh_line = 1541;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    if (L[17].tag) t1 = L[17]; else { or17 = 1; t1 = bh_chk(G[230], "格数"); }
    t2 = bh_add(K[286], t1);
    t3 = bh_add(t2, K[235]);
    t4 = bi_append(2, (Value[]){t0, t3});
    bh_line = 1542;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    t1 = bi_append(2, (Value[]){t0, K[321]});
    bh_line = 1543;
    t0 = bh_chk(G[76], "并入");
    if (L[10].tag) t1 = L[10]; else { or10 = 1; t1 = bh_chk(G[237], "成品"); }
    if (L[8].tag) t2 = L[8]; else { or8 = 1; t2 = bh_chk(G[240], "主体"); }
    t3 = bh_call(t0, 2, (Value[]){t1, t2}, "并入");
    bh_line = 1544;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    t1 = bi_append(2, (Value[]){t0, K[322]});
    bh_line = 1545;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    t1 = bi_append(2, (Value[]){t0, K[173]});
    bh_line = 1546;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    t1 = bi_append(2, (Value[]){t0, K[79]});
    bh_line = 1547;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    t1 = bi_append(2, (Value[]){t0, K[323]});
    bh_line = 1548;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    t1 = bi_append(2, (Value[]){t0, K[324]});
    bh_line = 1549;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    t1 = bi_append(2, (Value[]){t0, K[325]});
    bh_line = 1550;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    t1 = bi_append(2, (Value[]){t0, K[326]});
    bh_line = 1551;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    if (gf0) t1 = bh_chk(G[18], "源码行表"); else if (L[3].tag) t1 = L[3]; else { or3 = 1; t1 = bh_chk(G[18], "源码行表"); }
    t2 = bi_len(1, (Value[]){t1});
    t3 = bh_add(K[327], t2);
    t4 = bh_add(t3, K[52]);
    t5 = bi_append(2, (Value[]){t0, t4});
    bh_line = 1552;
    if (L[14].tag) t0 = L[14]; else { or14 = 1; t0 = bh_chk(G[239], "带源码"); }
    if (bh_truthy(t0)) {
        bh_line = 1553;
        if (L[10].tag) t1 = L[10]; else { or10 = 1; t1 = bh_chk(G[237], "成品"); }
        t2 = bi_append(2, (Value[]){t1, K[328]});
        bh_line = 1554;
        if (L[10].tag) t1 = L[10]; else { or10 = 1; t1 = bh_chk(G[237], "成品"); }
        t2 = bi_split(2, (Value[]){L[2], K[56]});
        t3 = bi_len(1, (Value[]){t2});
        t4 = bh_add(K[329], t3);
        t5 = bh_add(t4, K[52]);
        t6 = bi_append(2, (Value[]){t1, t5});
    } else {
    }
    bh_line = 1555;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    t1 = bh_chk(G[122], "全局名表");
    t2 = bi_len(1, (Value[]){t1});
    t3 = bh_add(K[330], t2);
    t4 = bh_add(t3, K[331]);
    t5 = bh_chk(G[123], "常量表");
    t6 = bi_len(1, (Value[]){t5});
    t7 = bh_add(t4, t6);
    t8 = bh_add(t7, K[235]);
    t9 = bi_append(2, (Value[]){t0, t8});
    bh_line = 1556;
    if (!L[16].tag && or16) bh_modify_outer("下标"); L[16] = bh_int(0LL);
    bh_line = 1557;
    for (;;) {
        BH_SAFEPOINT();
        t0 = BH_FALSE;
        {
            bh_line = 1557;
            if (L[16].tag) t1 = L[16]; else { or16 = 1; t1 = bh_chk(G[19], "下标"); }
            t2 = bh_chk(G[123], "常量表");
            t3 = bi_len(1, (Value[]){t2});
            if (bh_cmp(2, t1, t3)) {
                t0 = BH_TRUE;
            }
        }
        if (!bh_truthy(t0)) break;
        bh_line = 1558;
        t4 = bh_chk(G[123], "常量表");
        if (L[16].tag) t5 = L[16]; else { or16 = 1; t5 = bh_chk(G[19], "下标"); }
        t6 = bh_index(t4, t5);
        if (!L[18].tag && or18) bh_modify_outer("项"); L[18] = t6;
        bh_line = 1559;
        if (!L[19].tag && or19) bh_modify_outer("制造"); L[19] = K[332];
        bh_line = 1560;
        t4 = BH_FALSE;
        {
            if (L[18].tag) t5 = L[18]; else { or18 = 1; t5 = bh_chk(G[75], "项"); }
            t6 = bi_slice(3, (Value[]){t5, bh_int(0LL), bh_int(1LL)});
            if (bh_cmp(0, t6, K[190])) {
                t4 = BH_TRUE;
            }
        }
        if (bh_truthy(t4)) {
            bh_line = 1561;
            if (!L[19].tag && or19) bh_modify_outer("制造"); L[19] = K[333];
        } else {
        }
        bh_line = 1562;
        if (L[10].tag) t4 = L[10]; else { or10 = 1; t4 = bh_chk(G[237], "成品"); }
        T[0] = t4;
        if (L[16].tag) t5 = L[16]; else { or16 = 1; t5 = bh_chk(G[19], "下标"); }
        t6 = bh_add(K[334], t5);
        t7 = bh_add(t6, K[214]);
        if (L[19].tag) t8 = L[19]; else { or19 = 1; t8 = bh_chk(G[241], "制造"); }
        t9 = bh_add(t7, t8);
        T[1] = t9;
        t10 = bh_chk(G[160], "C文字");
        if (L[18].tag) t11 = L[18]; else { or18 = 1; t11 = bh_chk(G[75], "项"); }
        if (L[18].tag) t12 = L[18]; else { or18 = 1; t12 = bh_chk(G[75], "项"); }
        t13 = bi_len(1, (Value[]){t12});
        t14 = bi_slice(3, (Value[]){t11, bh_int(1LL), t13});
        t15 = bh_call(t10, 1, (Value[]){t14}, "C文字");
        t16 = bh_add(T[1], t15);
        t17 = bh_add(t16, K[235]);
        t18 = bi_append(2, (Value[]){T[0], t17});
        bh_line = 1563;
        if (L[16].tag) t4 = L[16]; else { or16 = 1; t4 = bh_chk(G[19], "下标"); }
        t5 = bh_add(t4, bh_int(1LL));
        if (!L[16].tag && or16) bh_modify_outer("下标"); L[16] = t5;
    }
    bh_line = 1564;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    t1 = bi_append(2, (Value[]){t0, K[335]});
    bh_line = 1565;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    t1 = bi_append(2, (Value[]){t0, K[336]});
    bh_line = 1566;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    t1 = bi_append(2, (Value[]){t0, K[337]});
    bh_line = 1567;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    t1 = bi_append(2, (Value[]){t0, K[173]});
    bh_line = 1568;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    t1 = bi_append(2, (Value[]){t0, K[79]});
    bh_line = 1569;
    if (L[10].tag) t0 = L[10]; else { or10 = 1; t0 = bh_chk(G[237], "成品"); }
    t1 = bi_join(2, (Value[]){t0, K[56]});
    BH_RETURN(fr, t1);
    BH_RETURN(fr, BH_NIL);
}

static Value fn88(Frame *up, Value *a) {    /* 函数 引，第 1576 行 */
    Value L[1] = {{0}};
    Value t0, t1, t2, t3;
    L[0] = a[0];    /* 文 */
    char or0 = 0;
    bh_line = 1578;
    t0 = bi_split(2, (Value[]){L[0], K[58]});
    t1 = bi_join(2, (Value[]){t0, K[338]});
    t2 = bh_add(K[58], t1);
    t3 = bh_add(t2, K[58]);
    return t3;
    return BH_NIL;
}

static Value fn89(Frame *up, Value *a) {    /* 函数 主程序，第 1581 行 */
    struct { BhRoots h; Value v[17]; } fr;
    Value *L, *T;
    BH_ENTER(fr, 17);
    L = fr.v; T = fr.v + 15;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17, t18, t19, t20, t21, t22, t23, t24, t25, t26;
    char or0 = 0;
    char or1 = 0;
    char or2 = 0;
    char or3 = 0;
    char or4 = 0;
    char or5 = 0;
    char or6 = 0;
    char or7 = 0;
    char or8 = 0;
    char or9 = 0;
    char or10 = 0;
    char or11 = 0;
    char or12 = 0;
    char or13 = 0;
    char or14 = 0;
    BH_SAFEPOINT();
    bh_line = 1582;
    t0 = bi_args(0, NULL);
    if (!L[0].tag && or0) bh_modify_outer("参数表"); L[0] = t0;
    bh_line = 1583;
    if (!L[1].tag && or1) bh_modify_outer("输入文件"); L[1] = BH_NIL;
    bh_line = 1584;
    if (!L[2].tag && or2) bh_modify_outer("输出名"); L[2] = BH_NIL;
    bh_line = 1585;
    if (!L[3].tag && or3) bh_modify_outer("只要C"); L[3] = BH_FALSE;
    bh_line = 1586;
    if (!L[4].tag && or4) bh_modify_outer("C编译器"); L[4] = K[339];
    bh_line = 1587;
    if (!L[5].tag && or5) bh_modify_outer("运行时文件"); L[5] = BH_NIL;
    bh_line = 1588;
    if (!L[6].tag && or6) bh_modify_outer("下标"); L[6] = bh_int(0LL);
    bh_line = 1589;
    for (;;) {
        BH_SAFEPOINT();
        t0 = BH_FALSE;
        {
            bh_line = 1589;
            if (L[6].tag) t1 = L[6]; else { or6 = 1; t1 = bh_chk(G[19], "下标"); }
            if (L[0].tag) t2 = L[0]; else { or0 = 1; t2 = bh_chk(G[244], "参数表"); }
            t3 = bi_len(1, (Value[]){t2});
            if (bh_cmp(2, t1, t3)) {
                t0 = BH_TRUE;
            }
        }
        if (!bh_truthy(t0)) break;
        bh_line = 1590;
        if (L[0].tag) t4 = L[0]; else { or0 = 1; t4 = bh_chk(G[244], "参数表"); }
        if (L[6].tag) t5 = L[6]; else { or6 = 1; t5 = bh_chk(G[19], "下标"); }
        t6 = bh_index(t4, t5);
        if (!L[7].tag && or7) bh_modify_outer("项"); L[7] = t6;
        bh_line = 1591;
        t4 = BH_FALSE;
        {
            if (L[6].tag) t5 = L[6]; else { or6 = 1; t5 = bh_chk(G[19], "下标"); }
            t6 = bh_add(t5, bh_int(1LL));
            if (L[0].tag) t7 = L[0]; else { or0 = 1; t7 = bh_chk(G[244], "参数表"); }
            t8 = bi_len(1, (Value[]){t7});
            if (bh_cmp(2, t6, t8)) {
                t4 = BH_TRUE;
            }
        }
        bh_line = 1591;
        if (!L[8].tag && or8) bh_modify_outer("有下一个"); L[8] = t4;
        bh_line = 1592;
        {
            t5 = BH_FALSE;
            {
                if (L[7].tag) t6 = L[7]; else { or7 = 1; t6 = bh_chk(G[75], "项"); }
                if (bh_cmp(0, t6, K[340])) {
                    t5 = BH_TRUE;
                }
            }
            if (bh_truthy(t5)) {
                bh_line = 1592;
                if (L[8].tag) t7 = L[8]; else { or8 = 1; t7 = bh_chk(G[245], "有下一个"); }
                t4 = t7;
            } else {
                t4 = t5;
            }
        }
        if (bh_truthy(t4)) {
            bh_line = 1593;
            if (L[0].tag) t8 = L[0]; else { or0 = 1; t8 = bh_chk(G[244], "参数表"); }
            if (L[6].tag) t9 = L[6]; else { or6 = 1; t9 = bh_chk(G[19], "下标"); }
            t10 = bh_add(t9, bh_int(1LL));
            t11 = bh_index(t8, t10);
            if (!L[2].tag && or2) bh_modify_outer("输出名"); L[2] = t11;
            bh_line = 1594;
            if (L[6].tag) t8 = L[6]; else { or6 = 1; t8 = bh_chk(G[19], "下标"); }
            t9 = bh_add(t8, bh_int(1LL));
            if (!L[6].tag && or6) bh_modify_outer("下标"); L[6] = t9;
        } else {
            {
                t9 = BH_FALSE;
                {
                    bh_line = 1595;
                    if (L[7].tag) t10 = L[7]; else { or7 = 1; t10 = bh_chk(G[75], "项"); }
                    if (bh_cmp(0, t10, K[341])) {
                        t9 = BH_TRUE;
                    }
                }
                if (bh_truthy(t9)) {
                    bh_line = 1595;
                    if (L[8].tag) t11 = L[8]; else { or8 = 1; t11 = bh_chk(G[245], "有下一个"); }
                    t8 = t11;
                } else {
                    t8 = t9;
                }
            }
            if (bh_truthy(t8)) {
                bh_line = 1596;
                if (L[0].tag) t12 = L[0]; else { or0 = 1; t12 = bh_chk(G[244], "参数表"); }
                if (L[6].tag) t13 = L[6]; else { or6 = 1; t13 = bh_chk(G[19], "下标"); }
                t14 = bh_add(t13, bh_int(1LL));
                t15 = bh_index(t12, t14);
                if (!L[4].tag && or4) bh_modify_outer("C编译器"); L[4] = t15;
                bh_line = 1597;
                if (L[6].tag) t12 = L[6]; else { or6 = 1; t12 = bh_chk(G[19], "下标"); }
                t13 = bh_add(t12, bh_int(1LL));
                if (!L[6].tag && or6) bh_modify_outer("下标"); L[6] = t13;
            } else {
                {
                    t13 = BH_FALSE;
                    {
                        bh_line = 1598;
                        if (L[7].tag) t14 = L[7]; else { or7 = 1; t14 = bh_chk(G[75], "项"); }
                        if (bh_cmp(0, t14, K[342])) {
                            t13 = BH_TRUE;
                        }
                    }
                    if (bh_truthy(t13)) {
                        bh_line = 1598;
                        if (L[8].tag) t15 = L[8]; else { or8 = 1; t15 = bh_chk(G[245], "有下一个"); }
                        t12 = t15;
                    } else {
                        t12 = t13;
                    }
                }
                if (bh_truthy(t12)) {
                    bh_line = 1599;
                    if (L[0].tag) t16 = L[0]; else { or0 = 1; t16 = bh_chk(G[244], "参数表"); }
                    if (L[6].tag) t17 = L[6]; else { or6 = 1; t17 = bh_chk(G[19], "下标"); }
                    t18 = bh_add(t17, bh_int(1LL));
                    t19 = bh_index(t16, t18);
                    if (!L[5].tag && or5) bh_modify_outer("运行时文件"); L[5] = t19;
                    bh_line = 1600;
                    if (L[6].tag) t16 = L[6]; else { or6 = 1; t16 = bh_chk(G[19], "下标"); }
                    t17 = bh_add(t16, bh_int(1LL));
                    if (!L[6].tag && or6) bh_modify_outer("下标"); L[6] = t17;
                } else {
                    t16 = BH_FALSE;
                    {
                        bh_line = 1601;
                        if (L[7].tag) t17 = L[7]; else { or7 = 1; t17 = bh_chk(G[75], "项"); }
                        if (bh_cmp(0, t17, K[343])) {
                            t16 = BH_TRUE;
                        }
                    }
                    if (bh_truthy(t16)) {
                        bh_line = 1602;
                        if (!L[3].tag && or3) bh_modify_outer("只要C"); L[3] = BH_TRUE;
                    } else {
                        {
                            t19 = BH_FALSE;
                            {
                                bh_line = 1603;
                                if (L[1].tag) t20 = L[1]; else { or1 = 1; t20 = bh_chk(G[246], "输入文件"); }
                                if (bh_cmp(0, t20, BH_NIL)) {
                                    t19 = BH_TRUE;
                                }
                            }
                            if (bh_truthy(t19)) {
                                t21 = BH_FALSE;
                                {
                                    bh_line = 1603;
                                    if (L[7].tag) t22 = L[7]; else { or7 = 1; t22 = bh_chk(G[75], "项"); }
                                    t23 = bi_slice(3, (Value[]){t22, bh_int(0LL), bh_int(1LL)});
                                    if (bh_cmp(1, t23, K[160])) {
                                        t21 = BH_TRUE;
                                    }
                                }
                                t18 = t21;
                            } else {
                                t18 = t19;
                            }
                        }
                        if (bh_truthy(t18)) {
                            bh_line = 1604;
                            if (L[7].tag) t24 = L[7]; else { or7 = 1; t24 = bh_chk(G[75], "项"); }
                            if (!L[1].tag && or1) bh_modify_outer("输入文件"); L[1] = t24;
                        } else {
                            bh_line = 1606;
                            if (L[7].tag) t24 = L[7]; else { or7 = 1; t24 = bh_chk(G[75], "项"); }
                            t25 = bh_add(K[344], t24);
                            t26 = bi_eprint(1, (Value[]){t25});
                            bh_line = 1607;
                            t24 = bi_exit(1, (Value[]){bh_int(2LL)});
                        }
                    }
                }
            }
        }
        bh_line = 1608;
        if (L[6].tag) t4 = L[6]; else { or6 = 1; t4 = bh_chk(G[19], "下标"); }
        t5 = bh_add(t4, bh_int(1LL));
        if (!L[6].tag && or6) bh_modify_outer("下标"); L[6] = t5;
    }
    bh_line = 1609;
    t0 = BH_FALSE;
    {
        if (L[1].tag) t1 = L[1]; else { or1 = 1; t1 = bh_chk(G[246], "输入文件"); }
        if (bh_cmp(0, t1, BH_NIL)) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1610;
        t2 = bi_eprint(1, (Value[]){K[345]});
        bh_line = 1611;
        t2 = bi_exit(1, (Value[]){bh_int(2LL)});
    } else {
    }
    bh_line = 1613;
    if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[246], "输入文件"); }
    t1 = bi_read_file(1, (Value[]){t0});
    if (!L[9].tag && or9) bh_modify_outer("源码"); L[9] = t1;
    bh_line = 1614;
    t0 = BH_FALSE;
    {
        if (L[9].tag) t1 = L[9]; else { or9 = 1; t1 = bh_chk(G[247], "源码"); }
        if (bh_cmp(0, t1, BH_NIL)) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1615;
        if (L[1].tag) t2 = L[1]; else { or1 = 1; t2 = bh_chk(G[246], "输入文件"); }
        t3 = bh_add(K[346], t2);
        t4 = bi_eprint(1, (Value[]){t3});
        bh_line = 1616;
        t2 = bi_exit(1, (Value[]){bh_int(1LL)});
    } else {
    }
    bh_line = 1617;
    t0 = BH_FALSE;
    {
        if (L[5].tag) t1 = L[5]; else { or5 = 1; t1 = bh_chk(G[248], "运行时文件"); }
        if (bh_cmp(0, t1, BH_NIL)) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1618;
        t2 = bi_runtime_source(0, NULL);
        if (!L[10].tag && or10) bh_modify_outer("运行时"); L[10] = t2;
    } else {
        bh_line = 1620;
        if (L[5].tag) t2 = L[5]; else { or5 = 1; t2 = bh_chk(G[248], "运行时文件"); }
        t3 = bi_read_file(1, (Value[]){t2});
        if (!L[10].tag && or10) bh_modify_outer("运行时"); L[10] = t3;
        bh_line = 1621;
        t2 = BH_FALSE;
        {
            if (L[10].tag) t3 = L[10]; else { or10 = 1; t3 = bh_chk(G[249], "运行时"); }
            if (bh_cmp(0, t3, BH_NIL)) {
                t2 = BH_TRUE;
            }
        }
        if (bh_truthy(t2)) {
            bh_line = 1622;
            if (L[5].tag) t4 = L[5]; else { or5 = 1; t4 = bh_chk(G[248], "运行时文件"); }
            t5 = bh_add(K[346], t4);
            t6 = bi_eprint(1, (Value[]){t5});
            bh_line = 1623;
            t4 = bi_exit(1, (Value[]){bh_int(1LL)});
        } else {
        }
    }
    bh_line = 1625;
    if (L[1].tag) t0 = L[1]; else { or1 = 1; t0 = bh_chk(G[246], "输入文件"); }
    t1 = bh_add(t0, K[347]);
    if (!L[11].tag && or11) bh_modify_outer("基名"); L[11] = t1;
    bh_line = 1626;
    {
        t1 = BH_FALSE;
        {
            if (L[1].tag) t2 = L[1]; else { or1 = 1; t2 = bh_chk(G[246], "输入文件"); }
            t3 = bi_len(1, (Value[]){t2});
            if (bh_cmp(3, t3, bh_int(3LL))) {
                t1 = BH_TRUE;
            }
        }
        if (bh_truthy(t1)) {
            t4 = BH_FALSE;
            {
                bh_line = 1626;
                if (L[1].tag) t5 = L[1]; else { or1 = 1; t5 = bh_chk(G[246], "输入文件"); }
                if (L[1].tag) t6 = L[1]; else { or1 = 1; t6 = bh_chk(G[246], "输入文件"); }
                t7 = bi_len(1, (Value[]){t6});
                t8 = bh_sub(t7, bh_int(3LL));
                t9 = bi_slice(3, (Value[]){t5, t8, bh_int(3LL)});
                if (bh_cmp(0, t9, K[348])) {
                    t4 = BH_TRUE;
                }
            }
            t0 = t4;
        } else {
            t0 = t1;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1627;
        if (L[1].tag) t10 = L[1]; else { or1 = 1; t10 = bh_chk(G[246], "输入文件"); }
        if (L[1].tag) t11 = L[1]; else { or1 = 1; t11 = bh_chk(G[246], "输入文件"); }
        t12 = bi_len(1, (Value[]){t11});
        t13 = bh_sub(t12, bh_int(3LL));
        t14 = bi_slice(3, (Value[]){t10, bh_int(0LL), t13});
        if (!L[11].tag && or11) bh_modify_outer("基名"); L[11] = t14;
    } else {
    }
    bh_line = 1629;
    t0 = bh_chk(G[242], "编译");
    if (L[9].tag) t1 = L[9]; else { or9 = 1; t1 = bh_chk(G[247], "源码"); }
    if (L[1].tag) t2 = L[1]; else { or1 = 1; t2 = bh_chk(G[246], "输入文件"); }
    if (L[10].tag) t3 = L[10]; else { or10 = 1; t3 = bh_chk(G[249], "运行时"); }
    t4 = bh_call(t0, 3, (Value[]){t1, t2, t3}, "编译");
    if (!L[12].tag && or12) bh_modify_outer("C代码"); L[12] = t4;
    bh_line = 1631;
    if (L[3].tag) t0 = L[3]; else { or3 = 1; t0 = bh_chk(G[250], "只要C"); }
    if (bh_truthy(t0)) {
        bh_line = 1632;
        t1 = BH_FALSE;
        {
            if (L[2].tag) t2 = L[2]; else { or2 = 1; t2 = bh_chk(G[251], "输出名"); }
            if (bh_cmp(0, t2, BH_NIL)) {
                t1 = BH_TRUE;
            }
        }
        if (bh_truthy(t1)) {
            bh_line = 1633;
            if (L[11].tag) t3 = L[11]; else { or11 = 1; t3 = bh_chk(G[252], "基名"); }
            t4 = bh_add(t3, K[349]);
            if (!L[2].tag && or2) bh_modify_outer("输出名"); L[2] = t4;
        } else {
        }
        bh_line = 1634;
        if (L[2].tag) t1 = L[2]; else { or2 = 1; t1 = bh_chk(G[251], "输出名"); }
        if (L[12].tag) t2 = L[12]; else { or12 = 1; t2 = bh_chk(G[253], "C代码"); }
        t3 = bi_write_file(2, (Value[]){t1, t2});
        t4 = bh_bool(!bh_truthy(t3));
        if (bh_truthy(t4)) {
            bh_line = 1635;
            if (L[2].tag) t5 = L[2]; else { or2 = 1; t5 = bh_chk(G[251], "输出名"); }
            t6 = bh_add(K[350], t5);
            t7 = bi_eprint(1, (Value[]){t6});
            bh_line = 1636;
            t5 = bi_exit(1, (Value[]){bh_int(1LL)});
        } else {
        }
        bh_line = 1637;
        BH_RETURN(fr, BH_NIL);
    } else {
    }
    bh_line = 1639;
    t0 = BH_FALSE;
    {
        if (L[2].tag) t1 = L[2]; else { or2 = 1; t1 = bh_chk(G[251], "输出名"); }
        if (bh_cmp(0, t1, BH_NIL)) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1640;
        if (L[11].tag) t2 = L[11]; else { or11 = 1; t2 = bh_chk(G[252], "基名"); }
        if (!L[2].tag && or2) bh_modify_outer("输出名"); L[2] = t2;
    } else {
    }
    bh_line = 1641;
    if (L[2].tag) t0 = L[2]; else { or2 = 1; t0 = bh_chk(G[251], "输出名"); }
    t1 = bh_add(t0, K[349]);
    if (!L[13].tag && or13) bh_modify_outer("C文件"); L[13] = t1;
    bh_line = 1642;
    if (L[13].tag) t0 = L[13]; else { or13 = 1; t0 = bh_chk(G[254], "C文件"); }
    if (L[12].tag) t1 = L[12]; else { or12 = 1; t1 = bh_chk(G[253], "C代码"); }
    t2 = bi_write_file(2, (Value[]){t0, t1});
    t3 = bh_bool(!bh_truthy(t2));
    if (bh_truthy(t3)) {
        bh_line = 1643;
        if (L[13].tag) t4 = L[13]; else { or13 = 1; t4 = bh_chk(G[254], "C文件"); }
        t5 = bh_add(K[350], t4);
        t6 = bi_eprint(1, (Value[]){t5});
        bh_line = 1644;
        t4 = bi_exit(1, (Value[]){bh_int(1LL)});
    } else {
    }
    bh_line = 1645;
    if (L[4].tag) t0 = L[4]; else { or4 = 1; t0 = bh_chk(G[255], "C编译器"); }
    t1 = bh_add(t0, K[351]);
    T[0] = t1;
    t2 = bh_chk(G[243], "引");
    if (L[2].tag) t3 = L[2]; else { or2 = 1; t3 = bh_chk(G[251], "输出名"); }
    t4 = bh_call(t2, 1, (Value[]){t3}, "引");
    t5 = bh_add(T[0], t4);
    t6 = bh_add(t5, K[54]);
    T[1] = t6;
    t7 = bh_chk(G[243], "引");
    if (L[13].tag) t8 = L[13]; else { or13 = 1; t8 = bh_chk(G[254], "C文件"); }
    t9 = bh_call(t7, 1, (Value[]){t8}, "引");
    t10 = bh_add(T[1], t9);
    t11 = bh_add(t10, K[352]);
    t12 = bi_system(1, (Value[]){t11});
    if (!L[14].tag && or14) bh_modify_outer("状态"); L[14] = t12;
    bh_line = 1646;
    t0 = BH_FALSE;
    {
        if (L[14].tag) t1 = L[14]; else { or14 = 1; t1 = bh_chk(G[256], "状态"); }
        if (bh_cmp(1, t1, bh_int(0LL))) {
            t0 = BH_TRUE;
        }
    }
    if (bh_truthy(t0)) {
        bh_line = 1647;
        if (L[4].tag) t2 = L[4]; else { or4 = 1; t2 = bh_chk(G[255], "C编译器"); }
        t3 = bh_add(K[353], t2);
        t4 = bh_add(t3, K[354]);
        if (L[13].tag) t5 = L[13]; else { or13 = 1; t5 = bh_chk(G[254], "C文件"); }
        t6 = bh_add(t4, t5);
        t7 = bh_add(t6, K[355]);
        t8 = bi_eprint(1, (Value[]){t7});
        bh_line = 1648;
        t2 = bi_exit(1, (Value[]){bh_int(1LL)});
    } else {
    }
    bh_line = 1649;
    t0 = bh_chk(G[243], "引");
    if (L[13].tag) t1 = L[13]; else { or13 = 1; t1 = bh_chk(G[254], "C文件"); }
    t2 = bh_call(t0, 1, (Value[]){t1}, "引");
    t3 = bh_add(K[356], t2);
    t4 = bi_system(1, (Value[]){t3});
    bh_line = 1650;
    if (L[2].tag) t0 = L[2]; else { or2 = 1; t0 = bh_chk(G[251], "输出名"); }
    t1 = bh_add(K[357], t0);
    t2 = bi_print(1, (Value[]){t1});
    BH_RETURN(fr, BH_NIL);
}

static void bh_module(void) {
    struct { BhRoots h; Value v[2]; } fr;
    Value *T;
    Value t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10;
    BH_ENTER(fr, 2);
    T = fr.v;
    bh_line = 19;
    t0 = bh_list_make(17, (Value[]){K[0], K[1], K[2], K[3], K[4], K[5], K[6], K[7], K[8], K[9], K[10], K[11], K[12], K[13], K[14], K[15], K[16]});
    G[0] = t0;
    bh_line = 22;
    t0 = bh_list_make(11, (Value[]){K[17], K[18], K[19], K[20], K[21], K[22], K[23], K[24], K[25], K[26], K[27]});
    G[1] = t0;
    bh_line = 23;
    t0 = bh_list_make(11, (Value[]){K[28], K[29], K[30], K[31], K[32], K[33], K[33], K[34], K[35], K[36], K[37]});
    G[2] = t0;
    bh_line = 26;
    t0 = bh_list_make(0, NULL);
    G[3] = t0;
    bh_line = 27;
    {
        t0 = bh_list_make(3, (Value[]){bh_int(4LL), bh_int(3LL), bh_int(2LL)});
        T[0] = bh_iter(t0);
        int64_t n1;
        for (n1 = 0; n1 < bh_list_len(T[0]); n1++) {
            BH_SAFEPOINT();
            bh_line = 27;
            G[4] = bh_list_at(T[0], n1);
            bh_line = 28;
            {
                t1 = bh_chk(G[0], "关键字表");
                t2 = bh_chk(G[1], "文字运算符");
                t3 = bh_add(t1, t2);
                T[1] = bh_iter(t3);
                int64_t n2;
                for (n2 = 0; n2 < bh_list_len(T[1]); n2++) {
                    BH_SAFEPOINT();
                    bh_line = 28;
                    G[5] = bh_list_at(T[1], n2);
                    bh_line = 29;
                    t4 = BH_FALSE;
                    {
                        t5 = bh_chk(G[5], "词");
                        t6 = bi_len(1, (Value[]){t5});
                        t7 = bh_chk(G[4], "字数");
                        if (bh_cmp(0, t6, t7)) {
                            t4 = BH_TRUE;
                        }
                    }
                    if (bh_truthy(t4)) {
                        bh_line = 30;
                        t8 = bh_chk(G[3], "长关键字");
                        t9 = bh_chk(G[5], "词");
                        t10 = bi_append(2, (Value[]){t8, t9});
                    } else {
                    }
                }
            }
        }
    }
    bh_line = 33;
    t0 = bi_chr(1, (Value[]){bh_int(12288LL)});
    t1 = bi_chr(1, (Value[]){bh_int(160LL)});
    t2 = bi_chr(1, (Value[]){bh_int(8203LL)});
    t3 = bi_chr(1, (Value[]){bh_int(65279LL)});
    t4 = bh_list_make(15, (Value[]){K[38], K[39], K[40], K[41], K[42], K[43], K[44], K[45], K[46], K[47], K[48], t0, t1, t2, t3});
    G[6] = t4;
    bh_line = 34;
    t0 = bh_list_make(15, (Value[]){K[49], K[50], K[51], K[52], K[30], K[29], K[53], K[28], K[32], K[31], K[34], K[54], K[54], K[54], K[54]});
    G[7] = t0;
    bh_line = 37;
    t0 = bi_chr(1, (Value[]){bh_int(12288LL)});
    t1 = bi_chr(1, (Value[]){bh_int(160LL)});
    t2 = bi_chr(1, (Value[]){bh_int(8203LL)});
    t3 = bi_chr(1, (Value[]){bh_int(65279LL)});
    t4 = bh_list_make(6, (Value[]){K[54], K[55], t0, t1, t2, t3});
    G[8] = t4;
    bh_line = 38;
    t0 = bh_list_make(6, (Value[]){bh_int(1LL), bh_int(4LL), bh_int(2LL), bh_int(1LL), bh_int(0LL), bh_int(0LL)});
    G[9] = t0;
    bh_line = 39;
    t0 = bi_chr(1, (Value[]){bh_int(13LL)});
    t1 = bi_chr(1, (Value[]){bh_int(11LL)});
    t2 = bi_chr(1, (Value[]){bh_int(12LL)});
    t3 = bi_chr(1, (Value[]){bh_int(12288LL)});
    t4 = bi_chr(1, (Value[]){bh_int(160LL)});
    t5 = bh_list_make(8, (Value[]){K[54], K[55], K[56], t0, t1, t2, t3, t4});
    G[10] = t5;
    bh_line = 42;
    t0 = bh_list_make(8, (Value[]){K[57], K[58], K[59], K[60], K[61], K[62], K[63], K[64]});
    G[11] = t0;
    bh_line = 43;
    t0 = bh_list_make(8, (Value[]){K[57], K[58], K[65], K[65], K[66], K[66], K[67], K[68]});
    G[12] = t0;
    bh_line = 45;
    t0 = bh_list_make(8, (Value[]){K[33], K[34], K[32], K[31], K[69], K[70], K[71], K[72]});
    G[13] = t0;
    bh_line = 46;
    G[14] = K[73];
    bh_line = 47;
    t0 = bh_list_make(5, (Value[]){K[74], K[69], K[70], K[71], K[72]});
    G[15] = t0;
    bh_line = 48;
    t0 = bh_list_make(6, (Value[]){K[33], K[34], K[30], K[29], K[32], K[31]});
    G[16] = t0;
    bh_line = 52;
    t0 = bh_list_make(28, (Value[]){bh_int(19968LL), bh_int(40959LL), bh_int(65LL), bh_int(90LL), bh_int(97LL), bh_int(122LL), bh_int(192LL), bh_int(214LL), bh_int(216LL), bh_int(246LL), bh_int(248LL), bh_int(591LL), bh_int(880LL), bh_int(1023LL), bh_int(1024LL), bh_int(1327LL), bh_int(12353LL), bh_int(12447LL), bh_int(12449LL), bh_int(12543LL), bh_int(13312LL), bh_int(19903LL), bh_int(44032LL), bh_int(55203LL), bh_int(63744LL), bh_int(64255LL), bh_int(131072LL), bh_int(201551LL)});
    G[17] = t0;
    bh_line = 54;
    t0 = bh_list_make(0, NULL);
    G[18] = t0;
    bh_line = 57;
    G[21] = bh_func(fn1, 1, "半角", NULL);
    bh_line = 68;
    G[22] = bh_func(fn2, 1, "是字母", NULL);
    bh_line = 80;
    G[23] = bh_func(fn3, 1, "是数字", NULL);
    bh_line = 84;
    G[24] = bh_func(fn4, 1, "名字字符", NULL);
    bh_line = 88;
    G[27] = bh_func(fn5, 1, "去空白", NULL);
    bh_line = 98;
    G[29] = bh_func(fn6, 3, "半角串", NULL);
    bh_line = 107;
    G[30] = bh_func(fn7, 1, "去前导零", NULL);
    bh_line = 114;
    G[32] = bh_func(fn8, 2, "语法出错", NULL);
    bh_line = 124;
    G[34] = bh_func(fn9, 1, "空格提示", NULL);
    bh_line = 133;
    G[55] = bh_func(fn10, 0, "词法分析", NULL);
    bh_line = 279;
    t0 = bh_list_make(0, NULL);
    G[56] = t0;
    bh_line = 280;
    G[57] = bh_int(0LL);
    bh_line = 281;
    G[58] = bh_int(0LL);
    bh_line = 282;
    G[59] = bh_int(0LL);
    bh_line = 285;
    G[60] = bh_func(fn11, 0, "看", NULL);
    bh_line = 289;
    G[62] = bh_func(fn12, 0, "前进", NULL);
    bh_line = 297;
    G[63] = bh_func(fn13, 2, "是这个", NULL);
    bh_line = 305;
    G[64] = bh_func(fn14, 2, "接受", NULL);
    bh_line = 311;
    G[66] = bh_func(fn15, 1, "说", NULL);
    bh_line = 321;
    G[70] = bh_func(fn16, 1, "语法错", NULL);
    bh_line = 333;
    G[72] = bh_func(fn17, 2, "期待", NULL);
    bh_line = 344;
    G[73] = bh_func(fn18, 1, "期待名字", NULL);
    bh_line = 353;
    G[74] = bh_func(fn19, 0, "语句结束了", NULL);
    bh_line = 357;
    G[76] = bh_func(fn20, 2, "并入", NULL);
    bh_line = 364;
    G[79] = bh_func(fn21, 0, "解析程序", NULL);
    bh_line = 371;
    G[78] = bh_func(fn22, 0, "解析语句", NULL);
    bh_line = 386;
    G[85] = bh_func(fn23, 0, "解析简单行", NULL);
    bh_line = 405;
    G[86] = bh_func(fn24, 0, "解析简单语句", NULL);
    bh_line = 442;
    G[94] = bh_func(fn25, 0, "解析块", NULL);
    bh_line = 459;
    G[96] = bh_func(fn26, 0, "解析循环体", NULL);
    bh_line = 467;
    G[80] = bh_func(fn27, 0, "解析如果", NULL);
    bh_line = 488;
    G[81] = bh_func(fn28, 0, "解析当", NULL);
    bh_line = 494;
    G[82] = bh_func(fn29, 0, "解析对于", NULL);
    bh_line = 503;
    G[83] = bh_func(fn30, 0, "解析重复", NULL);
    bh_line = 511;
    G[84] = bh_func(fn31, 0, "解析函数", NULL);
    bh_line = 537;
    G[90] = bh_func(fn32, 0, "解析表达式", NULL);
    bh_line = 541;
    G[105] = bh_func(fn33, 0, "解析或", NULL);
    bh_line = 549;
    G[106] = bh_func(fn34, 0, "解析且", NULL);
    bh_line = 557;
    G[108] = bh_func(fn35, 0, "解析非", NULL);
    bh_line = 564;
    G[109] = bh_func(fn36, 0, "解析比较", NULL);
    bh_line = 583;
    G[110] = bh_func(fn37, 0, "解析加减", NULL);
    bh_line = 591;
    G[113] = bh_func(fn38, 0, "解析乘除", NULL);
    bh_line = 599;
    G[114] = bh_func(fn39, 0, "解析一元", NULL);
    bh_line = 606;
    G[115] = bh_func(fn40, 0, "解析后缀", NULL);
    bh_line = 621;
    G[118] = bh_func(fn41, 1, "解析一串", NULL);
    bh_line = 632;
    G[116] = bh_func(fn42, 0, "解析基本", NULL);
    bh_line = 705;
    t0 = bh_list_make(0, NULL);
    G[122] = t0;
    bh_line = 706;
    t0 = bh_list_make(0, NULL);
    G[123] = t0;
    bh_line = 707;
    t0 = bh_list_make(0, NULL);
    G[124] = t0;
    bh_line = 708;
    G[125] = bh_int(0LL);
    bh_line = 709;
    G[126] = BH_NIL;
    bh_line = 710;
    t0 = bh_list_make(0, NULL);
    G[127] = t0;
    bh_line = 711;
    t0 = bh_list_make(0, NULL);
    G[128] = t0;
    bh_line = 712;
    t0 = bh_list_make(0, NULL);
    G[129] = t0;
    bh_line = 713;
    t0 = bh_list_make(0, NULL);
    G[130] = t0;
    bh_line = 716;
    G[131] = bh_int(0LL);
    bh_line = 717;
    G[132] = bh_int(1LL);
    bh_line = 718;
    G[133] = bh_int(2LL);
    bh_line = 719;
    G[134] = bh_int(3LL);
    bh_line = 720;
    G[135] = bh_int(4LL);
    bh_line = 721;
    G[136] = bh_int(5LL);
    bh_line = 722;
    G[137] = bh_int(6LL);
    bh_line = 723;
    G[138] = bh_int(7LL);
    bh_line = 724;
    G[139] = bh_int(8LL);
    bh_line = 725;
    G[140] = bh_int(9LL);
    bh_line = 726;
    G[141] = bh_int(10LL);
    bh_line = 727;
    G[142] = bh_int(11LL);
    bh_line = 728;
    G[143] = bh_int(12LL);
    bh_line = 729;
    G[144] = bh_int(13LL);
    bh_line = 730;
    G[145] = bh_int(14LL);
    bh_line = 733;
    G[146] = bh_func(fn43, 6, "新环境", NULL);
    bh_line = 737;
    G[147] = bh_func(fn44, 1, "出", NULL);
    bh_line = 741;
    G[148] = bh_func(fn45, 1, "开块", NULL);
    bh_line = 746;
    G[149] = bh_func(fn46, 0, "关块", NULL);
    bh_line = 752;
    G[150] = bh_func(fn47, 0, "否则块", NULL);
    bh_line = 759;
    G[152] = bh_func(fn48, 0, "新临时", NULL);
    bh_line = 768;
    G[153] = bh_func(fn49, 0, "根临时", NULL);
    bh_line = 780;
    G[154] = bh_func(fn50, 1, "不用保护", NULL);
    bh_line = 788;
    G[155] = bh_func(fn51, 1, "保根", NULL);
    bh_line = 797;
    G[156] = bh_func(fn52, 0, "新整型", NULL);
    bh_line = 802;
    G[157] = bh_func(fn53, 1, "定行", NULL);
    bh_line = 809;
    G[158] = bh_func(fn54, 1, "全局槽", NULL);
    bh_line = 817;
    G[159] = bh_func(fn55, 1, "常量槽", NULL);
    bh_line = 825;
    G[160] = bh_func(fn56, 1, "C文字", NULL);
    bh_line = 848;
    G[161] = bh_func(fn57, 1, "数字常量", NULL);
    bh_line = 856;
    G[162] = bh_func(fn58, 1, "至少一", NULL);
    bh_line = 862;
    G[163] = bh_func(fn59, 2, "记下", NULL);
    bh_line = 867;
    G[165] = bh_func(fn60, 4, "收集", NULL);
    bh_line = 893;
    G[171] = bh_func(fn61, 1, "读内置表", NULL);
    bh_line = 912;
    G[172] = bh_func(fn62, 2, "找函数", NULL);
    bh_line = 929;
    G[177] = bh_func(fn63, 2, "找全局赋值", NULL);
    bh_line = 944;
    G[181] = bh_func(fn64, 1, "直接内置", NULL);
    bh_line = 965;
    G[184] = bh_func(fn65, 1, "可能回收", NULL);
    bh_line = 994;
    G[183] = bh_func(fn66, 1, "摊平", NULL);
    bh_line = 1003;
    G[182] = bh_func(fn67, 1, "一串可能回收", NULL);
    bh_line = 1010;
    G[188] = bh_func(fn68, 1, "后面可能回收", NULL);
    bh_line = 1023;
    G[189] = bh_func(fn69, 1, "是叶子", NULL);
    bh_line = 1049;
    G[190] = bh_func(fn70, 1, "全局取值", NULL);
    bh_line = 1053;
    G[194] = bh_func(fn71, 1, "外层取值", NULL);
    bh_line = 1072;
    G[198] = bh_func(fn72, 1, "读变量", NULL);
    bh_line = 1094;
    G[200] = bh_func(fn73, 2, "写变量", NULL);
    bh_line = 1111;
    G[201] = bh_func(fn74, 1, "运算函数", NULL);
    bh_line = 1119;
    G[205] = bh_func(fn75, 1, "算一串", NULL);
    bh_line = 1133;
    G[206] = bh_func(fn76, 1, "数组式", NULL);
    bh_line = 1139;
    G[202] = bh_func(fn77, 1, "算", NULL);
    bh_line = 1221;
    G[208] = bh_func(fn78, 1, "算比较", NULL);
    bh_line = 1242;
    G[209] = bh_func(fn79, 1, "算逻辑", NULL);
    bh_line = 1265;
    G[213] = bh_func(fn80, 1, "编语句表", NULL);
    bh_line = 1270;
    G[212] = bh_func(fn81, 1, "编语句", NULL);
    bh_line = 1363;
    G[216] = bh_func(fn82, 1, "是范围调用", NULL);
    bh_line = 1371;
    G[214] = bh_func(fn83, 1, "编赋值", NULL);
    bh_line = 1398;
    G[215] = bh_func(fn84, 1, "编如果", NULL);
    bh_line = 1412;
    G[220] = bh_func(fn85, 1, "编函数", NULL);
    bh_line = 1471;
    G[231] = bh_func(fn86, 1, "添加C临时", NULL);
    bh_line = 1482;
    G[242] = bh_func(fn87, 3, "编译", NULL);
    bh_line = 1576;
    G[243] = bh_func(fn88, 1, "引", NULL);
    bh_line = 1581;
    G[257] = bh_func(fn89, 0, "主程序", NULL);
    bh_line = 1653;
    t0 = bh_chk(G[257], "主程序");
    t1 = bh_call(t0, 0, NULL, "主程序");
    bh_roots = fr.h.prev;
}

int main(int argc, char **argv) {
    char base;
    bh_init(argc, argv, &base);
    bh_src = bh_srclines;
    bh_src_n = 1654;
    bh_rt_lines = bh_rtsource;
    bh_rt_lines_n = 2345;
    bh_init_globals(G, bh_gnames, 258, K, 358);
    K[0] = bh_str_lit("函数");
    K[1] = bh_str_lit("返回");
    K[2] = bh_str_lit("全局");
    K[3] = bh_str_lit("如果");
    K[4] = bh_str_lit("否则");
    K[5] = bh_str_lit("否则如果");
    K[6] = bh_str_lit("当");
    K[7] = bh_str_lit("对于");
    K[8] = bh_str_lit("里的每个");
    K[9] = bh_str_lit("中的每个");
    K[10] = bh_str_lit("重复");
    K[11] = bh_str_lit("次");
    K[12] = bh_str_lit("跳出");
    K[13] = bh_str_lit("继续");
    K[14] = bh_str_lit("真");
    K[15] = bh_str_lit("假");
    K[16] = bh_str_lit("空");
    K[17] = bh_str_lit("除以");
    K[18] = bh_str_lit("大于");
    K[19] = bh_str_lit("小于");
    K[20] = bh_str_lit("大于等于");
    K[21] = bh_str_lit("小于等于");
    K[22] = bh_str_lit("等于");
    K[23] = bh_str_lit("是");
    K[24] = bh_str_lit("不等于");
    K[25] = bh_str_lit("并且");
    K[26] = bh_str_lit("或者");
    K[27] = bh_str_lit("不是");
    K[28] = bh_str_lit("/");
    K[29] = bh_str_lit(">");
    K[30] = bh_str_lit("<");
    K[31] = bh_str_lit(">=");
    K[32] = bh_str_lit("<=");
    K[33] = bh_str_lit("==");
    K[34] = bh_str_lit("!=");
    K[35] = bh_str_lit("and");
    K[36] = bh_str_lit("or");
    K[37] = bh_str_lit("not");
    K[38] = bh_str_lit("【");
    K[39] = bh_str_lit("】");
    K[40] = bh_str_lit("、");
    K[41] = bh_str_lit("。");
    K[42] = bh_str_lit("《");
    K[43] = bh_str_lit("》");
    K[44] = bh_str_lit("×");
    K[45] = bh_str_lit("÷");
    K[46] = bh_str_lit("≤");
    K[47] = bh_str_lit("≥");
    K[48] = bh_str_lit("≠");
    K[49] = bh_str_lit("[");
    K[50] = bh_str_lit("]");
    K[51] = bh_str_lit(",");
    K[52] = bh_str_lit(";");
    K[53] = bh_str_lit("*");
    K[54] = bh_str_lit(" ");
    K[55] = bh_str_lit("\t");
    K[56] = bh_str_lit("\n");
    K[57] = bh_str_lit("\"");
    K[58] = bh_str_lit("'");
    K[59] = bh_str_lit("“");
    K[60] = bh_str_lit("”");
    K[61] = bh_str_lit("‘");
    K[62] = bh_str_lit("’");
    K[63] = bh_str_lit("「");
    K[64] = bh_str_lit("『");
    K[65] = bh_str_lit("”“");
    K[66] = bh_str_lit("’‘");
    K[67] = bh_str_lit("」");
    K[68] = bh_str_lit("』");
    K[69] = bh_str_lit("+=");
    K[70] = bh_str_lit("-=");
    K[71] = bh_str_lit("*=");
    K[72] = bh_str_lit("/=");
    K[73] = bh_str_lit("+-*/%=<>!()[],:;");
    K[74] = bh_str_lit("=");
    K[75] = bh_str_lit("_");
    K[76] = bh_str_lit("0");
    K[77] = bh_str_lit("语法错误（第 ");
    K[78] = bh_str_lit(" 行）：");
    K[79] = bh_str_lit("");
    K[80] = bh_str_lit("  >  ");
    K[81] = bh_str_lit("\n提示：「");
    K[82] = bh_str_lit("」被当成了一个名字。如果其中的「");
    K[83] = bh_str_lit("」是单独的一个词，要在它前后加上空格。");
    K[84] = bh_str_lit("#");
    K[85] = bh_str_lit("INDENT");
    K[86] = bh_str_lit("DEDENT");
    K[87] = bh_str_lit("缩进对不齐：这一行开头的空格数和前面哪一层都对不上");
    K[88] = bh_str_lit("\\");
    K[89] = bh_str_lit("n");
    K[90] = bh_str_lit("t");
    K[91] = bh_str_lit("文字的引号没有关上（开头的引号是 ");
    K[92] = bh_str_lit(" ）");
    K[93] = bh_str_lit("STR");
    K[94] = bh_str_lit(".。．");
    K[95] = bh_str_lit(".");
    K[96] = bh_str_lit("NUM");
    K[97] = bh_str_lit("OP");
    K[98] = bh_str_lit("KW");
    K[99] = bh_str_lit("NAME");
    K[100] = bh_str_lit("不认识的符号「");
    K[101] = bh_str_lit("(");
    K[102] = bh_str_lit(")");
    K[103] = bh_str_lit("多了一个「");
    K[104] = bh_str_lit("」，前面没有和它配对的括号");
    K[105] = bh_str_lit("括号没有配对：「");
    K[106] = bh_str_lit("」不能用「");
    K[107] = bh_str_lit("」来关");
    K[108] = bh_str_lit("NEWLINE");
    K[109] = bh_str_lit("括号「");
    K[110] = bh_str_lit("」没有关上");
    K[111] = bh_str_lit("EOF");
    K[112] = bh_str_lit("换行");
    K[113] = bh_str_lit("缩进");
    K[114] = bh_str_lit("缩进的结束");
    K[115] = bh_str_lit("程序的结尾");
    K[116] = bh_str_lit(":");
    K[117] = bh_str_lit("冒号「：」");
    K[118] = bh_str_lit("左括号「（」");
    K[119] = bh_str_lit("右括号「）」");
    K[120] = bh_str_lit("右方括号「】」");
    K[121] = bh_str_lit("这里需要");
    K[122] = bh_str_lit("，但出现的是");
    K[123] = bh_str_lit("」是白话语言自己要用的词，不能拿来当");
    K[124] = bh_str_lit("这里需要一个");
    K[125] = bh_str_lit("这一行不该缩进：只有冒号下面的内容才需要缩进");
    K[126] = bh_str_lit("这一句到这里应该结束了，但后面还有");
    K[127] = bh_str_lit("expr");
    K[128] = bh_str_lit("name");
    K[129] = bh_str_lit("\n提示：如果是想调用「");
    K[130] = bh_str_lit("」，要加括号，例如：");
    K[131] = bh_str_lit("（");
    K[132] = bh_str_lit("）");
    K[133] = bh_str_lit("\n提示：顿号在这里相当于逗号。如果想做除法，请写 / 或者「除以」。");
    K[134] = bh_str_lit("「返回」只能写在函数里面");
    K[135] = bh_str_lit("return");
    K[136] = bh_str_lit("」只能写在循环里面");
    K[137] = bh_str_lit("break");
    K[138] = bh_str_lit("continue");
    K[139] = bh_str_lit("变量名");
    K[140] = bh_str_lit("global");
    K[141] = bh_str_lit("index");
    K[142] = bh_str_lit("」的左边只能是名字，或者 列表【下标】");
    K[143] = bh_str_lit("assign");
    K[144] = bh_str_lit("判断是否相等要写「==」「等于」或者「是」，单独一个「=」是赋值");
    K[145] = bh_str_lit("冒号下面的内容需要缩进（开头空四格）");
    K[146] = bh_str_lit("if");
    K[147] = bh_str_lit("while");
    K[148] = bh_str_lit("「对于」的写法是：对于 列表 里的每个 名字：");
    K[149] = bh_str_lit("foreach");
    K[150] = bh_str_lit("「重复」的写法是：重复 3 次：");
    K[151] = bh_str_lit("repeat");
    K[152] = bh_str_lit("函数名");
    K[153] = bh_str_lit("参数名");
    K[154] = bh_str_lit("参数名「");
    K[155] = bh_str_lit("」重复了");
    K[156] = bh_str_lit("def");
    K[157] = bh_str_lit("!");
    K[158] = bh_str_lit("compare");
    K[159] = bh_str_lit("+");
    K[160] = bh_str_lit("-");
    K[161] = bh_str_lit("binop");
    K[162] = bh_str_lit("%");
    K[163] = bh_str_lit("neg");
    K[164] = bh_str_lit("call");
    K[165] = bh_str_lit("num");
    K[166] = bh_str_lit("str");
    K[167] = bh_str_lit("const");
    K[168] = bh_str_lit("dict");
    K[169] = bh_str_lit("list");
    K[170] = bh_str_lit("」不能出现在这里（它是白话语言的关键字，也不能当名字用）");
    K[171] = bh_str_lit("这里缺了一个值，出现的是");
    K[172] = bh_str_lit("    ");
    K[173] = bh_str_lit("}");
    K[174] = bh_str_lit("} else {");
    K[175] = bh_str_lit("编译器内部错误：叶子函数里不该需要登记临时变量");
    K[176] = bh_str_lit("T[");
    K[177] = bh_str_lit("L[");
    K[178] = bh_str_lit("K[");
    K[179] = bh_str_lit("BH");
    K[180] = bh_str_lit("bh_int(");
    K[181] = bh_str_lit("bh_float(");
    K[182] = bh_str_lit(" = ");
    K[183] = bh_str_lit("bh_line = ");
    K[184] = bh_str_lit("\\\\");
    K[185] = bh_str_lit("\\\"");
    K[186] = bh_str_lit("\?");
    K[187] = bh_str_lit("\\\?");
    K[188] = bh_str_lit("\\n");
    K[189] = bh_str_lit("\\t");
    K[190] = bh_str_lit("数");
    K[191] = bh_str_lit("LL)");
    K[192] = bh_str_lit("bh_builtins[] = {");
    K[193] = bh_str_lit("{NULL, NULL");
    K[194] = bh_str_lit("{\"");
    K[195] = bh_str_lit("bh_chk(G[");
    K[196] = bh_str_lit("], ");
    K[197] = bh_str_lit("up");
    K[198] = bh_str_lit("->v[");
    K[199] = bh_str_lit("->up");
    K[200] = bh_str_lit(".tag \? ");
    K[201] = bh_str_lit(" : ");
    K[202] = bh_str_lit("if (gf");
    K[203] = bh_str_lit(") ");
    K[204] = bh_str_lit("; else ");
    K[205] = bh_str_lit("if (L[");
    K[206] = bh_str_lit("].tag) ");
    K[207] = bh_str_lit(" = L[");
    K[208] = bh_str_lit("]; else { or");
    K[209] = bh_str_lit(" = 1; ");
    K[210] = bh_str_lit("; }");
    K[211] = bh_str_lit(" = gf");
    K[212] = bh_str_lit(" \? ");
    K[213] = bh_str_lit("G[");
    K[214] = bh_str_lit("] = ");
    K[215] = bh_str_lit("if (!L[");
    K[216] = bh_str_lit("].tag && or");
    K[217] = bh_str_lit(") bh_modify_outer(");
    K[218] = bh_str_lit("); L[");
    K[219] = bh_str_lit(") G[");
    K[220] = bh_str_lit("; else { ");
    K[221] = bh_str_lit(" }");
    K[222] = bh_str_lit("bh_add");
    K[223] = bh_str_lit("bh_sub");
    K[224] = bh_str_lit("bh_mul");
    K[225] = bh_str_lit("bh_div");
    K[226] = bh_str_lit("bh_mod");
    K[227] = bh_str_lit("NULL");
    K[228] = bh_str_lit("(Value[]){");
    K[229] = bh_str_lit(", ");
    K[230] = bh_str_lit("文");
    K[231] = bh_str_lit("BH_TRUE");
    K[232] = bh_str_lit("BH_FALSE");
    K[233] = bh_str_lit("BH_NIL");
    K[234] = bh_str_lit(" = bh_list_make(");
    K[235] = bh_str_lit(");");
    K[236] = bh_str_lit(" = bh_dict_make(");
    K[237] = bh_str_lit(" = bh_bool(!bh_truthy(");
    K[238] = bh_str_lit("));");
    K[239] = bh_str_lit(" = bh_neg(");
    K[240] = bh_str_lit(" = bh_index(");
    K[241] = bh_str_lit(" = bh_call(");
    K[242] = bh_str_lit(" = BH_FALSE;");
    K[243] = bh_str_lit("{");
    K[244] = bh_str_lit("if (bh_cmp(");
    K[245] = bh_str_lit(")) {");
    K[246] = bh_str_lit(" = BH_TRUE;");
    K[247] = bh_str_lit("if (bh_truthy(");
    K[248] = bh_str_lit("for (;;) {");
    K[249] = bh_str_lit("BH_SAFEPOINT();");
    K[250] = bh_str_lit("if (!bh_truthy(");
    K[251] = bh_str_lit(")) break;");
    K[252] = bh_str_lit("r");
    K[253] = bh_str_lit("BhRange ");
    K[254] = bh_str_lit("int64_t ");
    K[255] = bh_str_lit(" = bh_range_begin(&");
    K[256] = bh_str_lit("for (");
    K[257] = bh_str_lit(" = 0; ");
    K[258] = bh_str_lit(" < ");
    K[259] = bh_str_lit(".count; ");
    K[260] = bh_str_lit("++) {");
    K[261] = bh_str_lit("bh_range_at(&");
    K[262] = bh_str_lit(" = bh_iter(");
    K[263] = bh_str_lit(" < bh_list_len(");
    K[264] = bh_str_lit("); ");
    K[265] = bh_str_lit("bh_list_at(");
    K[266] = bh_str_lit(" = bh_count(");
    K[267] = bh_str_lit("), ");
    K[268] = bh_str_lit("; ");
    K[269] = bh_str_lit("return ");
    K[270] = bh_str_lit("BH_RETURN(fr, ");
    K[271] = bh_str_lit("break;");
    K[272] = bh_str_lit("continue;");
    K[273] = bh_str_lit("gf");
    K[274] = bh_str_lit(" = 1;");
    K[275] = bh_str_lit("范围");
    K[276] = bh_str_lit("bh_set_index(");
    K[277] = bh_str_lit("static Value fn");
    K[278] = bh_str_lit("(Frame *up, Value *a) {    /* 函数 ");
    K[279] = bh_str_lit("，第 ");
    K[280] = bh_str_lit(" 行 */");
    K[281] = bh_str_lit("    Value L[");
    K[282] = bh_str_lit("] = {{0}};");
    K[283] = bh_str_lit("    struct { BhRoots h; Value v[");
    K[284] = bh_str_lit("]; } fr;");
    K[285] = bh_str_lit("    Frame *self; Value *L, *T;");
    K[286] = bh_str_lit("    BH_ENTER(fr, ");
    K[287] = bh_str_lit("    self = bh_frame_new(up, ");
    K[288] = bh_str_lit("); fr.v[0] = bh_obj(T_FRAME, self); L = self->v; T = fr.v + 1;");
    K[289] = bh_str_lit("    Value *L, *T;");
    K[290] = bh_str_lit("    L = fr.v; T = fr.v + ");
    K[291] = bh_str_lit("    fr.v[");
    K[292] = bh_str_lit("] = bh_obj(T_FRAME, up);");
    K[293] = bh_str_lit("    L[");
    K[294] = bh_str_lit("] = a[");
    K[295] = bh_str_lit("];    /* ");
    K[296] = bh_str_lit(" */");
    K[297] = bh_str_lit("    char or");
    K[298] = bh_str_lit(" = 0;");
    K[299] = bh_str_lit("    char gf");
    K[300] = bh_str_lit("    return BH_NIL;");
    K[301] = bh_str_lit("    BH_SAFEPOINT();");
    K[302] = bh_str_lit("    BH_RETURN(fr, BH_NIL);");
    K[303] = bh_str_lit("self");
    K[304] = bh_str_lit("bh_func(fn");
    K[305] = bh_str_lit("    Value ");
    K[306] = bh_str_lit("*/");
    K[307] = bh_str_lit("程序");
    K[308] = bh_str_lit("/* 由白话编译器生成，源文件：");
    K[309] = bh_str_lit("static Value G[");
    K[310] = bh_str_lit("];");
    K[311] = bh_str_lit("static const char *bh_gnames[] = {");
    K[312] = bh_str_lit("};");
    K[313] = bh_str_lit("static Value K[");
    K[314] = bh_str_lit("static const char *bh_srclines[] = {");
    K[315] = bh_str_lit("    NULL};");
    K[316] = bh_str_lit("运行时源码");
    K[317] = bh_str_lit("static const char *bh_rtsource[] = {");
    K[318] = bh_str_lit("(Frame *up, Value *a);");
    K[319] = bh_str_lit("static void bh_module(void) {");
    K[320] = bh_str_lit("    Value *T;");
    K[321] = bh_str_lit("    T = fr.v;");
    K[322] = bh_str_lit("    bh_roots = fr.h.prev;");
    K[323] = bh_str_lit("int main(int argc, char **argv) {");
    K[324] = bh_str_lit("    char base;");
    K[325] = bh_str_lit("    bh_init(argc, argv, &base);");
    K[326] = bh_str_lit("    bh_src = bh_srclines;");
    K[327] = bh_str_lit("    bh_src_n = ");
    K[328] = bh_str_lit("    bh_rt_lines = bh_rtsource;");
    K[329] = bh_str_lit("    bh_rt_lines_n = ");
    K[330] = bh_str_lit("    bh_init_globals(G, bh_gnames, ");
    K[331] = bh_str_lit(", K, ");
    K[332] = bh_str_lit("bh_str_lit(");
    K[333] = bh_str_lit("bh_big_lit(");
    K[334] = bh_str_lit("    K[");
    K[335] = bh_str_lit("    bh_module();");
    K[336] = bh_str_lit("    fflush(stdout);");
    K[337] = bh_str_lit("    return 0;");
    K[338] = bh_str_lit("'\\''");
    K[339] = bh_str_lit("cc");
    K[340] = bh_str_lit("-o");
    K[341] = bh_str_lit("--cc");
    K[342] = bh_str_lit("--runtime");
    K[343] = bh_str_lit("--emit-c");
    K[344] = bh_str_lit("看不懂的参数：");
    K[345] = bh_str_lit("用法：baihuac 程序.bh [-o 输出名] [--emit-c] [--cc C编译器]");
    K[346] = bh_str_lit("打不开文件：");
    K[347] = bh_str_lit(".out");
    K[348] = bh_str_lit(".bh");
    K[349] = bh_str_lit(".c");
    K[350] = bh_str_lit("写不了文件：");
    K[351] = bh_str_lit(" -O2 -w -o ");
    K[352] = bh_str_lit(" -lm");
    K[353] = bh_str_lit("调用 C 编译器（");
    K[354] = bh_str_lit("）没有成功。生成的 C 代码留在 ");
    K[355] = bh_str_lit(" 里。");
    K[356] = bh_str_lit("rm -f ");
    K[357] = bh_str_lit("编译好了：");
    bh_module();
    fflush(stdout);
    return 0;
}
