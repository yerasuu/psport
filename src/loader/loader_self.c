// SPDX-License-Identifier: GPL-2.0-or-later
/* PS4 SELF and plain ELF files for the loader: program headers, the dynamic section and the SCE
 * dynamic tables (strings, symbols, relocations, library and module ids), every offset checked
 * before use. Decrypted dumps only: an encrypted or compressed SELF segment is an error. */
#define _GNU_SOURCE
#include "loader.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PT_DYNAMIC 2
#define PT_SCE_DYNLIBDATA 0x61000000
#define PT_SCE_RELRO 0x61000010
#define PT_GNU_EH_FRAME 0x6474e550
#define DT_SCE_STRTAB 0x61000035
#define DT_SCE_STRSZ 0x61000037
#define DT_SCE_SYMTAB 0x61000039
#define DT_SCE_SYMTABSZ 0x6100003f
#define DT_SCE_RELA 0x6100002f
#define DT_SCE_RELASZ 0x61000031
#define DT_SCE_JMPREL 0x61000029
#define DT_SCE_PLTRELSZ 0x6100002d
#define IMAGE_LIMIT (512u * 1024 * 1024)

const char *g_sz_loader_file;

void loader_fail(const char *sz_format, ...) {
    va_list args;
    va_start(args, sz_format);
    fputs("ERROR: ", stderr);
    if (g_sz_loader_file) fprintf(stderr, "%s: ", g_sz_loader_file);
    vfprintf(stderr, sz_format, args);
    fputc('\n', stderr);
    va_end(args);
    exit(1);
}

unsigned char *loader_read_file(const char *sz_path, size_t *p_size) {
    FILE *p_file = ps_host_fs_open_stream(sz_path, "rb");
    PsHostStat st;
    if (!p_file || ps_host_fs_stat_stream(p_file, &st) || !PS_HOST_MODE_IS_REGULAR(st.mode)) loader_fail("cannot read %s", sz_path);
    unsigned char *p_data = malloc(st.cb_size ? (size_t)st.cb_size : 1);
    if (!p_data || fread(p_data, 1, (size_t)st.cb_size, p_file) != (size_t)st.cb_size) loader_fail("cannot read %s", sz_path);
    fclose(p_file);
    *p_size = (size_t)st.cb_size;
    return p_data;
}

const unsigned char *loader_span(const unsigned char *p_data, size_t cb_data, uint64_t off_data, uint64_t length) {
    if (off_data > cb_data || length > cb_data - off_data)
        loader_fail("out of bounds: %#llx+%#llx / %#zx", (unsigned long long)off_data, (unsigned long long)length, cb_data);
    return p_data + off_data;
}
static uint16_t rd16(const unsigned char *p_bytes) { uint16_t v; memcpy(&v, p_bytes, 2); return v; }
static uint32_t rd32(const unsigned char *p_bytes) { uint32_t v; memcpy(&v, p_bytes, 4); return v; }
static uint64_t rd64(const unsigned char *p_bytes) { uint64_t v; memcpy(&v, p_bytes, 8); return v; }
static uint64_t add_sat(uint64_t a, uint64_t b) { return a > UINT64_MAX - b ? UINT64_MAX : a + b; }

static void parse_headers(const unsigned char *p_data, size_t cb_data, uint64_t base, LoaderModule *p_module, uint64_t *p_phoff) {
    const unsigned char *p_header = loader_span(p_data, cb_data, base, 64);
    if (memcmp(p_header, "\x7f" "ELF\x02\x01\x01", 7) || rd16(p_header + 18) != 62) loader_fail("expected little-endian x86-64 ELF");
    p_module->phnum = rd16(p_header + 56);
    if (rd16(p_header + 54) != 56 || !p_module->phnum || p_module->phnum >= 256) loader_fail("unsupported program headers");
    p_module->entry = rd64(p_header + 24);
    *p_phoff = rd64(p_header + 32);
    if (!(p_module->p_ph = calloc(p_module->phnum, sizeof(*p_module->p_ph)))) loader_fail("allocation failed");
    for (unsigned i = 0; i < p_module->phnum; ++i) {
        const unsigned char *p_entry = loader_span(p_data, cb_data, add_sat(add_sat(base, *p_phoff), i * 56u), 56);
        LoaderPh *p_ph = &p_module->p_ph[i];
        p_ph->type = rd32(p_entry); p_ph->flags = rd32(p_entry + 4); p_ph->offset = rd64(p_entry + 8); p_ph->vaddr = rd64(p_entry + 16);
        p_ph->paddr = rd64(p_entry + 24); p_ph->filesz = rd64(p_entry + 32); p_ph->memsz = rd64(p_entry + 40); p_ph->align = rd64(p_entry + 48);
    }
}

