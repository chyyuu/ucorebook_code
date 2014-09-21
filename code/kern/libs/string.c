#include <string.h>
#include <slab.h>

char *
strdup(const char *src) {
    char *dst;
    size_t len = strlen(src);
    if ((dst = kmalloc(len + 1)) != NULL) {
        memcpy(dst, src, len);
        dst[len] = '\0';
    }
    return dst;
}

