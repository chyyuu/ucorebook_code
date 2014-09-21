#ifndef __KERN_FS_SYSFILE_H__
#define __KERN_FS_SYSFILE_H__

#include <types.h>

struct stat;

int sysfile_open(const char *path, uint32_t open_flags);
int sysfile_close(int fd);
int sysfile_read(int fd, void *base, size_t len);
int sysfile_write(int fd, void *base, size_t len);
int sysfile_fstat(int fd, struct stat *stat);
int sysfile_dup(int fd1, int fd2);

#endif /* !__KERN_FS_SYSFILE_H__ */

