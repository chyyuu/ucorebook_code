#include <string.h>
#include <x86.h>

/* *
 * strlen - calculate the length of the string @s, not including
 * the terminating '\0' character.
 * @s:      the input string
 *
 * The strlen() function returns the length of string @s.
 * */
size_t
strlen(const char *s) {
    size_t cnt = 0;
    while (*s ++ != '\0') {
        cnt ++;
    }
    return cnt;
}

/* *
 * strnlen - calculate the length of the string @s, not including
 * the terminating '\0' char acter, but at most @len.
 * @s:      the input string
 * @len:    the max-length that function will scan
 *
 * Note that, this function looks only at the first @len characters
 * at @s, and never beyond @s + @len.
 *
 * The return value is strlen(s), if that is less than @len, or
 * @len if there is no '\0' character among the first @len characters
 * pointed by @s.
 * */
size_t
strnlen(const char *s, size_t len) {
    size_t cnt = 0;
    while (cnt < len && *s ++ != '\0') {
        cnt ++;
    }
    return cnt;
}

/* *
 * memset - sets the first @n bytes of the memory area pointed by @s
 * to the specified value @c.
 * @s:      pointer the the memory area to fill
 * @c:      value to set
 * @n:      number of bytes to be set to the value
 *
 * The memset() function returns @s.
 * */
void *
memset(void *s, char c, size_t n) {
#ifdef __HAVE_ARCH_MEMSET
    return __memset(s, c, n);
#else
    char *p = s;
    while (n -- > 0) {
        *p ++ = c;
    }
    return s;
#endif /* __HAVE_ARCH_MEMSET */
}

/* *
 * memmove - copies the values of @n bytes from the location pointed by @src to
 * the memory area pointed by @dst. @src and @dst are allowed to overlap.
 * @dst     pointer to the destination array where the content is to be copied
 * @src     pointer to the source of data to by copied
 * @n:      number of bytes to copy
 *
 * The memmove() function returns @dst.
 * */
void *
memmove(void *dst, const void *src, size_t n) {
#ifdef __HAVE_ARCH_MEMMOVE
    return __memmove(dst, src, n);
#else
    const char *s = src;
    char *d = dst;
    if (s < d && s + n > d) {
        s += n, d += n;
        while (n -- > 0) {
            *-- d = *-- s;
        }
    } else {
        while (n -- > 0) {
            *d ++ = *s ++;
        }
    }
    return dst;
#endif /* __HAVE_ARCH_MEMMOVE */
}

/* *
 * memcpy - copies the value of @n bytes from the location pointed by @src to
 * the memory area pointed by @dst.
 * @dst     pointer to the destination array where the content is to be copied
 * @src     pointer to the source of data to by copied
 * @n:      number of bytes to copy
 *
 * The memcpy() returns @dst.
 *
 * Note that, the function does not check any terminating null character in @src,
 * it always copies exactly @n bytes. To avoid overflows, the size of arrays pointed
 * by both @src and @dst, should be at least @n bytes, and should not overlap
 * (for overlapping memory area, memmove is a safer approach).
 * */
void *
memcpy(void *dst, const void *src, size_t n) {
#ifdef __HAVE_ARCH_MEMCPY
    return __memcpy(dst, src, n);
#else
    const char *s = src;
    char *d = dst;
    while (n -- > 0) {
        *d ++ = *s ++;
    }
    return dst;
#endif /* __HAVE_ARCH_MEMCPY */
}