/* The ELF inside a SELF, rebuilt with each program header's bytes at its offset.
 * A SELF leaves out metadata headers no loader reads; those stay zero, the loadable ones must be
 * there. */
void loader_parse_self(const unsigned char *p_data, size_t cb_data, LoaderModule *p_module) {
    uint64_t phoff, end = 0;
    if (!memcmp(loader_span(p_data, cb_data, 0, 4), "\x7f" "ELF", 4)) {
        parse_headers(p_data, cb_data, 0, p_module, &phoff);
        end = add_sat(phoff, p_module->phnum * 56u);
        for (unsigned i = 0; i < p_module->phnum; ++i)
            if (add_sat(p_module->p_ph[i].offset, p_module->p_ph[i].filesz) > end) end = add_sat(p_module->p_ph[i].offset, p_module->p_ph[i].filesz);
        if (end > IMAGE_LIMIT) loader_fail("probe image exceeds 512 MiB limit");
        loader_span(p_data, cb_data, 0, end);
        if (!(p_module->p_elf = malloc(end ? end : 1))) loader_fail("allocation failed");
        memcpy(p_module->p_elf, p_data, end);
        p_module->cb_elf = end;
        return;
    }
    if (memcmp(p_data, "O\x15=\x1d", 4))
        loader_fail("expected PS4 SELF or ELF (eboot.bin is neither: an encrypted or damaged dump?)");
    unsigned c_segments = rd16(loader_span(p_data, cb_data, 24, 2));
    uint64_t base = 32 + c_segments * 32u;
    parse_headers(p_data, cb_data, base, p_module, &phoff);
    for (unsigned i = 0; i < p_module->phnum; ++i)
        if (add_sat(p_module->p_ph[i].offset, p_module->p_ph[i].filesz) > end) end = add_sat(p_module->p_ph[i].offset, p_module->p_ph[i].filesz);
    if (end > IMAGE_LIMIT) loader_fail("probe image exceeds 512 MiB limit");
    uint64_t header_end = add_sat(phoff, p_module->phnum * 56u);
    const unsigned char *p_headers = loader_span(p_data, cb_data, base, header_end);
    p_module->cb_elf = end > header_end ? end : header_end;
    if (!(p_module->p_elf = calloc(p_module->cb_elf ? p_module->cb_elf : 1, 1))) loader_fail("allocation failed");
    memcpy(p_module->p_elf, p_headers, header_end);
    uint64_t (*p_covered)[2] = calloc(c_segments + 1, sizeof(*p_covered));
    if (!p_covered) loader_fail("allocation failed");
    p_covered[0][0] = 0; p_covered[0][1] = header_end;
    unsigned ncovered = 1;
    for (unsigned i = 0; i < c_segments; ++i) {
        const unsigned char *p_entry = loader_span(p_data, cb_data, 32 + i * 32u, 32);
        uint64_t flags = rd64(p_entry), off_segment = rd64(p_entry + 8), length = rd64(p_entry + 16), memory = rd64(p_entry + 24);
        if (!(flags & 0x800)) continue;
        if (flags & 10) loader_fail("encrypted/compressed SELF segment is unsupported");
        unsigned i_ph = (flags >> 20) & 4095;
        if (i_ph >= p_module->phnum) loader_fail("invalid segment index");
        const LoaderPh *p_ph = &p_module->p_ph[i_ph];
        if (length != p_ph->filesz || length != memory) loader_fail("unsupported blocked segment layout");
        memcpy(p_module->p_elf + p_ph->offset, loader_span(p_data, cb_data, off_segment, length), length);
        p_covered[ncovered][0] = p_ph->offset; p_covered[ncovered][1] = p_ph->offset + length;
        ++ncovered;
    }
    for (unsigned i = 0; i < p_module->phnum; ++i) {
        const LoaderPh *p_ph = &p_module->p_ph[i];
        if (!p_ph->filesz) continue;
        int is_present = 0;
        for (unsigned c = 0; c < ncovered && !is_present; ++c)
            is_present = p_covered[c][0] <= p_ph->offset && p_ph->offset + p_ph->filesz <= p_covered[c][1];
        if (!is_present && (p_ph->type == 1 || p_ph->type == PT_DYNAMIC || p_ph->type == PT_SCE_DYNLIBDATA || p_ph->type == PT_SCE_RELRO))
            loader_fail("required segment %u is unavailable", i);
    }
    free(p_covered);
}

