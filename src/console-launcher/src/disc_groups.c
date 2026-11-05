#include "launcher/history.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

unsigned int cl_disc_group_key(const struct cl_entry *entry, char key[512], char title[97])
{
    const char *path = entry->content_path, *base;
    if (strcmp(entry->system, "DC") != 0 && strcmp(entry->system, "PS") != 0 &&
        strcmp(entry->system, "SATURN") != 0 && strcmp(entry->system, "PCE") != 0 &&
        strcmp(entry->system, "SEGACD") != 0 && strcmp(entry->system, "NEOCD") != 0 &&
        strcmp(entry->system, "PCFX") != 0 && strcmp(entry->system, "AMIGA") != 0) return 0U;
    if (strnlen(path, 512U) == 512U) return 0U;
    base = strrchr(path, '/');
    base = base == NULL ? path : base + 1;
    const char *directory=base;
    if(base>path){
        const char *parent=base-1;
        while(parent>path && parent[-1]!='/') --parent;
        if(parent[0]=='.') directory=parent;
    }
    for (const char *p = base; *p != '\0'; ++p) {
        bool bracket = strncasecmp(p, "(Disc ", 6U) == 0 || strncasecmp(p, "(Disk ", 6U) == 0;
        if (!bracket && strncasecmp(p, " - Disc ", 8U) != 0) continue;
        const char *digit = p + (bracket ? 6 : 8), *end = digit;
        unsigned int index = 0U;
        while (*end >= '0' && *end <= '9') {
            index = index * 10U + (unsigned int)(*end++ - '0');
            if (index > 99U) return 0U;
        }
        if(end==digit || index==0U) continue;
        if(bracket && strncasecmp(end," of ",4U)==0) {
            end+=4;
            if(*end<'1' || *end>'9') continue;
            while(*end>='0' && *end<='9') ++end;
        }
        if (bracket ? *end != ')' : (*end != ' ' && *end != '.'))
            continue;
        if (bracket) ++end;
        size_t prefix = (size_t)(directory - path), length = (size_t)(p - base);
        while (length > 0U && base[length - 1U] == ' ') --length;
        if (length == 0U || length >= 97U || prefix + length + strlen(end) >= 512U) return 0U;
        memcpy(key,path,prefix);
        memcpy(key+prefix,base,length);
        strcpy(key+prefix+length,end);
        memcpy(title, base, length); title[length] = '\0';
        return index;
    }
    return 0U;
}

size_t cl_group_discs(struct cl_entry *entries, size_t count, const char *anchor)
{
    bool used[CL_LIBRARY_BOUND] = {false};
    size_t kept = 0U;
    if (entries == NULL || count > CL_LIBRARY_BOUND) return count;
    for (size_t i = 0U; i < count; ++i) {
        char key[512], title[97];
        unsigned int indices[CL_LIBRARY_BOUND] = {0};
        size_t chosen = i, members = 1U;
        if (used[i]) continue;
        indices[i] = cl_disc_group_key(&entries[i], key, title);
        if (indices[i] != 0U) {
            for (size_t j = i + 1U; j < count; ++j) {
                char candidate[512], ignored[97];
                unsigned int index = cl_disc_group_key(&entries[j], candidate, ignored);
                if (used[j] || index == 0U || strcmp(entries[i].system, entries[j].system) != 0 ||
                    strcmp(key, candidate) != 0) continue;
                bool duplicate = false;
                for (size_t k = i; k < j; ++k) if (indices[k] == index) duplicate = true;
                if (duplicate) continue;
                indices[j] = index; used[j] = true; ++members;
                if (indices[j] < indices[chosen]) chosen = j;
            }
            if (anchor != NULL && anchor[0] != '\0')
                for (size_t j = i; j < count; ++j)
                    if (indices[j] != 0U && strcmp(entries[j].identity.game, anchor) == 0) chosen = j;
        }
        struct cl_entry entry = entries[chosen];
        entry.disc_index = indices[chosen]; entry.disc_count = (unsigned int)members;
        entry.next_disc_game[0] = '\0';
        if (members > 1U) {
            size_t next = chosen;
            for (size_t j = i; j < count; ++j)
                if (indices[j] > indices[chosen] && (next == chosen || indices[j] < indices[next])) next = j;
            if (next == chosen)
                for (size_t j = i; j < count; ++j)
                    if (indices[j] != 0U && indices[j] < indices[next]) next = j;
            strcpy(entry.title, title); strcpy(entry.next_disc_game, entries[next].identity.game);
            (void)snprintf(entry.context, sizeof(entry.context), "Disc %u (%zu discs)", entry.disc_index, members);
        }
        entries[kept++] = entry;
    }
    return kept;
}
