#include "launcher/theme.h"
#include "sessiond/library.h"
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
bool cl_theme_read(struct cl_theme *theme, const char *path) {
    int fd;
    struct stat st;
    char *text = NULL;
    size_t used = 0U;
    bool ok = false;
    if (theme == NULL || path == NULL)
        return false;
    fd = cs_library_open_absolute(path, false);
    if (fd < 0)
        return false;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 65536)
        goto done;
    text = malloc((size_t)st.st_size + 1U);
    if (text == NULL)
        goto done;
    while (used < (size_t)st.st_size) {
        ssize_t n = read(fd, text + used, (size_t)st.st_size - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            goto done;
        used += (size_t)n;
    }
    if (close(fd) != 0) {
        fd = -1;
        goto done;
    }
    fd = -1;
    ok = cl_theme_apply(theme, text, used);
done:
    free(text);
    if (fd >= 0 && close(fd) != 0)
        ok = false;
    return ok;
}
