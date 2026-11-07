/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exact production suffix and container parser; core/config APIs are fixtures. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#undef assert
#define assert(x) do {if(!(x)){fprintf(stderr,"ASSERT line=%d %s\n",__LINE__,#x);exit(1);}}while(0)
struct magicx_blob { void *data; size_t size; };
struct element { const char *data; };
struct values { size_t size; struct element *elems; };
struct core_option { const char *key; struct values *vals; size_t default_index,index,opt_idx; };
typedef struct {size_t size;struct core_option *opts;const char *conf_path;} core_option_manager_t;
typedef struct {int unused;} config_file_t;
static struct {core_option_manager_t *core_options;} runtime;
static unsigned changes;
#define runloop_state_get_ptr() (&runtime)
static config_file_t *config_file_new_from_path_to_string(const char *p){(void)p;return NULL;}
static bool config_get_array(config_file_t *c,const char *k,char *v,size_t n){(void)c;(void)k;(void)v;(void)n;return false;}
static void config_file_free(config_file_t *c){(void)c;}
static const char *core_option_manager_get_val(core_option_manager_t *m,size_t i){return m->opts[i].vals->elems[m->opts[i].index].data;}
static void core_option_manager_set_val(core_option_manager_t *m,size_t i,size_t v,bool flush){(void)flush;++changes;m->opts[i].index=v;}
static uint32_t little_u32(const unsigned char *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static bool safe_rastate(const struct magicx_blob *state)
{
    const unsigned char *data = state->data;
    size_t position = 8U;
    bool found_memory = false;
    if (data == NULL || state->size < 32U || memcmp(data, "RASTATE", 7U) != 0 || data[7] != 1U) {
        return false;
    }
    while (position <= state->size - 8U) {
        size_t bytes = (size_t)little_u32(data + position + 4U), aligned;
        if (memcmp(data + position, "END ", 4U) == 0) {
            return found_memory && bytes == 0U && position + 8U == state->size;
        }
        /* This qualified profile has no achievement/movie metadata. Refuse
         * unknown blocks before entering the upstream container parser. */
        if (found_memory || memcmp(data + position, "MEM ", 4U) != 0 || bytes == 0U ||
            bytes > SIZE_MAX - 7U) {
            return false;
        }
        aligned = (bytes + 7U) & ~(size_t)7U;
        if (aligned > state->size - position - 8U) {
            return false;
        }
        found_memory = true;
        position += 8U + aligned;
    }
    return false;
}
#include "../magicx_state_options.h"
static struct magicx_blob clone(const struct magicx_blob *b){struct magicx_blob c={malloc(b->size),b->size};assert(c.data);memcpy(c.data,b->data,b->size);return c;}
static void reject(struct magicx_blob *b){fprintf(stderr,"reject size=%zu\n",b->size);unsigned before=changes;assert(!options_restore(b));assert(changes==before);free(b->data);}
int main(void){
 struct element es[]={{"off"},{"on"}};struct values values={2,es};struct core_option opts[]={{"one",&values,0,0,0},{"two",&values,0,0,1}};
 core_option_manager_t manager={2,opts,""};runtime.core_options=&manager;
 unsigned char native[32]={0};memcpy(native,"RASTATE\1MEM ",12);native[12]=8;memcpy(native+24,"END ",4);
 struct magicx_blob base={native,sizeof(native)},plain=clone(&base);assert(options_snapshot(&plain,4096)&&plain.size==base.size&&memcmp(plain.data,base.data,base.size)==0);free(plain.data);
 opts[0].index=1;struct magicx_blob changed=clone(&base);assert(options_snapshot(&changed,4096)&&changed.size>32);
 opts[0].index=0;struct magicx_blob restored=clone(&changed);assert(options_restore(&restored)&&restored.size==32&&opts[0].index==1);free(restored.data);
 restored=clone(&base);assert(options_restore(&restored)&&opts[0].index==0);free(restored.data);
 for(size_t length=0;length<changed.size;++length){if(length==32)continue;restored=clone(&changed);restored.size=length;reject(&restored);}
 size_t footer=changed.size-16,payload=32;
 for(unsigned variant=0;variant<9;++variant){fprintf(stderr,"variant=%u\n",variant);restored=clone(&changed);unsigned char *p=restored.data;
  switch(variant){case 0:p[footer+12]=2;break;case 1:p[footer+8]=255;break;case 2:p[payload]=3;break;case 3:p[payload+4]=0;break;case 4:p[payload+6]=255;break;case 5:p[payload+8]=0;break;case 6:p[payload+11]='X';break;case 7:memcpy(p+payload+17,"one",3);break;case 8:p[0]='X';break;}
  reject(&restored);
 }
 struct magicx_blob small=clone(&base);opts[0].index=1;assert(!options_snapshot(&small,32));free(small.data);
 free(changed.data);puts("NATIVE_OPTIONS_DEFAULT_BYTE_EXACT_CHANGED_ROUNDTRIP_FACTORY_RESET_AND_MALFORMED_NO_MUTATION_PASS");return 0;
}
