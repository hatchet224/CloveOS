#ifndef SESSIOND_RETROARCH_COMPAT_H
#define SESSIOND_RETROARCH_COMPAT_H

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CS_RETROARCH_COMPAT_PATH "/etc/console/retroarch-compatible.sha256"
/* r30 and the control-only successor have identical state serialization.
 * First authenticate the executable bytes. Only this root-owned release
 * record can map the new digest to the existing checkpoint identity. */
static inline void cs_retroarch_checkpoint_hash(int fd, char hash[65])
{
    static const char previous[] =
        "2d138567b9a4ac11b2b29fa766b568fee3592b26cba169a1099fa3934b914fe0";
    struct stat st;
    char record[65];
    if (fd < 0) return;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0U &&
        st.st_nlink == 1U && (st.st_mode & 0222U) == 0U && st.st_size == sizeof(record) &&
        read(fd, record, sizeof(record)) == sizeof(record) && record[64] == '\n' &&
        memcmp(record, hash, 64U) == 0)
        memcpy(hash, previous, sizeof(previous));
    (void)close(fd);
}
#endif