const LoaderPh *loader_find_ph(const LoaderModule *p_module, uint32_t type) {
    for (unsigned i = 0; i < p_module->phnum; ++i)
        if (p_module->p_ph[i].type == type) return &p_module->p_ph[i];
    return NULL;
}

/* The whole PT_DYNAMIC, or up to DT_NULL (stop_at_null), as each script read it. */
static void load_dynamic(LoaderModule *p_module, int stop_at_null) {
    const LoaderPh *p_dynamic_ph = loader_find_ph(p_module, PT_DYNAMIC);
    if (!p_dynamic_ph) loader_fail("no PT_DYNAMIC");
    size_t c_capacity = (size_t)(p_dynamic_ph->filesz / 16 + 1);
    if (p_dynamic_ph->filesz > p_module->cb_elf || !(p_module->p_dynamic = calloc(c_capacity, sizeof(*p_module->p_dynamic))))
        loader_fail("out of bounds: dynamic section");
    p_module->c_dynamic_entries = 0;
    for (uint64_t pos = 0; pos < p_dynamic_ph->filesz; pos += 16) {
        const unsigned char *p_entry = loader_span(p_module->p_elf, p_module->cb_elf, add_sat(p_dynamic_ph->offset, pos), 16);
        if (stop_at_null && !rd64(p_entry)) break;
        p_module->p_dynamic[p_module->c_dynamic_entries].tag = rd64(p_entry);
        p_module->p_dynamic[p_module->c_dynamic_entries++].value = rd64(p_entry + 8);
    }
}

int loader_tag(const LoaderModule *p_module, uint64_t tag, uint64_t *p_value) {
    for (size_t i = p_module->c_dynamic_entries; i--;)
        if (p_module->p_dynamic[i].tag == tag) { *p_value = p_module->p_dynamic[i].value; return 1; }
    return 0;
}
static uint64_t required_tag(const LoaderModule *p_module, uint64_t tag) {
    uint64_t value;
    if (!loader_tag(p_module, tag, &value)) loader_fail("dynamic tag %#llx missing", (unsigned long long)tag);
    return value;
}

static const unsigned char *blob_table(const LoaderModule *p_module, uint64_t off_tag, uint64_t cb_tag, size_t *p_size) {
    const LoaderPh *p_dynlib_ph = loader_find_ph(p_module, PT_SCE_DYNLIBDATA);
    if (!p_dynlib_ph) loader_fail("no PT_SCE_DYNLIBDATA");
    const unsigned char *p_blob = loader_span(p_module->p_elf, p_module->cb_elf, p_dynlib_ph->offset, p_dynlib_ph->filesz);
    uint64_t off_blob = required_tag(p_module, off_tag), length = required_tag(p_module, cb_tag);
    *p_size = (size_t)length;
    return loader_span(p_blob, (size_t)p_dynlib_ph->filesz, off_blob, length);
}

static const char *string_at(const LoaderModule *p_module, uint64_t off_string) {
    loader_span(p_module->p_strings, p_module->cb_strings, off_string, 1);
    const unsigned char *p_string = p_module->p_strings + off_string, *p_end = memchr(p_string, 0, p_module->cb_strings - off_string);
    if (!p_end) loader_fail("unterminated string at %#llx", (unsigned long long)off_string);
    for (const unsigned char *p_char = p_string; p_char < p_end; ++p_char)
        if (*p_char >= 0x80) loader_fail("non-ASCII string at %#llx", (unsigned long long)off_string);
    return (const char *)p_string;
}

void loader_encode_id(unsigned value, char sz_key[4]) {
    static const char s_sz_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    char sz_reversed[4];
    int c_digits = 0;
    do { sz_reversed[c_digits++] = s_sz_alphabet[value & 63]; value >>= 6; } while (value);
    for (int i = 0; i < c_digits; ++i) sz_key[i] = sz_reversed[c_digits - 1 - i];
    sz_key[c_digits] = 0;
}

static void add_id(LoaderId **pp_table, size_t *p_count, uint64_t value, const char *sz_name) {
    char sz_key[4];
    loader_encode_id((unsigned)(value >> 48), sz_key);
    unsigned version = (unsigned)(value >> 32) & 65535;
    for (size_t i = 0; i < *p_count; ++i)
        if (!strcmp((*pp_table)[i].sz_key, sz_key)) {
            if (strcmp((*pp_table)[i].sz_name, sz_name) || (*pp_table)[i].version != version)
                loader_fail("conflicting module/library IDs");
            return;
        }
    *pp_table = realloc(*pp_table, (*p_count + 1) * sizeof(**pp_table));
    if (!*pp_table) loader_fail("allocation failed");
    LoaderId *p_id = &(*pp_table)[(*p_count)++];
    memcpy(p_id->sz_key, sz_key, 4);
    p_id->sz_name = sz_name;
    p_id->version = version;
}

