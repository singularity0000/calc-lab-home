// Naim Khuduzade 251ADB072
// https://github.com/naimkhuduzade/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: K&R "The C Programming Language" (Chapter 5) for the parser structure, 
// and cppreference.com/cplusplus.com for standard library functions and file handling.

#define _XOPEN_SOURCE 700

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define FIRST_NAME "naim"
#define LAST_NAME "khuduzade"
#define STUDENT_ID "251ADB072"

// We use long long for integers and double for floats. C integer division
// already truncates toward zero (e.g., 7/2 = 3), which is exactly what the lab asks for.
typedef struct {
    int is_float;
    long long i;
    double f;
} Value;

typedef enum {
    T_NUM, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_POW,
    T_LPAREN, T_RPAREN, T_EOL, T_BAD
} TokenKind;

typedef struct {
    int kind;
    size_t start; // 0-based offset in the whole file buffer.
    Value val;
} Token;

typedef struct {
    Value v;
    size_t op_off;
    size_t start;
} PowItem;

typedef struct {
    char *buf;
    size_t pos;
    size_t end;
    Token tok;
    size_t err; 
    PowItem *stack;
    size_t sp, cap;
} Parser;

typedef struct {
    char *data;
    size_t len, cap;
} OutBuf;
static double as_double(Value v) { return v.is_float ? v.f : (double)v.i; }
static int is_zero(Value v) { return v.is_float ? v.f == 0.0 : v.i == 0; }

