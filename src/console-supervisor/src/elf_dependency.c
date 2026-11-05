#include "supervisor/elf_dependency.h"
#include <elf.h>
#include <stdint.h>
#include <string.h>
static bool range(uint64_t offset, uint64_t length, size_t size)
{ return offset <= size && length <= size - offset; }
bool sc_elf_remove_unused_tmenu(unsigned char *bytes, size_t size)
{
    Elf64_Ehdr h; Elf64_Phdr ph; Elf64_Dyn d;
    uint64_t dynamic = 0U, entries = 0U, string_address = 0U, strings = 0U, string_size = 0U;
    uint64_t found = UINT64_MAX, end = UINT64_MAX;
    if (bytes == NULL || size < sizeof(h)) return false;
    memcpy(&h, bytes, sizeof(h));
    if (memcmp(h.e_ident, "\177ELF\002\001\001", 7U) != 0 || h.e_machine != EM_AARCH64 ||
        h.e_type != ET_DYN || h.e_phentsize != sizeof(ph) || h.e_phnum > 128U ||
        !range(h.e_phoff, (uint64_t)h.e_phnum * sizeof(ph), size)) return false;
    for (unsigned int i = 0U; i < h.e_phnum; ++i) {
        memcpy(&ph, bytes + h.e_phoff + (uint64_t)i * sizeof(ph), sizeof(ph));
        if (ph.p_type != PT_DYNAMIC) continue;
        if (entries != 0U || ph.p_filesz % sizeof(d) != 0U || !range(ph.p_offset, ph.p_filesz, size)) return false;
        dynamic = ph.p_offset; entries = ph.p_filesz / sizeof(d);
    }
    if (entries == 0U || entries > 4096U) return false;
    for (uint64_t i = 0U; i < entries; ++i) {
        memcpy(&d, bytes + dynamic + i * sizeof(d), sizeof(d));
        if (d.d_tag == DT_NULL) { end = i; break; }
        if (d.d_tag == DT_STRTAB) string_address = d.d_un.d_ptr;
        if (d.d_tag == DT_STRSZ) string_size = d.d_un.d_val;
    }
    if (end == UINT64_MAX || string_size == 0U) return false;
    for (unsigned int i = 0U; i < h.e_phnum; ++i) {
        memcpy(&ph, bytes + h.e_phoff + (uint64_t)i * sizeof(ph), sizeof(ph));
        if (ph.p_type == PT_LOAD && string_address >= ph.p_vaddr &&
            string_address - ph.p_vaddr < ph.p_filesz) {
            uint64_t displacement = string_address - ph.p_vaddr;
            if (string_size > ph.p_filesz - displacement || !range(ph.p_offset, displacement, size)) return false;
            strings = ph.p_offset + displacement;
            break;
        }
    }
    if (strings == 0U || !range(strings, string_size, size)) return false;
    for (uint64_t i = 0U; i < end; ++i) {
        memcpy(&d, bytes + dynamic + i * sizeof(d), sizeof(d));
        if (d.d_tag != DT_NEEDED) continue;
        if (d.d_un.d_val >= string_size) return false;
        if (string_size - d.d_un.d_val >= sizeof("libtmenu.so") &&
            memcmp(bytes + strings + d.d_un.d_val, "libtmenu.so", sizeof("libtmenu.so")) == 0) {
            if (found != UINT64_MAX) return false;
            found = i;
        }
    }
    if (found == UINT64_MAX) return false;
    memmove(bytes + dynamic + found * sizeof(d), bytes + dynamic + (found + 1U) * sizeof(d),
            (size_t)(end - found) * sizeof(d));
    memset(bytes + dynamic + end * sizeof(d), 0, sizeof(d));
    return true;
}
