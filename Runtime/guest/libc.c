/*
 * C library basics for the guest, compiled through the big-endian pipeline like game code.
 * Everything is weak: a game's own library (MSL in the decomp) wins where it builds.
 */
#include <stddef.h>

#define WEAK __attribute__((weak))

WEAK void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

WEAK void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s)
    {
        while (n--) *d++ = *s++;
    }
    else
    {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

WEAK void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

WEAK int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
    {
        if (*x != *y) return *x - *y;
    }
    return 0;
}

WEAK void *memchr(const void *p, int c, size_t n)
{
    const unsigned char *s = p;
    for (; n; n--, s++)
    {
        if (*s == (unsigned char)c) return (void *)s;
    }
    return NULL;
}

WEAK size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

WEAK char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++) != 0) {}
    return r;
}

WEAK char *strncpy(char *d, const char *s, size_t n)
{
    char *r = d;
    for (; n && *s; n--) *d++ = *s++;
    for (; n; n--) *d++ = 0;
    return r;
}

WEAK char *strcat(char *d, const char *s)
{
    strcpy(d + strlen(d), s);
    return d;
}

WEAK char *strncat(char *d, const char *s, size_t n)
{
    char *p = d + strlen(d);
    for (; n && *s; n--) *p++ = *s++;
    *p = 0;
    return d;
}

WEAK int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

WEAK int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++)
    {
        if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b;
    }
    return 0;
}

WEAK char *strchr(const char *s, int c)
{
    for (;; s++)
    {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

WEAK char *strrchr(const char *s, int c)
{
    const char *r = NULL;
    for (;; s++)
    {
        if (*s == (char)c) r = s;
        if (!*s) return (char *)r;
    }
}

WEAK char *strstr(const char *h, const char *n)
{
    size_t len = strlen(n);
    for (; *h; h++)
    {
        if (strncmp(h, n, len) == 0) return (char *)h;
    }
    return len ? NULL : (char *)h;
}