// We only save the very first error we hit. If we keep parsing after an error,
// the parser state is broken, and we might overwrite the true error position.
static void fail(Parser *p, size_t offset) {
    if (p->err == 0) p->err = offset + 1;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

// Deciding between int and float is simple: we look for a '.'. 
// If there's a dot, we use double to keep the fraction. If not, we stick to long long.
// If a dot is followed by a non-digit (like "5."), it's an error right at the dot.
static int read_number(Parser *p, size_t i) {
    char *b = p->buf;
    size_t j = i;
    unsigned long long acc = 0;
    
    while (j < p->end && is_digit(b[j])) {
        acc = acc * 10u + (unsigned)(b[j] - '0');
        j++;
    }
    
    if (j < p->end && b[j] == '.') {
        if (j + 1 >= p->end || !is_digit(b[j + 1])) {
            p->tok.kind = T_BAD;
            p->tok.start = j;
            p->pos = j + 1;
            return 1;
        }
        j++;
        while (j < p->end && is_digit(b[j])) j++;
        
        char saved = b[j];
        b[j] = '\0';
        p->tok.val.is_float = 1;
        p->tok.val.f = strtod(b + i, NULL);
        b[j] = saved;
    } else {
        p->tok.val.is_float = 0;
        p->tok.val.i = (long long)acc;
    }
    
    p->tok.kind = T_NUM;
    p->tok.start = i;
    p->pos = j;
    return 0;
}

// How I track positions: I don't use line/column numbers. Instead, I put the whole
// file into one buffer and just use the character's 1-based index. Since every character
// (including '\n') is in the buffer, counting is automatic. If an error happens,
// I just report the offset of the token + 1.
static void next(Parser *p) {
    const char *b = p->buf;
    size_t i = p->pos;
    
    while (i < p->end && (b[i] == ' ' || b[i] == '\t')) i++;

    p->tok.start = i;
    if (i >= p->end) {
        p->tok.kind = T_EOL;
        p->pos = i;
        return;
    }

    switch (b[i]) {
        case '+': p->tok.kind = T_PLUS; p->pos = i + 1; return;
        case '-': p->tok.kind = T_MINUS; p->pos = i + 1; return;
        case '/': p->tok.kind = T_SLASH; p->pos = i + 1; return;
        case '(': p->tok.kind = T_LPAREN; p->pos = i + 1; return;
        case ')': p->tok.kind = T_RPAREN; p->pos = i + 1; return;
        case '*':
            if (i + 1 < p->end && b[i + 1] == '*') {
                p->tok.kind = T_POW;
                p->pos = i + 2;
            } else {
                p->tok.kind = T_STAR;
                p->pos = i + 1;
            }
            return;
    }

    if (is_digit(b[i])) {
        read_number(p, i);
        return;
    }
    
    p->tok.kind = T_BAD;
    p->pos = i + 1;
}
// If float math gives us infinity or NaN, the error is reported at the operator
// because the operator caused the bad value.
static Value float_result(Parser *p, double r, size_t op_off) {
    Value v = {1, 0, r};
    if (!isfinite(r)) fail(p, op_off);
    return v;
}

static Value binary(Parser *p, int op, Value a, Value b, size_t op_off) {
    // If both sides are integers, the result stays an integer. 
    // As soon as one side is a float, we convert everything to double.
    if (!a.is_float && !b.is_float) {
        unsigned long long x = (unsigned long long)a.i;
        unsigned long long y = (unsigned long long)b.i;
        Value v = {0, 0, 0.0};
        switch (op) {
            case T_PLUS: v.i = (long long)(x + y); break;
            case T_MINUS: v.i = (long long)(x - y); break;
            case T_STAR: v.i = (long long)(x * y); break;
            default: v.i = a.i / b.i; break;
        }
        return v;
    }
    double x = as_double(a), y = as_double(b), r;
    switch (op) {
        case T_PLUS: r = x + y; break;
        case T_MINUS: r = x - y; break;
        case T_STAR: r = x * y; break;
        default: r = x / y; break;
    }
    return float_result(p, r, op_off);
}

static Value power_of(Parser *p, Value base, Value ex, size_t op_off, size_t ex_start) {
    if (!base.is_float && !ex.is_float) {
        // Negative exponents for integers are not allowed. Error points to the exponent's start.
        if (ex.i < 0) {
            fail(p, ex_start);
            return base;
        }
        unsigned long long r = 1, b = (unsigned long long)base.i;
        long long e = ex.i;
        while (e > 0) {
            if (e & 1) r *= b;
            b *= b;
            e >>= 1;
        }
        Value v = {0, (long long)r, 0.0};
        return v;
    }
    return float_result(p, pow(as_double(base), as_double(ex)), op_off);
}

static Value parse_expr(Parser *p);

// This is where error positions happen. If we expect an operand and find something else,
// that token IS the problem. 
// - "7 + * 3" -> error at the '*' (two operators in a row)
// - "3 *"     -> error at '\n' (missing operand at end of line)
// - "(1 + 2"  -> error at '\n' (missing closing parenthesis)
static Value parse_primary(Parser *p) {
    Value v = {0, 0, 0.0};
    if (p->tok.kind == T_NUM) {
        v = p->tok.val;
        next(p);
        return v;
    }
    if (p->tok.kind == T_LPAREN) {
        next(p);
        v = parse_expr(p);
        if (p->err) return v;
        if (p->tok.kind != T_RPAREN) {
            fail(p, p->tok.start);
            return v;
        }
        next(p);
        return v;
    }
    fail(p, p->tok.start);
    return v;
}

static int push_item(Parser *p, Value v, size_t op_off, size_t start) {
    if (p->sp == p->cap) {
        size_t ncap = p->cap ? p->cap * 2 : 64;
        PowItem *ns = realloc(p->stack, ncap * sizeof *ns);
        if (!ns) return -1;
        p->stack = ns;
        p->cap = ncap;
    }
    p->stack[p->sp].v = v;
    p->stack[p->sp].op_off = op_off;
    p->stack[p->sp].start = start;
    p->sp++;
    return 0;
}

// How precedence works: The parser goes down from expr -> term -> power -> primary.
// Because '+' only combines terms, and a term already handled all '*' and '/',
// "3 + 2 * 5" naturally groups as "3 + (2 * 5)".
//
// Why '**' is right-associative: The grammar rule puts "power" on the right side,
// so the parser folds from right to left: 2 ** 3 ** 2 = 2 ** (3 ** 2) = 512.
// I used a stack and a loop here instead of raw recursion so a long chain like
// "1 ** 1 ** 1 ..." won't crash the program with a stack overflow.
static Value parse_power(Parser *p) {
    size_t base = p->sp;
    Value v = parse_primary(p);
    
    if (p->err || p->tok.kind != T_POW) return v;

    if (push_item(p, v, 0, 0)) {
        fail(p, p->tok.start);
        return v;
    }
    while (p->tok.kind == T_POW) {
        size_t op_off = p->tok.start;
        next(p);
        size_t start = p->tok.start;
        Value e = parse_primary(p);
        if (p->err || push_item(p, e, op_off, start)) {
            fail(p, start);
            p->sp = base;
            return e;
        }
    }

    Value acc = p->stack[p->sp - 1].v;
    for (size_t k = p->sp - 1; k > base && !p->err; k--) {
        acc = power_of(p, p->stack[k - 1].v, acc, p->stack[k].op_off, p->stack[k].start);
    }
    p->sp = base;
    return acc;
}

// * and / are left-associative. If division by zero happens, the error is reported 
// at the start of the right operand (the divisor), since that's the wrong value.
static Value parse_term(Parser *p) {
    Value v = parse_power(p);
    while (!p->err && (p->tok.kind == T_STAR || p->tok.kind == T_SLASH)) {
        int op = p->tok.kind;
        size_t op_off = p->tok.start;
        next(p);
        size_t right_start = p->tok.start;
        Value r = parse_power(p);
        
        if (p->err) break;
        if (op == T_SLASH && is_zero(r)) {
            fail(p, right_start);
            break;
        }
        v = binary(p, op, v, r, op_off);
    }
    return v;
}

// + and - are left-associative.
static Value parse_expr(Parser *p) {
    Value v = parse_term(p);
    while (!p->err && (p->tok.kind == T_PLUS || p->tok.kind == T_MINUS)) {
        int op = p->tok.kind;
        size_t op_off = p->tok.start;
        next(p);
        Value r = parse_term(p);
        if (p->err) break;
        v = binary(p, op, v, r, op_off);
    }
    return v;
}
static int out_append(OutBuf *o, const char *s, size_t n) {
    if (o->len + n > o->cap) {
        size_t ncap = o->cap ? o->cap : 4096;
        while (o->len + n > ncap) ncap *= 2;
        char *nd = realloc(o->data, ncap);
        if (!nd) return -1;
        o->data = nd;
        o->cap = ncap;
    }
    memcpy(o->data + o->len, s, n);
    o->len += n;
    return 0;
}

// How I handle large files: Instead of reading the file line by line (which is slow),
// I just use fread to dump the whole file into one big memory buffer. 
// Then, the tokenizer just moves an index through this buffer. No string copying happens 
// at all for the lines. For the output, I also use a dynamic buffer that grows, 
// so we don't write to the disk a million times.
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fclose(f); return NULL; }
    
    for (;;) {
        if (len + 1 >= cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); fclose(f); return NULL; }
            buf = nb;
            cap *= 2;
        }
        size_t got = fread(buf + len, 1, cap - 1 - len, f);
        len += got;
        if (got == 0) break;
    }
    
    int bad = ferror(f);
    fclose(f);
    if (bad) { free(buf); return NULL; }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

