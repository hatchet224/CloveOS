#include "supervisor/zip.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb"); long length; unsigned char *bytes;
    assert(f != NULL && fseek(f, 0, SEEK_END) == 0);
    length = ftell(f); assert(length > 0 && length <= 67108864L);
    *size = (size_t)length; rewind(f); bytes = malloc(*size); assert(bytes != NULL);
    assert(fread(bytes, 1U, *size, f) == *size && fclose(f) == 0); return bytes;
}
int main(int argc, char **argv)
{
    unsigned char *archive, *nested = NULL, *core = NULL, *expected;
    size_t size, nested_size = 0U, core_size = 0U, expected_size;
    assert(argc == 5); archive = read_file(argv[1], &size); expected = read_file(argv[4], &expected_size);
    if (strcmp(argv[2], "-") != 0) {
        assert(sc_zip_read(archive, size, argv[2], 50331648U, &nested, &nested_size));
        free(archive); archive = nested; size = nested_size;
    }
    assert(!sc_zip_read(archive, size, argv[3], 4U, &core, &core_size));
    assert(!sc_zip_read(archive, size - 1U, argv[3], 33554432U, &core, &core_size));
    assert(sc_zip_read(archive, size, argv[3], 33554432U, &core, &core_size));
    assert(core_size == expected_size && memcmp(core, expected, core_size) == 0);
    free(core); free(expected); free(archive);
    printf("PUBLIC_ZIP_EXACT_PASS %s\n", argv[3]); return 0;
}
