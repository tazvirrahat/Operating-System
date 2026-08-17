#include "string.h"

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;

    while (n--)
        *d++ = (uint8_t)c;

    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    while (n--)
        *d++ = *s++;

    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (d == s || n == 0)
        return dst;

    /* When the destination overlaps and sits after the source, copying
     * forwards would overwrite bytes still to be read. Going backwards in
     * that case keeps the unread portion intact. */
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }

    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;

    while (n--) {
        if (*x != *y)
            return *x - *y;
        x++;
        y++;
    }

    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;

    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }

    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }

    if (n == 0)
        return 0;

    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *out = dst;

    while ((*dst++ = *src++))
        ;

    return out;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;

    while (i < n && src[i]) {
        dst[i] = src[i];
        i++;
    }

    while (i < n)
        dst[i++] = '\0';

    return dst;
}

uint32_t strtoul(const char *s, const char **end)
{
    uint32_t value = 0;
    uint32_t base  = 10;

    while (*s == ' ' || *s == '\t')
        s++;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }

    while (*s) {
        uint32_t digit;

        if (*s >= '0' && *s <= '9')
            digit = (uint32_t)(*s - '0');
        else if (*s >= 'a' && *s <= 'f')
            digit = (uint32_t)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F')
            digit = (uint32_t)(*s - 'A' + 10);
        else
            break;

        if (digit >= base)
            break;

        value = value * base + digit;
        s++;
    }

    if (end)
        *end = s;

    return value;
}
