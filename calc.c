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