static const LoaderId *find_id(const LoaderId *p_table, size_t c_table, const char *sz_key, size_t length) {
    for (size_t i = 0; i < c_table; ++i)
        if (strlen(p_table[i].sz_key) == length && !memcmp(p_table[i].sz_key, sz_key, length)) return &p_table[i];
    loader_fail("unknown library or module id %.*s", (int)length, sz_key);
}

static LoaderIdentity identity(const LoaderModule *p_module, const char *sz_name) {
    LoaderIdentity id = {0};
    const char *sz_first = strchr(sz_name, '#'), *sz_second = sz_first ? strchr(sz_first + 1, '#') : NULL;
    if (!sz_second || strchr(sz_second + 1, '#')) return id; /* plain names such as module_start */
    id.is_valid = 1;
    id.sz_nid = sz_name;
    id.nid_length = (size_t)(sz_first - sz_name);
    id.p_library = find_id(p_module->p_libraries, p_module->c_libraries, sz_first + 1, (size_t)(sz_second - sz_first - 1));
    id.p_module = find_id(p_module->p_modules, p_module->c_modules, sz_second + 1, strlen(sz_second + 1));
    return id;
}

static void load_symbols(LoaderModule *p_module, int identities) {
    size_t cb_table;
    const unsigned char *p_table = blob_table(p_module, DT_SCE_SYMTAB, DT_SCE_SYMTABSZ, &cb_table);
    if (cb_table % 24) loader_fail("bad symbol table size");
    p_module->c_symbols = cb_table / 24;
    if (!(p_module->p_symbols = calloc(p_module->c_symbols ? p_module->c_symbols : 1, sizeof(*p_module->p_symbols)))) loader_fail("allocation failed");
    for (size_t i = 0; i < p_module->c_symbols; ++i) {
        const unsigned char *p_entry = p_table + i * 24;
        LoaderSymbol *p_symbol = &p_module->p_symbols[i];
        p_symbol->sz_name = string_at(p_module, rd32(p_entry));
        p_symbol->type = p_entry[4] & 15; p_symbol->binding = p_entry[4] >> 4; p_symbol->section = rd16(p_entry + 6);
        p_symbol->value = rd64(p_entry + 8); p_symbol->cb_symbol = rd64(p_entry + 16);
        if (identities && p_symbol->sz_name[0]) p_symbol->identity = identity(p_module, p_symbol->sz_name);
    }
}

static void load_strings(LoaderModule *p_module) {
    p_module->p_strings = blob_table(p_module, DT_SCE_STRTAB, DT_SCE_STRSZ, &p_module->cb_strings);
}

/* A module's ids, symbols with their identities, and both relocation tables. */
void loader_module(const char *sz_path, LoaderModule *p_module) {
    size_t cb_data;
    unsigned char *p_data = loader_read_file(sz_path, &cb_data);
    memset(p_module, 0, sizeof(*p_module));
    loader_parse_self(p_data, cb_data, p_module);
    free(p_data);
    load_dynamic(p_module, 1);
    load_strings(p_module);
    for (size_t i = 0; i < p_module->c_dynamic_entries; ++i) {
        uint64_t tag = p_module->p_dynamic[i].tag, value = p_module->p_dynamic[i].value;
        if (tag == 0x61000013 || tag == 0x61000015)
            add_id(&p_module->p_libraries, &p_module->c_libraries, value, string_at(p_module, value & 0xffffffff));
        else if (tag == 0x6100000d || tag == 0x6100000f)
            add_id(&p_module->p_modules, &p_module->c_modules, value, string_at(p_module, value & 0xffffffff));
    }
    load_symbols(p_module, 1);
    static const uint64_t s_tables[2][2] = {{DT_SCE_JMPREL, DT_SCE_PLTRELSZ}, {DT_SCE_RELA, DT_SCE_RELASZ}};
    const unsigned char *rela[2];
    size_t rela_size[2];
    for (int t = 0; t < 2; ++t) {
        rela[t] = blob_table(p_module, s_tables[t][0], s_tables[t][1], &rela_size[t]);
        if (rela_size[t] % 24) loader_fail("bad relocation table size");
    }
    p_module->c_relas = rela_size[0] / 24 + rela_size[1] / 24;
    if (!(p_module->p_relas = calloc(p_module->c_relas ? p_module->c_relas : 1, sizeof(*p_module->p_relas)))) loader_fail("allocation failed");
    size_t i_rela = 0;
    for (int t = 0; t < 2; ++t)
        for (size_t pos = 0; pos < rela_size[t]; pos += 24) {
            uint64_t info = rd64(rela[t] + pos + 8);
            p_module->p_relas[i_rela].target = rd64(rela[t] + pos);
            p_module->p_relas[i_rela].kind = (uint32_t)info;
            p_module->p_relas[i_rela].symbol = (uint32_t)(info >> 32);
            p_module->p_relas[i_rela++].addend = (int64_t)rd64(rela[t] + pos + 16);
        }
}

