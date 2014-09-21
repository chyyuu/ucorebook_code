#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/select.h>
#include <fcntl.h>
#include <errno.h>

typedef void *(*thread_func)(void *data);

pthread_t thread_qemu, thread_gdb;
int sockfd_qemu = -1, sockfd_gdb = -1;

int isfailed = 0;

void
sleeps(float time) {
    if (time > 0) {
        int sec = (int)time;
        int usec = (time - sec) * 1000000;
        if (sec > 0) {
            sleep(sec);
        }
        if (usec > 0) {
            usleep(usec);
        }
    }
}

void
do_connect_qemu(void *data) {
    sleeps(0.2);
    if (isfailed) {
        return ;
    }
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9875);
    if ((addr.sin_addr.s_addr = inet_addr("127.0.0.1")) == INADDR_NONE) {
        isfailed = 1;
        return ;
    }
    bzero(&(addr.sin_zero), sizeof(addr.sin_zero));
    int size = sizeof(struct sockaddr);
    sockfd_qemu = socket(AF_INET, SOCK_STREAM, 0);
    int i;
    for (i = 0; i < 120; i ++) {
        if (connect(sockfd_qemu, (struct sockaddr *)&addr, size) == 0) {
            unsigned char magic;
            if (read(sockfd_qemu, &magic, 1) != 1 || magic != 0xEF) {
                isfailed = 1;
            }
            return ;
        }
        if (isfailed != 0) {
            return ;
        }
        sleeps(0.5);
    }
}

void
do_connect_gdb(void) {
    sleeps(0.2);
    if (isfailed) {
        return ;
    }
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd == -1) {
        goto connect_gdb_error;
    }
    int on = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (void *)&on, sizeof(on)) < 0) {
        goto connect_gdb_error;
    }
    struct timeval tv_timeout;
    tv_timeout.tv_sec = 0;
    tv_timeout.tv_usec = 500000;
    if (setsockopt(listenfd, SOL_SOCKET, SO_RCVTIMEO, &tv_timeout, sizeof(tv_timeout)) < 0) {
        goto connect_gdb_error;
    }
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(9876);
    int size = sizeof(struct sockaddr_in);
    if (bind(listenfd, (struct sockaddr *)&addr, size) < 0 || listen(listenfd, 1) < 0) {
        goto connect_gdb_error;
    }
    int i;
    for (i = 0; i < 120; i ++) {
        if ((sockfd_gdb = accept(listenfd, NULL, NULL)) != -1 || isfailed != 0) {
            close(listenfd);
            return ;
        }
    }
connect_gdb_error:
    if (listenfd != -1) {
        close(listenfd);
    }
    isfailed = 1;
}

void
closefd(void) {
    if (sockfd_qemu != -1) {
        close(sockfd_qemu);
    }
    if (sockfd_gdb != -1) {
        close(sockfd_gdb);
    }
}

void
build_conns(void) {
    pthread_t thread_qemu, thread_gdb;
    if (pthread_create(&thread_qemu, NULL, (thread_func)do_connect_qemu, NULL) != 0 ||
            pthread_create(&thread_gdb, NULL, (thread_func)do_connect_gdb, NULL) != 0) {
        isfailed = 1;
    }
    if (isfailed == 0) {
        pthread_join(thread_qemu, NULL);
        pthread_join(thread_gdb, NULL);
    }
    if (isfailed != 0) {
        closefd();
    }
}

int
read_write(int fd_from, int fd_to) {
    char buf[1024];
    int len;
    if ((len = read(fd_from, buf, sizeof(buf))) == -1 || len == 0) {
        return -1;
    }
    int p = 0;
    while (p < len) {
        int r;
        if ((r = write(fd_to, &buf[p], len - p)) == -1) {
            return -1;
        }
        p += r;
    }
    return len;
}

void
work(void) {
    struct timeval tv_timeout;
    tv_timeout.tv_sec = 1;
    tv_timeout.tv_usec = 0;
    int maxfd = sockfd_qemu;
    if (maxfd < sockfd_gdb) {
        maxfd = sockfd_gdb;
    }
    fd_set readfds;
    while (1) {
        FD_ZERO(&readfds);
        FD_SET(sockfd_qemu, &readfds);
        FD_SET(sockfd_gdb, &readfds);
        int r;
        if ((r = select(maxfd + 1, &readfds, NULL, NULL, &tv_timeout)) == -1) {
            /* some error happens */
            return ;
        }
        if (r == 0) {
            continue ;
        }
        if (FD_ISSET(sockfd_qemu, &readfds)) {
            if (read_write(sockfd_qemu, sockfd_gdb) < 0) {
                return ;
            }
        }
        if (FD_ISSET(sockfd_gdb, &readfds)) {
            if (read_write(sockfd_gdb, sockfd_qemu) < 0) {
                return ;
            }
        }
    }
}

int
main(void) {
    build_conns();
    if (isfailed != 0) {
        return -1;
    }
    work();
    closefd();
    return 0;
}

