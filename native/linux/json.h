#ifndef DESKTOP_JSON_H
#define DESKTOP_JSON_H
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct { const char *start; const char *end; } Json;
static const char *skip_space(const char *p) { while (*p && isspace((unsigned char)*p)) p++; return p; }
static const char *skip_value(const char *p, int depth) {
    if (depth > 16) return NULL;
    p = skip_space(p);
    if (*p == '"') {
        p++;
        while (*p) {
            if ((unsigned char)*p < 32) return NULL;
            if (*p == '"') return p + 1;
            if (*p == '\\') { p++; if (!*p) return NULL; if (*p == 'u') { for (int i = 0; i < 4; i++) { p++; if (!isxdigit((unsigned char)*p)) return NULL; } } }
            p++;
        }
        return NULL;
    }
    if (*p == '{' || *p == '[') {
        char close = *p++ == '{' ? '}' : ']';
        p = skip_space(p);
        if (*p == close) return p + 1;
        for (;;) {
            if (close == '}') { if (*p != '"') return NULL; p = skip_value(p, depth + 1); if (!p) return NULL; p = skip_space(p); if (*p++ != ':') return NULL; }
            p = skip_value(p, depth + 1); if (!p) return NULL; p = skip_space(p);
            if (*p == close) return p + 1;
            if (*p++ != ',') return NULL;
            p = skip_space(p);
        }
    }
    if (!strncmp(p, "true", 4)) return p + 4;
    if (!strncmp(p, "false", 5)) return p + 5;
    if (!strncmp(p, "null", 4)) return p + 4;
    char *end;
    strtod(p, &end);
    return end == p ? NULL : end;
}
static Json member(Json object, const char *key) {
    const char *p = skip_space(object.start);
    if (*p++ != '{') return (Json){"", ""};
    while (*p && p < object.end) {
        p = skip_space(p);
        if (*p == '}') break;
        const char *name = p;
        const char *end = skip_value(p, 0);
        if (!end) break;
        p = skip_space(end);
        if (*p++ != ':') break;
        p = skip_space(p);
        const char *value_end = skip_value(p, 0);
        if (!value_end) break;
        if ((size_t)(end - name) == strlen(key) + 2 && !strncmp(name + 1, key, strlen(key))) return (Json){p, value_end};
        p = skip_space(value_end);
        if (*p++ != ',') break;
    }
    return (Json){"", ""};
}
static void text(Json value, char *output, size_t size) {
    size_t used = 0;
    const char *p = value.start;
    if (*p++ != '"') { output[0] = 0; return; }
    while (p < value.end - 1 && used + 1 < size) {
        unsigned char ch = (unsigned char)*p++;
        if (ch == '\\' && p < value.end - 1) {
            ch = (unsigned char)*p++;
            if (ch == 'n') ch = '\n'; else if (ch == 'r') ch = '\r'; else if (ch == 't') ch = '\t';
            else if (ch == 'u' && p + 4 <= value.end - 1) {
                char hex[5]; memcpy(hex, p, 4); hex[4] = 0; p += 4;
                unsigned long code = strtoul(hex, NULL, 16);
                if (code < 128) ch = (unsigned char)code;
                else if (code < 2048 && used + 2 < size) { output[used++] = (char)(0xc0 | (code >> 6)); ch = (unsigned char)(0x80 | (code & 63)); }
                else if (used + 3 < size) { output[used++] = (char)(0xe0 | (code >> 12)); output[used++] = (char)(0x80 | ((code >> 6) & 63)); ch = (unsigned char)(0x80 | (code & 63)); }
                else break;
            }
        }
        output[used++] = (char)ch;
    }
    output[used] = 0;
}
static int number(Json object, const char *key, int fallback) {
    Json value = member(object, key);
    if (value.start == value.end) return fallback;
    char *end; long parsed = strtol(value.start, &end, 10);
    if (end != value.end || parsed < INT32_MIN || parsed > INT32_MAX) return fallback;
    return (int)parsed;
}
static void quoted(const char *value, size_t limit) {
    putchar('"');
    for (size_t i = 0; value && value[i] && i < limit; i++) {
        unsigned char ch = (unsigned char)value[i];
        if (ch == '"' || ch == '\\') { putchar('\\'); putchar(ch); }
        else if (ch < 32) printf("\\u%04x", ch);
        else putchar(ch);
    }
    putchar('"');
}
#endif
