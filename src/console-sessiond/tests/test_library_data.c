#include "sessiond/document.h"
#include "sessiond/library.h"
#include "sessiond/hotkeys.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static void json_tests(void) {
    struct cs_document *d = calloc(1U, sizeof(*d));
    const char *valid = "{\"nested\":{\"font\":\"Pok\\u00e9mon "
                        "\\ud83c\\udfae\",\"signed\":-2147483648},\"list\":[true,null,3.5e2]}";
    const char *bad[] = {"{\"a\":1,\"a\":2}", "{\"a\":1,\"\\u0061\":2}", "[1,]",
                         "{\"x\":01}",        "{\"x\":\"\\uD800\"}",     "{\"x\":\"\\u0000\"}",
                         "{} trailing",       "{\"x\":\"\xc0\xaf\"}"};
    size_t i, parent;
    int n;
    char text[64];
    CHECK(d != NULL && cs_doc_parse(d, valid, strlen(valid)));
    parent = cs_doc_get(d, 0U, "nested");
    CHECK(parent != SIZE_MAX);
    CHECK(cs_doc_text(d, cs_doc_get(d, parent, "font"), text, sizeof(text)));
    CHECK(strcmp(text, "Pok\xc3\xa9mon \xf0\x9f\x8e\xae") == 0);
    CHECK(cs_doc_int(d, cs_doc_get(d, parent, "signed"), &n) && n == INT_MIN);
    CHECK(!cs_doc_int(d, cs_doc_get(d, 0U, "list"), &n));
    for (i = 0U; i < sizeof(bad) / sizeof(bad[0]); ++i)
        CHECK(!cs_doc_parse(d, bad[i], strlen(bad[i])));
    {
        char deep[80];
        memset(deep, '[', 33U);
        memset(deep + 33, ']', 33U);
        CHECK(!cs_doc_parse(d, deep, 66U));
    }
    {
        unsigned int seed = 12345U;
        char fuzz[256];
        size_t round, j;
        for (round = 0U; round < 2000U; ++round) {
            for (j = 0U; j < sizeof(fuzz); ++j) {
                seed = seed * 1664525U + 1013904223U;
                fuzz[j] = (char)(seed >> 24U);
            }
            (void)cs_doc_parse(d, fuzz, round % sizeof(fuzz) + 1U);
        }
    }
    free(d);
}
static void metadata_tests(void) {
    struct cs_metadata_list *list = calloc(1U, sizeof(*list));
    char path[512];
    const char *xml = "\xef\xbb\xbf<?xml version=\"1.0\"?><gameList><game id=\"2\"><path>./Pokemon "
                      "- Blue.gbc</path>"
                      "<name>Pok&#233;mon &amp; Blue</name><image>./Imgs/Blue.png</image></game>"
                      "<game><path>Hacks/Example.gb</path><name><![CDATA[Example & "
                      "test]]></name></game></gameList>";
    const char *json = "[{\"label\":\"Pokemon - "
                       "Gold\",\"rompath\":\"/mnt/SDCARD/Roms/GBC/Gold.gbc\",\"imgpath\":\"Imgs/"
                       "Gold.png\",\"launch\":\"evil.sh\"}]";
    const char *bad[] = {"<!DOCTYPE gameList [<!ENTITY x SYSTEM 'file:///etc/passwd'>]><gameList/>",
                         "<gameList><game><path>x.gb</path></game>",
                         "<gameList><game><path>one.gb</path><path>two.gb</path></game></gameList>",
                         "<gameList><game><path>&unknown;</path></game></gameList>",
                         "<gameList><game><path>\xed\xa0\x80</path></game></gameList>"};
    size_t i;
    CHECK(list != NULL && cs_metadata_parse_xml(xml, strlen(xml), list));
    CHECK(!cs_metadata_parse_xml(NULL, 100U, list));
    CHECK(!cs_metadata_parse_xml(xml, SIZE_MAX, list));
    CHECK(list->count == 2U && strcmp(list->games[0].name, "Pok\xc3\xa9mon & Blue") == 0);
    CHECK(strcmp(list->games[1].name, "Example & test") == 0);
    for (i = 0U; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK(!cs_metadata_parse_xml(bad[i], strlen(bad[i]), list));
        CHECK(list->count == 2U);
    }
    CHECK(cs_metadata_parse_json(json, strlen(json), list) && list->count == 1U);
    CHECK(strcmp(list->games[0].name, "Pokemon - Gold") == 0);
    {
        const char *ndjson = "{\"label\":\"Blue\",\"rompath\":\"Blue.gb\",\"type\":5}\n{\"label\":"
                             "\"Gold\",\"rompath\":\"Gold.gbc\",\"launch\":\"ignored.sh\"}\n";
        CHECK(cs_metadata_parse_json(ndjson, strlen(ndjson), list) && list->count == 2U);
        CHECK(strcmp(list->games[1].name, "Gold") == 0);
    }
    CHECK(cs_card_resolve("Emu/GB", "../../Roms/GB", path) && strcmp(path, "Roms/GB") == 0);
    CHECK(cs_card_resolve("Roms/GB", "./Hacks/../Pokemon.gb", path) &&
          strcmp(path, "Roms/GB/Pokemon.gb") == 0);
    CHECK(cs_card_resolve("Roms/GB", "/mnt/SDCARD/Roms/GBC/Gold.gbc", path) &&
          strcmp(path, "Roms/GBC/Gold.gbc") == 0);
    CHECK(!cs_card_resolve("", "../../etc/passwd", path));
    CHECK(!cs_card_resolve("Roms/GB", "/etc/passwd", path));
    CHECK(!cs_card_resolve("Roms/GB", "bad\\path.gb", path));
    free(list);
}
static void hotkey_tests(void) {
    struct cs_hotkeys k;
    cs_hotkeys_reset(&k, true);
    cs_hotkeys_menu(&k, false, false, 10U);
    CHECK(cs_hotkeys_tick(&k, 500U) == CS_MENU_NONE);
    cs_hotkeys_menu(&k, true, false, 600U);
    cs_hotkeys_menu(&k, false, false, 700U);
    CHECK(cs_hotkeys_tick(&k, 899U) == CS_MENU_NONE);
    CHECK(cs_hotkeys_tick(&k, 900U) == CS_MENU_RECENT);
    CHECK(cs_hotkeys_tick(&k, 901U) == CS_MENU_NONE);
    cs_hotkeys_menu(&k, true, false, 1000U);
    cs_hotkeys_consume(&k);
    cs_hotkeys_menu(&k, false, false, 1100U);
    CHECK(cs_hotkeys_tick(&k, 2000U) == CS_MENU_NONE);
    cs_hotkeys_menu(&k, true, false, 2100U);
    cs_hotkeys_menu(&k, false, false, 2200U);
    cs_hotkeys_consume(&k);
    CHECK(cs_hotkeys_tick(&k, 2400U) == CS_MENU_NONE);
    cs_hotkeys_menu(&k, true, false, 3000U);
    cs_hotkeys_menu(&k, false, false, 4100U);
    CHECK(cs_hotkeys_tick(&k, 4280U) == CS_MENU_LIBRARY);
    cs_hotkeys_menu(&k, true, false, 5000U);
    cs_hotkeys_menu(&k, true, true, 6000U);
    cs_hotkeys_menu(&k, false, false, 4999U);
    CHECK(cs_hotkeys_tick(&k, 9000U) == CS_MENU_NONE);
    cs_hotkeys_reset(&k,false);
    cs_hotkeys_menu(&k,true,false,10000U);
    cs_hotkeys_menu(&k,false,false,10060U);
    cs_hotkeys_menu(&k,true,false,10200U);
    cs_hotkeys_menu(&k,false,false,10260U);
    CHECK(cs_hotkeys_tick(&k,10260U) == CS_MENU_PREVIOUS);
    CHECK(cs_hotkeys_tick(&k,10400U) == CS_MENU_NONE);
    cs_hotkeys_menu(&k,true,false,11000U);
    CHECK(cs_hotkeys_tick(&k,11699U) == CS_MENU_NONE);
    CHECK(cs_hotkeys_tick(&k,11700U) == CS_MENU_LIBRARY);
    cs_hotkeys_menu(&k,false,false,11800U);
    CHECK(cs_hotkeys_tick(&k,12000U) == CS_MENU_NONE);
    cs_hotkeys_reset(&k, false);
    CHECK(!cs_hotkeys_volume(&k, true, 2, 0U)); /* boot-held repeat */
    cs_hotkeys_menu(&k, true, false, 100U);
    CHECK(cs_hotkeys_volume(&k, true, 1, 110U));
    CHECK(k.chord && !k.pending);
    CHECK(!cs_hotkeys_volume(&k, true, 1, 200U)); /* duplicate edge */
    CHECK(!cs_hotkeys_volume(&k, true, 2, 189U));
    CHECK(cs_hotkeys_volume(&k, true, 2, 190U));
    CHECK(!cs_hotkeys_volume(&k, true, 2, 189U)); /* clock rollback */
    CHECK(!cs_hotkeys_volume(&k, true, 0, 200U));
    cs_hotkeys_menu(&k, false, false, 210U);
    CHECK(cs_hotkeys_tick(&k, 1000U) == CS_MENU_NONE); /* brightness != carousel */
    CHECK(cs_hotkeys_volume(&k, false, 1, 1100U));
    CHECK(!k.down); /* unmodified volume */
    CHECK(!cs_hotkeys_volume(&k, false, 9, 1200U));
    cs_hotkeys_reset(&k, true); /* dropped input disarms old repeats */
    CHECK(!cs_hotkeys_volume(&k, false, 2, 2000U));
}
int main(void) {
    json_tests();
    metadata_tests();
    hotkey_tests();
    puts("PASS: nested JSON, Unicode, hostile/malformed XML, card paths, hotkey suppression, 2000 "
         "bounded parser probes");
    return 0;
}
