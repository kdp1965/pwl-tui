// Byte-wise replacements for newlib's word-at-a-time string routines.
//
// This toolchain's newlib believes misaligned word loads are cheap and
// uses them freely inside strcmp/strcpy/strlen and friends.  On TinyQV
// they are not merely slow - they return WRONG DATA, observed twice on
// hardware:
//
//   - strcmp("[6~", "[6~") returned -6912 for byte-identical strings
//     (the TUI's PGUP/PGDN decode; tcurses_vt100.c has the write-up)
//   - strcpy from an odd source address scrambled tab-completion
//     candidates, folding in bytes from beyond the source's NUL
//     ("cfg" arrived as "c.gfs_-")
//
// Linking these byte-loop versions from the project objects shadows
// the libc ones everywhere (the linker only reaches into libc.a for
// symbols still undefined), which retires the whole bug class instead
// of patching call sites as they bite.  mem* are left with newlib:
// its memcpy/memmove align explicitly before going word-wise.

#include <string.h>
#include <ctype.h>

size_t strlen(const char *s)
{
    const char *p = s;

    while (*p != 0)
        p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a != 0 && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n > 0 && *a != 0 && *a == *b) {
        a++;
        b++;
        n--;
    }
    return n == 0 ? 0 : (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;

    while ((*d++ = *src++) != 0)
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;

    while (n > 0 && *src != 0) {
        *d++ = *src++;
        n--;
    }
    while (n > 0) {
        *d++ = 0;
        n--;
    }
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst;

    while (*d != 0)
        d++;
    while ((*d++ = *src++) != 0)
        ;
    return dst;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (*s == 0)
            return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *found = NULL;

    for (;; s++) {
        if (*s == (char)c)
            found = s;
        if (*s == 0)
            return (char *)found;
    }
}

int strcasecmp(const char *a, const char *b)
{
    while (*a != 0 &&
           tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n > 0 && *a != 0 &&
           tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        a++;
        b++;
        n--;
    }
    return n == 0 ? 0
                  : tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
