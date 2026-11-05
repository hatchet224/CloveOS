#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
static unsigned int file_calls, volume_calls;
static int file_error, volume_error;
static int fixture_fsync(int fd) { (void)fd; ++file_calls; errno=file_error; return file_error ? -1 : 0; }
static int fixture_syncfs(int fd) { (void)fd; ++volume_calls; errno=volume_error; return volume_error ? -1 : 0; }
#define fsync fixture_fsync
#define syncfs fixture_syncfs
#include "sessiond/durability.h"
int main(void)
{
    assert(cs_flush_directory(9, true, true) == 0 && file_calls == 1U && volume_calls == 1U);
    assert(cs_flush_directory(9, false, false) == 0 && volume_calls == 1U);
    file_error=EINVAL; assert(cs_flush_directory(9, true, true) == 0 && volume_calls == 2U);
    file_error=EIO; assert(cs_flush_directory(9, true, true) < 0 && volume_calls == 2U);
    file_error=0; volume_error=EIO; assert(cs_flush_directory(9, true, true) < 0 && volume_calls == 3U);
    assert(cs_flush_directory(9, true, false) < 0 && volume_calls == 3U);
    puts("DURABILITY_PASS FAT always flushes volume even after successful directory fsync; ext4 unchanged; I/O errors and unqualified FAT rejected");
    return 0;
}
