#include "launcher/history.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void entry(struct cl_entry *e, const char *path, const char *system, char id)
{
    strcpy(e->content_path, path); strcpy(e->system, system);
    memset(e->identity.game, id, 64U); e->identity.game[64] = '\0';
    strcpy(e->title, path);
}
int main(void)
{
    struct cl_entry *e = calloc(8U, sizeof(*e)); assert(e != NULL);
    entry(&e[0], "/Roms/DC/Shenmue (USA) (Disc 3).chd", "DC", 'c');
    entry(&e[1], "/Roms/DC/Shenmue (USA) (Disc 1).chd", "DC", 'a');
    entry(&e[2], "/Roms/DC/Shenmue (USA) (Disc 2).chd", "DC", 'b');
    entry(&e[3], "/Roms/DC/Shenmue II - Disc 2 (English Dub).cdi", "DC", 'e');
    entry(&e[4], "/Roms/DC/Shenmue II - Disc 1 (English Dub).cdi", "DC", 'd');
    entry(&e[5], "/Roms/DC/Shenmue (Japan) (Disc 1).chd", "DC", 'f');
    entry(&e[6], "/Roms/DC/other/Shenmue (USA) (Disc 1).chd", "DC", 'g');
    entry(&e[7], "/Roms/GB/Disc 1.gb", "GB", 'h');
    struct cl_entry *original = malloc(8U * sizeof(*e)); assert(original != NULL);
    memcpy(original, e, 8U * sizeof(*e));
    assert(cl_group_discs(e, 8U, NULL) == 5U);
    assert(e[0].identity.game[0] == 'a' && e[0].disc_count == 3U && e[0].disc_index == 1U);
    assert(e[0].next_disc_game[0] == 'b' && strcmp(e[0].title, "Shenmue (USA)") == 0);
    assert(e[1].identity.game[0] == 'd' && e[1].disc_count == 2U);
    char anchor[65]; memset(anchor, 'c', 64U); anchor[64] = '\0';
    memcpy(e, original, 8U * sizeof(*e));
    assert(cl_group_discs(e, 8U, anchor) == 5U);
    assert(e[0].disc_index == 3U && e[0].next_disc_game[0] == 'a');
    assert(strcmp(e[0].content_path, original[0].content_path) == 0);
    free(original); free(e);
    puts("DISC_GROUPS_PASS one game per explicit disc family; initial disc one; anchored member retained; next disc wraps; regions/directories/systems stay distinct; original launch identity retained");
    return 0;
}
