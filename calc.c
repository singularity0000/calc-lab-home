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