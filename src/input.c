#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ibnetlens.h"

static int read_all(int fd, ib_input *in)
{
    size_t cap = 0, len = 0;
    char *buf = NULL;
    for (;;) {
        ibt_grow((void **)&buf, &cap, 1, len + 65536);
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            return -1;
        }
        if (r == 0)
            break;
        len += (size_t)r;
    }
    in->heap = buf;
    in->data = buf ? buf : "";
    in->len = len;
    return 0;
}

int ib_input_open(ib_input *in, const char *path)
{
    memset(in, 0, sizeof *in);
    bool is_stdin = strcmp(path, "-") == 0;
    int fd = is_stdin ? STDIN_FILENO : open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    struct stat st;
    int rc = -1;
    if (!is_stdin && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (m != MAP_FAILED) {
            in->map = m;
            in->data = m;
            in->len = (size_t)st.st_size;
            rc = 0;
        }
    }
    if (rc != 0)
        rc = read_all(fd, in);
    if (!is_stdin)
        close(fd);
    return rc;
}

void ib_input_close(ib_input *in)
{
    if (in->map)
        munmap(in->map, in->len);
    free(in->heap);
    memset(in, 0, sizeof *in);
}
