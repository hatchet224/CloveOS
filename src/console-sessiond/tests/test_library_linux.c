#include "sessiond/library.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #x, errno);              \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static void directory(const char *name) { CHECK(mkdir(name, 0700) == 0); }
static void file(const char *name, const char *text) {
    FILE *f = fopen(name, "wb");
    CHECK(f != NULL);
    CHECK(fwrite(text, 1U, strlen(text), f) == strlen(text));
    CHECK(fclose(f) == 0);
}
int main(void) {
    char primary[] = "/tmp/moss-library-primary-XXXXXX",
         secondary[] = "/tmp/moss-library-secondary-XXXXXX";
    struct cs_library *library = calloc(1U, sizeof(*library));
    struct cs_library_card cards[2];
    size_t i;
    uint64_t revision;
    char identity[65];
    CHECK(library != NULL && mkdtemp(primary) != NULL && mkdtemp(secondary) != NULL);
    CHECK(chdir(primary) == 0);
    directory("Emu");
    directory("Emu/GB");
    directory("Roms");
    directory("Roms/GB");
    directory("Roms/GB/Imgs");
    directory("roms");
    directory("roms/gbc");
    directory("roms/gbc/Hacks");
    directory("roms/gbc/Hacks/TooDeep");
    file("Roms/GB/Blue.gb", "test ROM fixture");
    file("Roms/GB/Hidden.gb", "test ROM fixture");
    file("Roms/GB/Imgs/Blue.png", "image path fixture only");
    file("roms/gbc/Crystal.GBC", "test ROM fixture");
    file("roms/gbc/Hacks/Test.gbc", "test ROM fixture");
    file("roms/gbc/Hacks/TooDeep/Hidden.gbc", "test ROM fixture");
    file("roms/gbc/readme.txt", "ignore");
    CHECK(symlink("/etc/passwd", "roms/gbc/unsafe.gbc") == 0);
    CHECK(symlink("/tmp", "roms/gbc/escape") == 0);
    file("Emu/GB/config.json",
         "{\"label\":\"Game "
         "Boy\",\"rompath\":\"../../Roms/GB\",\"imgpath\":\"../../Roms/GB/Imgs\",\"gamelist\":\"../"
         "../Roms/GB/"
         "miyoogamelist.xml\",\"extlist\":\"gb|gbc\",\"launch\":\"this-must-never-execute.sh\"}");
    file("Roms/GB/miyoogamelist.xml",
         "<gameList><game><path>./Blue.gb</path><name>Pokemon "
         "Blue</name><image>./Imgs/Blue.png</image></game></gameList>");
    CHECK(chdir(secondary) == 0);
    directory("Roms");
    directory("Roms/GB");
    file("Roms/GB/Blue.gb", "second card ROM fixture");
    cards[0] = (struct cs_library_card){primary, "primary-card", false};
    cards[1] = (struct cs_library_card){secondary, "secondary-card", false};
    CHECK(cs_library_scan(cards, 2U, library) && library->count == 4U);
    revision = library->revision;
    identity[0] = '\0';
    for (i = 0U; i < library->count; ++i) {
        CHECK(strstr(library->games[i].content, "Hidden") == NULL &&
              strstr(library->games[i].content, "unsafe") == NULL);
        if (strcmp(library->games[i].title, "Pokemon Blue") == 0) {
            strcpy(identity, library->games[i].id);
            CHECK(library->games[i].artwork[0] == '/');
            CHECK(library->games[i].card == 0U);
        }
    }
    CHECK(identity[0] != '\0');
    CHECK(cs_library_scan(cards, 2U, library) && library->revision == revision);
    cards[1].root = "/tmp/moss-library-card-that-is-absent";
    CHECK(cs_library_scan(cards, 2U, library) && library->count == 3U &&
          library->missing_cards == 1U);
    CHECK(chdir(primary) == 0);
    file("Roms/GB/miyoogamelist.xml", "<gameList/>");
    CHECK(cs_library_scan(cards, 1U, library) &&
          library->count == 2U); /* valid empty metadata stays empty */
    file("Roms/GB/miyoogamelist.xml", "<!DOCTYPE bad><gameList/>");
    CHECK(cs_library_scan(cards, 1U, library) && library->count == 4U &&
          library->malformed_metadata == 1U);
    file("Emu/GB/config.json",
         "{\"label\":\"Escape\",\"rompath\":\"../../../../etc\",\"extlist\":\"gb\"}");
    CHECK(cs_library_scan(cards, 1U, library) && library->malformed_metadata >= 1U);
    cards[0].require_mount = true;
    CHECK(cs_library_scan(cards, 1U, library) && library->count == 0U &&
          library->missing_cards == 1U);
    CHECK(cs_library_open_absolute("/tmp/../etc/passwd", false) < 0);
    puts("PASS: Onion config/XML, lowercase aliases, dual/missing card, stable IDs, metadata "
         "fallback, symlink/traversal containment");
    free(library);
    return 0;
}