static int evaluate(char *buf, size_t len, OutBuf *out) {
    Parser p;
    memset(&p, 0, sizeof p);
    p.buf = buf;

    size_t line = 0;
    while (line < len) {
        const char *nl = memchr(buf + line, '\n', len - line);
        size_t end = nl ? (size_t)(nl - buf) : len;

        size_t i = line;
        while (i < end && (buf[i] == ' ' || buf[i] == '\t')) i++;
        
        // Skip empty lines and comments. The characters still count for positions.
        if (i < end && buf[i] != '#') {
            p.pos = line;
            p.end = end;
            next(&p);
            Value v = parse_expr(&p);
            
            // If there's an extra ')' or something weird after the expression, it's an error.
            if (!p.err && p.tok.kind != T_EOL) {
                fail(&p, p.tok.start);
            }
            if (p.err) break;

            char tmp[64];
            int n = v.is_float ? snprintf(tmp, sizeof tmp, "%.15g\n", v.f)
                               : snprintf(tmp, sizeof tmp, "%lld\n", v.i);
            if (out_append(out, tmp, (size_t)n)) {
                free(p.stack);
                return -1;
            }
        }
        line = end + 1;
    }

    // If there's any error, clear the buffer and write ONLY the first error.
    if (p.err) {
        char tmp[64];
        int n = snprintf(tmp, sizeof tmp, "ERROR:%zu\n", p.err);
        out->len = 0;
        if (out_append(out, tmp, (size_t)n)) {
            free(p.stack);
            return -1;
        }
    }
    free(p.stack);
    return 0;
}
static char *join_path(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *s = malloc(la + lb + 2);
    if (!s) return NULL;
    memcpy(s, a, la);
    size_t k = la;
    if (la > 0 && a[la - 1] != '/') s[k++] = '/';
    memcpy(s + k, b, lb + 1);
    return s;
}

static char *base_name(const char *path, int strip_txt) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;
    size_t start = len;
    while (start > 0 && path[start - 1] != '/') start--;
    size_t n = len - start;

    if ((n == 1 && path[start] == '.') || (n == 2 && path[start] == '.' && path[start + 1] == '.')) {
        char *real = realpath(path, NULL);
        if (real) {
            char *r = base_name(real, strip_txt);
            free(real);
            return r;
        }
    }

    char *s = malloc(n + 1);
    if (!s) return NULL;
    memcpy(s, path + start, n);
    s[n] = '\0';
    if (strip_txt && n > 4 && strcmp(s + n - 4, ".txt") == 0) s[n - 4] = '\0';
    return s;
}

