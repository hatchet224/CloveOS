#include "supervisor/zip.h"
#include "lodepng.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static unsigned int u16(const unsigned char *p)
{ return (unsigned int)p[0] | (unsigned int)p[1] << 8U; }
static uint32_t u32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8U | (uint32_t)p[2] << 16U | (uint32_t)p[3] << 24U; }
static bool within(size_t at, size_t count, size_t bound)
{ return at <= bound && count <= bound - at; }
static bool directory_info(const unsigned char *zip,size_t size,size_t *start,
                           size_t *stop,unsigned int *count)
{
    if(zip==NULL || size<22U || size>64U*1024U*1024U) return false;
    size_t lower=size>65557U?size-65557U:0U,end=size-22U;
    for(;;) {
        if(memcmp(zip+end,"PK\005\006",4U)==0 &&
           (size_t)u16(zip+end+20U)==size-end-22U) break;
        if(end==lower) return false;
        --end;
    }
    unsigned int entries=u16(zip+end+10U);
    size_t length=u32(zip+end+12U),offset=u32(zip+end+16U);
    if(u16(zip+end+4U)!=0U || u16(zip+end+6U)!=0U || u16(zip+end+8U)!=entries ||
       entries==0U || entries>4096U || !within(offset,length,end) || offset+length!=end)
        return false;
    *start=offset;*stop=end;*count=entries;
    return true;
}
static size_t entry_span(const unsigned char *zip,size_t at,size_t end)
{
    if(!within(at,46U,end)) return 0U;
    const unsigned char *entry=zip+at;
    size_t span=46U+u16(entry+28U)+u16(entry+30U)+u16(entry+32U);
    if(memcmp(entry,"PK\001\002",4U)!=0 || u16(entry+34U)!=0U || !within(at,span,end))
        return 0U;
    return span;
}
bool sc_zip_walk(const unsigned char *zip,size_t size,sc_zip_member visit,void *context)
{
    size_t position,end;
    unsigned int count;
    if(visit==NULL || !directory_info(zip,size,&position,&end,&count)) return false;
    for(unsigned int i=0U;i<count;++i) {
        size_t span=entry_span(zip,position,end);
        if(span==0U) return false;
        const unsigned char *entry=zip+position;
        size_t length=u16(entry+28U);
        char name[512];
        if(length==0U || length>=sizeof(name) || memchr(entry+46U,0,length)!=NULL)
            return false;
        memcpy(name,entry+46U,length);name[length]='\0';
        if(!visit(context,name,u32(entry+24U))) return false;
        position+=span;
    }
    return position==end;
}
bool sc_zip_read(const unsigned char *zip, size_t size, const char *member,
                    size_t limit, unsigned char **bytes, size_t *count)
{
    size_t end, directory, position, member_size;
    const unsigned char *chosen = NULL, *local, *data;
    unsigned int entries, flags, method; uint32_t compressed, expanded, crc, offset;
    if (bytes == NULL || count == NULL) return false;
    *bytes = NULL; *count = 0U;
    if (member == NULL ||
        limit == 0U || limit > 64U * 1024U * 1024U) return false;
    member_size = strlen(member); if (member_size == 0U || member_size > 511U) return false;
    if(!directory_info(zip,size,&directory,&end,&entries)) return false;
    position = directory;
    for (unsigned int i = 0U; i < entries; ++i) {
        const unsigned char *entry; size_t length,span=entry_span(zip,position,end);
        if(span==0U) return false;
        entry = zip + position;
        length = u16(entry + 28U);
        if (length == member_size && memcmp(entry + 46U, member, length) == 0) {
            if (chosen != NULL) return false;
            chosen = entry;
        }
        position += span;
    }
    if (position != end || chosen == NULL) return false;
    flags = u16(chosen + 8U); method = u16(chosen + 10U);
    crc = u32(chosen + 16U); compressed = u32(chosen + 20U); expanded = u32(chosen + 24U); offset = u32(chosen + 42U);
    if ((flags & ~0x080eU) != 0U || (method != 0U && method != 8U) ||
        expanded == 0U || expanded > limit || !within(offset, 30U, directory)) return false;
    local = zip + offset;
    if (memcmp(local, "PK\003\004", 4U) != 0 || u16(local + 6U) != flags ||
        u16(local + 8U) != method || u16(local + 26U) != member_size ||
        !within(offset, 30U + member_size + u16(local + 28U), directory) ||
        memcmp(local + 30U, member, member_size) != 0) return false;
    if ((flags & 8U) == 0U && (u32(local + 14U) != crc || u32(local + 18U) != compressed || u32(local + 22U) != expanded)) return false;
    position = (size_t)offset + 30U + member_size + u16(local + 28U);
    if (!within(position, compressed, directory)) return false;
    data = zip + position;
    if (method == 0U) {
        if (compressed != expanded) return false;
        *bytes = malloc(expanded);
        if (*bytes == NULL) return false;
        memcpy(*bytes, data, expanded); *count = expanded;
    } else {
        LodePNGDecompressSettings settings;
        lodepng_decompress_settings_init(&settings); settings.max_output_size = expanded;
        if (lodepng_inflate(bytes, count, data, compressed, &settings) != 0U) goto failed;
    }
    if (*count != expanded || lodepng_crc32(*bytes, *count) != crc) goto failed;
    return true;
failed:
    free(*bytes); *bytes = NULL; *count = 0U; return false;
}
