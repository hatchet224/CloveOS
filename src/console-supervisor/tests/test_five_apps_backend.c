#ifdef NDEBUG
#error "App backend fixture requires active assertions"
#endif
#include "sessiond/library.h"
#include <assert.h>

static bool test_scan(const struct cs_library_card *cards,size_t count,struct cs_library *out);
#define cs_library_scan test_scan
#include "../src/disk.c"
#undef cs_library_scan

static bool test_scan(const struct cs_library_card *cards,size_t count,struct cs_library *out)
{
    (void)cards; (void)count;
    memset(out,0,sizeof(*out));
    out->revision = 123U; out->count = 2U;
    strcpy(out->games[0].system,"GB"); strcpy(out->games[1].system,"FC");
    strcpy(out->games[0].content,"/mnt/SDCARD/Roms/GB/Metal.gb");
    strcpy(out->games[1].content,"/mnt/SDCARD/Roms/FC/Metal.nes");
    strcpy(out->games[0].title,"Metal GB"); strcpy(out->games[1].title,"Metal FC");
    return true;
}

static void file(const char *path, const char *data)
{
    int fd = open(path,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600);
    assert(fd >= 0 && write(fd,data,strlen(data)) == (ssize_t)strlen(data) && close(fd) == 0);
}

int main(void)
{
    struct sc_config *c = calloc(1U,sizeof(*c));
    struct sc_tools_backend *tools = calloc(1U,sizeof(*tools));
    struct cl_tool_snapshot *snapshot = calloc(1U,sizeof(*snapshot));
    struct cl_history *query = calloc(1U,sizeof(*query)), *history = calloc(1U,sizeof(*history));
    struct sc_message *status = calloc(1U,sizeof(*status));
    struct sc_disk_result *result = calloc(1U,sizeof(*result));
    struct cs_store store;
    struct library_cache cache = {0};
    struct sp_package_selection selection;
    assert(c && tools && snapshot && query && history && status && result);
    strcpy(c->base.card_path,"/mnt/SDCARD"); strcpy(c->base.card_uuid,"C028-0002");
    strcpy(c->system_path,"/system"); c->base.qualified_fat_syncfs = true;
    c->base.max_state_bytes = 1048576U;
    assert(reader(c,&store) == CS_STORE_OK);

    struct cl_tool_request req = {.operation=CL_TOOL_LOAD_PACKAGES};
    sc_tools_request(tools,c,&store,&req,snapshot);
    assert(snapshot->ready && snapshot->revision == 11U && snapshot->app_features == CL_APP_DEFAULTS);
    assert(snapshot->enabled_mask == UINT64_C(0x10bffffff));
    req.operation = CL_TOOL_APPLY_PACKAGES;
    req.revision = snapshot->revision; req.enabled_mask = snapshot->enabled_mask;
    const char *enable[] = {"app.expert", "app.list-shortcuts", "app.battery"};
    for (size_t i = 0U; i < sizeof(enable)/sizeof(enable[0]); ++i) {
        int index = sp_package_index(enable[i]); assert(index >= 0);
        req.enabled_mask |= UINT64_C(1) << (unsigned int)index;
    }
    sc_tools_request(tools,c,&store,&req,snapshot);
    assert(snapshot->ready && snapshot->revision == 12U && snapshot->app_features == 31U);
    assert(sc_tools_packages(tools,&store,&selection));
    puts("R46_PACKAGE_MIGRATION_AND_FOUR_OPTIONAL_APP_ROWS_PASS");

    query->library_mode = CL_LIBRARY_SYSTEMS;
    query->library_search_tile = true; query->library_query_id = 1U;
    assert(library_snapshot(c,history,&selection,query,&cache,&store));
    assert(history->count == 5U && history->entries[2].library_action == CL_LIBRARY_SEARCH_RESULTS);
    assert(history->entries[3].library_action==CL_LIBRARY_PORTMASTER && history->entries[4].library_action==CL_LIBRARY_PORTS);
    /* Put PortMaster exactly on a page boundary; hiding it must compact the
     * complete system list before page totals, anchors and bounds are chosen. */
    query->library_page_size=3U;query->library_offset=3U;
    assert(library_snapshot(c,history,&selection,query,&cache,&store));
    assert(history->library_total==5U && history->library_offset==3U && history->count==2U);
    query->library_hide_portmaster=true;
    assert(library_snapshot(c,history,&selection,query,&cache,&store));
    assert(history->library_total==4U && history->library_offset==3U && history->count==1U && !history->library_more);
    assert(history->entries[0].library_action==CL_LIBRARY_PORTS);
    query->library_offset=0U;
    assert(library_snapshot(c,history,&selection,query,&cache,&store));
    assert(history->library_total==4U && history->count==3U && history->library_more);
    query->library_offset=99U;
    assert(library_snapshot(c,history,&selection,query,&cache,&store));
    assert(history->library_offset==3U && history->count==1U);
    query->library_hide_portmaster=false;query->library_page_size=0U;query->library_offset=0U;
    puts("R51_PORTMASTER_FILTER_BEFORE_PAGINATION_GLOBAL_TOTALS_BOUNDARY_AND_CLAMP_PASS");
    query->library_mode = CL_LIBRARY_GAMES; query->library_search_tile = false; query->library_search_results = true;
    strcpy(query->library_keyword,"metal");
    assert(library_snapshot(c,history,&selection,query,&cache,&store));
    assert(history->count == 3U && history->entries[2].library_action == CL_LIBRARY_CLEAR_SEARCH);
    query->library_search_results = false; strcpy(query->library_system,"GB");
    assert(library_snapshot(c,history,&selection,query,&cache,&store));
    assert(history->count == 4U && history->entries[1].library_action == CL_LIBRARY_CLEAR_FILTER &&
           history->entries[2].library_action == CL_LIBRARY_FILTER && history->entries[3].library_action == CL_LIBRARY_REFRESH);
    assert(strcmp(history->entries[0].system,"GB") == 0);
    req.revision = snapshot->revision; req.enabled_mask = snapshot->enabled_mask;
    req.enabled_mask &= ~((UINT64_C(1) << (unsigned int)sp_package_index("app.search")) |
                          (UINT64_C(1) << (unsigned int)sp_package_index("app.list-shortcuts")));
    sc_tools_request(tools,c,&store,&req,snapshot);
    assert(snapshot->ready && (snapshot->app_features & (CL_APP_SEARCH|CL_APP_LIST_SHORTCUTS)) == 0U);
    assert(sc_tools_packages(tools,&store,&selection));
    assert(library_snapshot(c,history,&selection,query,&cache,&store) && history->count == 1U);
    query->library_mode = CL_LIBRARY_SYSTEMS; query->library_search_tile = true;
    assert(library_snapshot(c,history,&selection,query,&cache,&store) && history->count == 4U);
    puts("OPTIONAL_SEARCH_AND_LIST_ROWS_CROSS_SYSTEM_CATALOG_PASS");

    req = (struct cl_tool_request){.operation=CL_TOOL_LOAD_GALLERY};
    sc_tools_request(tools,c,&store,&req,snapshot);
    assert(snapshot->ready && snapshot->count == 100U && snapshot->available_mask == 104U);
    assert(strcmp(snapshot->rows[0].label,"000.JPG") == 0);
    req.enabled_mask = 100U;
    sc_tools_request(tools,c,&store,&req,snapshot);
    assert(snapshot->ready && snapshot->count == 4U && snapshot->enabled_mask == 100U &&
           strcmp(snapshot->rows[3].label,"103.PNG") == 0);
    puts("GALLERY_FORMATS_SORT_AND_SECOND_PAGE_PASS");

    sc_message_init(status,SC_UI_STATUS,1U);
    status->data.telemetry.record_history = true;
    job(c,tools,status,result,&cache);
    assert(result->ok && result->message.data.telemetry.battery == 70 && !result->message.data.telemetry.usb);
    struct cl_battery_history samples;
    assert(sc_battery_history_read(&store,&samples) && samples.count == 1U && samples.points[0].power == CL_ON_BATTERY);
    uint32_t first = samples.points[0].seconds;
    assert(sc_battery_history_record(&store,first+60U,69,CL_ON_BATTERY));
    assert(sc_battery_history_record(&store,first+300U,68,CL_ON_BATTERY));
    assert(sc_battery_history_record(&store,first+320U,68,CL_CHARGING));
    req = (struct cl_tool_request){.operation=CL_TOOL_LOAD_BATTERY};
    sc_tools_request(tools,c,&store,&req,snapshot);
    assert(snapshot->ready && snapshot->battery.count == 3U && snapshot->battery.points[2].power == CL_CHARGING);
    file("/sys/class/power_supply/axp2202-battery/capacity","101\n");
    file("/sys/class/power_supply/axp2202-usb/online","1\n");
    job(c,tools,status,result,&cache);
    assert(result->message.data.telemetry.battery == -1 && !result->message.data.telemetry.usb);
    file("/sys/class/power_supply/axp2202-battery/status","Charging\n");
    job(c,tools,status,result,&cache);
    assert(result->message.data.telemetry.usb);
    assert(sc_battery_history_read(&store,&samples) && samples.count == 3U);
    puts("AXP2202_TELEMETRY_INVALID_VALUES_AND_BATTERY_HISTORY_PASS");
    cs_store_close(&store); free(cache.entries); free(result); free(status); free(history);
    free(query); free(snapshot); free(tools); free(c);
    return 0;
}
