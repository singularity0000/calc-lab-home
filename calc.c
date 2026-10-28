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