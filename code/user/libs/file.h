#ifndef __USER_LIBS_FILE_H__
#define __USER_LIBS_FILE_H__

#include <types.h>

struct stat;

int open(const char *path, uint32_t open_flags);
int close(int fd);
int read(int fd, void *base, size_t len);
int write(int fd, void *base, size_t len);
int fstat(int fd, struct stat *stat);
int dup(int fd);
int dup2(int fd1, int fd2);

void print_stat(const char *name, int fd, struct stat *stat);

#endif /* !__USER_LIBS_FILE_H__ */

