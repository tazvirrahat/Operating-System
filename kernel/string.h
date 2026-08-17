/* string.h — the handful of C library functions we need.
 *
 * There is no libc here. gcc also assumes memset/memcpy exist regardless of
 * -fno-builtin, because it emits calls to them for struct assignment and
 * large initialisers, so those two must be provided or the link fails.
 */
#ifndef STRING_H
#define STRING_H

#include <stddef.h>
#include <stdint.h>

void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);

/* Like memcpy but safe when the regions overlap, which memcpy is not.
 * Scrolling a console shifts a buffer onto itself and needs this. */
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);

/* Parse a decimal or 0x-prefixed hex integer. Returns 0 on malformed input;
 * if end is non-NULL it receives a pointer to the first unconsumed character,
 * which lets callers distinguish "parsed 0" from "parsed nothing". */
uint32_t strtoul(const char *s, const char **end);

#endif /* STRING_H */