/* libc.prx's _init_env must be exactly RET before the native libc may run its initializers: the
 * runtime gets this as its capability bit. Its PT_DYNAMIC is read past DT_NULL, as the Python loader
 * this replaces did, since every title was validated that way. */
int loader_init_env_is_ret(const char *sz_libc_path) {
    static const char s_sz_init_env[] = "bzQExy189ZI", s_sz_ios_base_init[] = "sqWytnhYdEg";
    size_t cb_data;
    unsigned char *p_data = loader_read_file(sz_libc_path, &cb_data);
    LoaderModule m = {0};
    loader_parse_self(p_data, cb_data, &m);
    free(p_data);
    load_dynamic(&m, 0);
    load_strings(&m);
    load_symbols(&m, 0);
    int is_ret = 0;
    for (size_t i = 0; i < m.c_symbols; ++i) {
        const LoaderSymbol *p_symbol = &m.p_symbols[i];
        size_t key = strcspn(p_symbol->sz_name, "#");
        int env = key == 11 && !memcmp(p_symbol->sz_name, s_sz_init_env, 11);
        if (!(env || (key == 11 && !memcmp(p_symbol->sz_name, s_sz_ios_base_init, 11))) || !p_symbol->section || p_symbol->type != 2) continue;
        const LoaderPh *p_ph = NULL;
        for (unsigned j = 0; j < m.phnum && !p_ph; ++j)
            if (m.p_ph[j].type == 1 && (m.p_ph[j].flags & 1) && m.p_ph[j].vaddr <= p_symbol->value && p_symbol->value - m.p_ph[j].vaddr < m.p_ph[j].filesz)
                p_ph = &m.p_ph[j];
        if (!p_ph) loader_fail("libc symbol %s outside its code", p_symbol->sz_name);
        const unsigned char *p_code = loader_span(m.p_elf, m.cb_elf, p_ph->offset + p_symbol->value - p_ph->vaddr, p_symbol->cb_symbol);
        if (env) is_ret = p_symbol->cb_symbol == 1 && p_code[0] == 0xc3;
    }
    free(m.p_elf); free(m.p_ph); free(m.p_dynamic); free(m.p_symbols);
    return is_ret;
}

/* PT_GNU_EH_FRAME and the .eh_frame it points at, for sceKernelGetModuleInfoForUnwind. */
void loader_eh_frame(const LoaderModule *p_module, uint64_t base, RuntimeModule *p_out) {
    const LoaderPh *p_hdr = loader_find_ph(p_module, PT_GNU_EH_FRAME);
    p_out->eh_frame_hdr = p_out->cb_eh_frame_hdr = p_out->eh_frame = p_out->cb_eh_frame = 0;
    if (!p_hdr || p_hdr->filesz < 8) return;
    const unsigned char *p_data = loader_span(p_module->p_elf, p_module->cb_elf, p_hdr->offset, 8);
    p_out->eh_frame_hdr = base + p_hdr->vaddr;
    p_out->cb_eh_frame_hdr = p_hdr->memsz;
    if (p_data[0] != 1 || p_data[1] != 0x1b) return; /* version 1, eh_frame_ptr DW_EH_PE_pcrel|sdata4 */
    int64_t frame = (int64_t)p_hdr->vaddr + 4 + (int32_t)rd32(p_data + 4);
    int64_t end = 0;
    for (unsigned i = 0; i < p_module->phnum; ++i)
        if (p_module->p_ph[i].type == 1 && (int64_t)(p_module->p_ph[i].vaddr + p_module->p_ph[i].memsz) > end) end = (int64_t)(p_module->p_ph[i].vaddr + p_module->p_ph[i].memsz);
    /* As shadPS4: .eh_frame_hdr follows .eh_frame, else .eh_frame runs to the image end. */
    int64_t length = (int64_t)p_hdr->vaddr > frame ? (int64_t)p_hdr->vaddr - frame : end - frame;
    if ((int64_t)base + frame < 0) loader_fail("eh_frame before the image");
    p_out->eh_frame = base + (uint64_t)frame;
    p_out->cb_eh_frame = length > 0 ? (uint64_t)length : 0;
}
