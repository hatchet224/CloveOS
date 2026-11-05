#include "supervisor/elf_dependency.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb"); long length; unsigned char *bytes;
    assert(f != NULL && fseek(f, 0, SEEK_END) == 0);
    length = ftell(f); assert(length > 0 && length < 67108864L);
    *size = (size_t)length; rewind(f); bytes = malloc(*size); assert(bytes != NULL);
    assert(fread(bytes, 1U, *size, f) == *size && fclose(f) == 0); return bytes;
}
int main(int argc, char **argv)
{
    unsigned char *input, *expected, *copy; size_t size, expected_size;
    assert(argc == 3); input = read_file(argv[1], &size); expected = read_file(argv[2], &expected_size);
    assert(size == expected_size); copy = malloc(size); assert(copy != NULL); memcpy(copy, input, size);
    assert(!sc_elf_remove_unused_tmenu(NULL, size));
    assert(!sc_elf_remove_unused_tmenu(copy, 32U));
    copy[18] = 62U; assert(!sc_elf_remove_unused_tmenu(copy, size)); memcpy(copy, input, size);
    assert(sc_elf_remove_unused_tmenu(copy, size)); assert(memcmp(copy, expected, size) == 0);
    assert(!sc_elf_remove_unused_tmenu(copy, size));
    free(copy); free(input); free(expected);
    puts("ELF_DEPENDENCY_PASS exact patched ELF, no repeat, rejected null/truncated/wrong architecture");
    return 0;
}
