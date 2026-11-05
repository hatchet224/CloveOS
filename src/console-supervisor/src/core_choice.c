#include "supervisor/core_choice.h"
#include "sessiond/durability.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const cores[] = {"gambatte","mgba","picodrive","genesis_plus_gx","fceumm","snes9x2005_plus","pcsx_rearmed","mupen64plus_next","flycast","ppsspp","fbneo","mednafen_pce_fast","fake08"};
static bool core_id(const char *id)
{
    for (size_t i = 0U; i < sizeof(cores)/sizeof(cores[0]); ++i) if (strcmp(id,cores[i]) == 0) return true;
    return false;
}
static bool template_game(const struct sc_game *base, const char *id, struct sc_game *out)
{
    char path[512], artifact[512]; struct cs_json j;
    if (!core_id(id)) return false;
    (void)snprintf(artifact,sizeof(artifact),"/system/lib/libretro/%s_libretro.so",id);
    if (strcmp(artifact,base->core) == 0) { *out = *base; return true; }
    int n = snprintf(path,sizeof(path),"/etc/console/core-variants/%s/%s.json",base->entry.system,id);
    if (n < 0 || (size_t)n >= sizeof(path) || !sc_read_json(path,&j)) return false;
    const char *core = cs_json_string(&j,"core_path"), *cfg = cs_json_string(&j,"config_path");
    const char *config_hash = cs_json_string(&j,"config_file_hash"), *options = cs_json_string(&j,"options_path");
    const char *options_hash = cs_json_string(&j,"options_file_hash"), *system = cs_json_string(&j,"system");
    if (core == NULL || strcmp(core,artifact) != 0 || cfg == NULL || strcmp(cfg,base->config) != 0 ||
        system == NULL || strcmp(system,base->entry.system) != 0 || config_hash == NULL || options_hash == NULL || options == NULL ||
        strcmp(options,"/etc/console/retroarch-core-options.cfg") != 0 ||
        !sc_hash_path(cfg,config_hash,1048576U) || !sc_hash_path(options,options_hash,1048576U)) return false;
    *out = *base;
    const char *const keys[] = {"core","config","retroarch","disc","policy"};
    char *values[] = {out->profile.identity.core,out->profile.identity.config,out->profile.identity.retroarch,out->profile.identity.disc,out->profile.identity.policy};
    for (size_t i = 0U; i < sizeof(keys)/sizeof(keys[0]); ++i) {
        const char *value = cs_json_string(&j,keys[i]);
        if (!cs_hex_valid(value,64U)) return false;
        memcpy(values[i],value,65U);
    }
    /* BIOS inventory belongs to the enrolled content, not an old template's
     * build-time inventory. Native state compatibility still checks all hashes. */
    strcpy(out->core,core); out->entry.identity = out->profile.identity;
    return true;
}
static bool choice(const struct sc_config *c, const char game[65], char out[64])
{
    char path[1024]; struct cs_json j; struct stat st;
    (void)snprintf(path,sizeof(path),"%s/.console/cores/%s.json",c->base.card_path,game);
    if (lstat(path,&st) != 0) { if (errno == ENOENT) {out[0]='\0';return true;} return false; }
    if (!sc_read_json(path,&j)) return false;
    uint64_t schema; const char *found = cs_json_string(&j,"game"), *id = cs_json_string(&j,"core");
    if (!cs_json_u64(&j,"schema",&schema) || schema != 1U || found == NULL || strcmp(found,game) != 0 || id == NULL ||
        (id[0] != '\0' && !core_id(id))) return false;
    strcpy(out,id); return true;
}
bool sc_core_resolve(const struct sc_config *c, const char game[65], struct sc_game *effective)
{
    const struct sc_game *base = sc_game_find(c,game); char id[64];
    if (base == NULL || !choice(c,game,id)) return false;
    if (id[0] == '\0') { *effective = *base; return true; }
    return template_game(base,id,effective);
}
static bool save_choice(const struct cs_store *store, const char game[65], const char *id)
{
    if (cs_store_check_media(store) != CS_STORE_OK || (id[0] != '\0' && !core_id(id))) return false;
    if (mkdirat(store->data_fd,"cores",0700) != 0 && errno != EEXIST) return false;
    int dir = openat(store->data_fd,"cores",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (dir < 0) return false;
    char name[80], temporary[128], bytes[256];
    (void)snprintf(name,sizeof(name),"%s.json",game);
    (void)snprintf(temporary,sizeof(temporary),".%s-%ld.tmp",game,(long)getpid());
    int n = snprintf(bytes,sizeof(bytes),"{\"schema\":1,\"game\":\"%s\",\"core\":\"%s\"}\n",game,id);
    int fd = openat(dir,temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    bool ok = fd >= 0 && n > 0 && (size_t)n < sizeof(bytes) && write(fd,bytes,(size_t)n) == n && fsync(fd) == 0;
    if (fd >= 0 && close(fd) != 0) ok = false;
    if (ok) ok = cs_flush_directory(dir,store->fat,store->qualified_fat_syncfs) == 0 &&
        renameat(dir,temporary,dir,name) == 0 && cs_flush_directory(dir,store->fat,store->qualified_fat_syncfs) == 0 &&
        cs_flush_directory(store->data_fd,store->fat,store->qualified_fat_syncfs) == 0;
    (void)unlinkat(dir,temporary,0);
    if (close(dir) != 0) ok = false;
    return ok;
}
void sc_core_options(const struct sc_config *c, const struct cs_store *store,
    const struct cl_tool_request *r, struct cl_tool_snapshot *s)
{
    memset(s,0,sizeof(*s)); s->kind = CL_TOOL_CORES;
    const struct sc_game *base = sc_game_find(c,r->target); struct sc_game effective, variant;
    if (base == NULL || !cs_hex_valid(r->target,64U)) goto failed;
    if (r->operation == CL_TOOL_SET_CORE) {
        if ((r->id[0] != '\0' && !template_game(base,r->id,&variant)) || !save_choice(store,r->target,r->id)) goto failed;
    }
    bool resolved = sc_core_resolve(c,r->target,&effective);
    struct cl_tool_row *row = &s->rows[s->count++];
    strcpy(row->id,"@default"); strcpy(row->label,"Default core"); row->available = true;
    row->enabled = resolved && strcmp(effective.core,base->core) == 0;
    (void)snprintf(row->detail,sizeof(row->detail),"%.100s",strrchr(base->core,'/')+1);
    for (size_t i = 0U; i < sizeof(cores)/sizeof(cores[0]); ++i) {
        if (!template_game(base,cores[i],&variant)) continue;
        row = &s->rows[s->count++]; strcpy(row->id,cores[i]); strcpy(row->label,cores[i]);
        row->available = true; row->enabled = resolved && strcmp(effective.core,variant.core) == 0;
        strcpy(row->detail,"Per-game core. Existing states remain tied to their original core.");
    }
    s->ready = true;
    strcpy(s->status,r->operation == CL_TOOL_SET_CORE ? "Game core saved. Use Reset game after changing core; older states are retained." : "Select a qualified core for this game. Default restores the system core.");
    return;
failed:
    s->error = true; strcpy(s->status,"Core selection could not be validated or saved. Choose Default to repair a missing core selection.");
}