static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        fprintf(stderr, "calc: %s exists and is not a directory\n", path);
        return -1;
    }
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "calc: cannot create %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int process_file(const char *in_path, const char *out_dir) {
    size_t len = 0;
    char *buf = read_file(in_path, &len);
    if (!buf) {
        fprintf(stderr, "calc: cannot read %s: %s\n", in_path, strerror(errno));
        return 1;
    }

    OutBuf out = {NULL, 0, 0};
    if (evaluate(buf, len, &out)) {
        fprintf(stderr, "calc: out of memory on %s\n", in_path);
        free(buf);
        free(out.data);
        return 1;
    }
    free(buf);

    char *base = base_name(in_path, 1);
    size_t fl = (base ? strlen(base) : 0) + sizeof FIRST_NAME + sizeof LAST_NAME + sizeof STUDENT_ID + 8;
    char *fname = malloc(fl);
    char *out_path = NULL;
    
    if (base && fname) {
        snprintf(fname, fl, "%s_%s_%s_%s.txt", base, FIRST_NAME, LAST_NAME, STUDENT_ID);
        out_path = join_path(out_dir, fname);
    }
    free(base);
    free(fname);
    
    if (!out_path) {
        fprintf(stderr, "calc: out of memory\n");
        free(out.data);
        return 1;
    }

    int rc = 0;
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "calc: cannot write %s: %s\n", out_path, strerror(errno));
        rc = 1;
    } else {
        if (out.len && fwrite(out.data, 1, out.len, f) != out.len) rc = 1;
        if (fclose(f) != 0) rc = 1;
        if (rc) fprintf(stderr, "calc: error writing %s\n", out_path);
    }
    
    free(out_path);
    free(out.data);
    return rc;
}

static char *default_out_dir(const char *input, int strip_txt) {
    char *base = base_name(input, strip_txt);
    if (!base) return NULL;
    size_t n = strlen(base) + sizeof FIRST_NAME + sizeof STUDENT_ID + 4;
    char *s = malloc(n);
    if (s) snprintf(s, n, "%s_%s_%s", base, FIRST_NAME, STUDENT_ID);
    free(base);
    return s;
}

static int ends_with_txt(const char *name) {
    size_t n = strlen(name);
    return n > 4 && strcmp(name + n - 4, ".txt") == 0;
}

static int run_dir(const char *dir, const char *out_dir) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "calc: cannot open folder %s: %s\n", dir, strerror(errno));
        return 1;
    }
    int rc = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || !ends_with_txt(e->d_name)) continue;
        
        char *path = join_path(dir, e->d_name);
        if (!path) { rc = 1; break; }
        
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            rc |= process_file(path, out_dir);
        }
        free(path);
    }
    closedir(d);
    return rc;
}

static void usage(void) {
    fprintf(stderr, "usage: calc [-d DIR | --dir DIR] [-o OUTDIR | --output-dir OUTDIR] [input.txt]\n");
}

static int take_opt(int argc, char **argv, int *i, const char *s, const char *l, const char **val) {
    const char *a = argv[*i];
    size_t ll = strlen(l);
    if (strncmp(a, l, ll) == 0 && a[ll] == '=') {
        *val = a + ll + 1;
        return 1;
    }
    if (strcmp(a, s) != 0 && strcmp(a, l) != 0) return 0;
    if (*i + 1 >= argc) return -1;
    *val = argv[++*i];
    return 1;
}

int main(int argc, char **argv) {
    const char *dir = NULL, *out_dir = NULL, *input = NULL;

    for (int i = 1; i < argc; i++) {
        int r = take_opt(argc, argv, &i, "-d", "--dir", &dir);
        if (r == 0) r = take_opt(argc, argv, &i, "-o", "--output-dir", &out_dir);
        if (r < 0) {
            fprintf(stderr, "calc: option %s needs a value\n", argv[i]);
            usage();
            return 2;
        }
        if (r > 0) continue;
        
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "calc: unknown option %s\n", argv[i]);
            usage();
            return 2;
        }
        if (input) {
            fprintf(stderr, "calc: only one input file is allowed\n");
            usage();
            return 2;
        }
        input = argv[i];
    }

    if (!dir && !input) {
        fprintf(stderr, "calc: no input file or folder given\n");
        usage();
        return 2;
    }
    if (dir && input) {
        fprintf(stderr, "calc: give either -d DIR or an input file\n");
        usage();
        return 2;
    }

    struct stat st;
    const char *src = dir ? dir : input;
    if (stat(src, &st) != 0) {
        fprintf(stderr, "calc: %s: %s\n", src, strerror(errno));
        return 1;
    }
    if (dir && !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "calc: %s is not a folder\n", dir);
        return 1;
    }
    if (!dir && !S_ISREG(st.st_mode)) {
        fprintf(stderr, "calc: %s is not a regular file\n", input);
        return 1;
    }

    char *def = NULL;
    if (!out_dir) {
        def = default_out_dir(src, dir == NULL);
        if (!def) {
            fprintf(stderr, "calc: out of memory\n");
            return 1;
        }
        out_dir = def;
    }

    int rc = 1;
    if (ensure_dir(out_dir) == 0) {
        rc = dir ? run_dir(dir, out_dir) : process_file(input, out_dir);
    }
    free(def);
    
    return rc ? 1 : 0;
}