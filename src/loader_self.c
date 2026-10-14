/* PS4 SELF and plain ELF files for the loader: program headers, the dynamic section and the SCE
 * dynamic tables (strings, symbols, relocations, library and module ids), every offset checked
 * before use. Decrypted dumps only: an encrypted or compressed SELF segment is an error. */
#define _GNU_SOURCE
#include "loader.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

const char *loader_file;

void loader_fail(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fputs("ERROR: ", stderr);
    if (loader_file) fprintf(stderr, "%s: ", loader_file);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
    exit(1);
}

unsigned char *loader_read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    struct stat st;
    if (!f || fstat(fileno(f), &st) || !S_ISREG(st.st_mode)) loader_fail("cannot read %s", path);
    unsigned char *data = malloc(st.st_size ? (size_t)st.st_size : 1);
    if (!data || fread(data, 1, (size_t)st.st_size, f) != (size_t)st.st_size) loader_fail("cannot read %s", path);
    fclose(f);
    *size = (size_t)st.st_size;
    return data;
}

const unsigned char *loader_span(const unsigned char *data, size_t size, uint64_t offset, uint64_t length) {
    if (offset > size || length > size - offset)
        loader_fail("out of bounds: %#llx+%#llx / %#zx", (unsigned long long)offset, (unsigned long long)length, size);
    return data + offset;
}
static uint16_t rd16(const unsigned char *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const unsigned char *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint64_t add_sat(uint64_t a, uint64_t b) { return a > UINT64_MAX - b ? UINT64_MAX : a + b; }

static void parse_headers(const unsigned char *data, size_t size, uint64_t base, LoaderModule *m, uint64_t *phoff) {
    const unsigned char *h = loader_span(data, size, base, 64);
    if (memcmp(h, "\x7f" "ELF\x02\x01\x01", 7) || rd16(h + 18) != 62) loader_fail("expected little-endian x86-64 ELF");
    m->phnum = rd16(h + 56);
    if (rd16(h + 54) != 56 || !m->phnum || m->phnum >= 256) loader_fail("unsupported program headers");
    m->entry = rd64(h + 24);
    *phoff = rd64(h + 32);
    if (!(m->ph = calloc(m->phnum, sizeof(*m->ph)))) loader_fail("allocation failed");
    for (unsigned i = 0; i < m->phnum; ++i) {
        const unsigned char *p = loader_span(data, size, add_sat(add_sat(base, *phoff), i * 56u), 56);
        LoaderPh *x = &m->ph[i];
        x->type = rd32(p); x->flags = rd32(p + 4); x->offset = rd64(p + 8); x->vaddr = rd64(p + 16);
        x->paddr = rd64(p + 24); x->filesz = rd64(p + 32); x->memsz = rd64(p + 40); x->align = rd64(p + 48);
    }
}

/* The ELF inside a SELF, rebuilt with each program header's bytes at its offset.
 * A SELF leaves out metadata headers no loader reads; those stay zero, the loadable ones must be
 * there. */
void loader_parse_self(const unsigned char *data, size_t size, LoaderModule *m) {
    uint64_t phoff, end = 0;
    if (!memcmp(loader_span(data, size, 0, 4), "\x7f" "ELF", 4)) {
        parse_headers(data, size, 0, m, &phoff);
        end = add_sat(phoff, m->phnum * 56u);
        for (unsigned i = 0; i < m->phnum; ++i)
            if (add_sat(m->ph[i].offset, m->ph[i].filesz) > end) end = add_sat(m->ph[i].offset, m->ph[i].filesz);
        if (end > IMAGE_LIMIT) loader_fail("probe image exceeds 512 MiB limit");
        loader_span(data, size, 0, end);
        if (!(m->elf = malloc(end ? end : 1))) loader_fail("allocation failed");
        memcpy(m->elf, data, end);
        m->elf_size = end;
        return;
    }
    if (memcmp(data, "O\x15=\x1d", 4))
        loader_fail("expected PS4 SELF or ELF (eboot.bin is neither: an encrypted or damaged dump?)");
    unsigned count = rd16(loader_span(data, size, 24, 2));
    uint64_t base = 32 + count * 32u;
    parse_headers(data, size, base, m, &phoff);
    for (unsigned i = 0; i < m->phnum; ++i)
        if (add_sat(m->ph[i].offset, m->ph[i].filesz) > end) end = add_sat(m->ph[i].offset, m->ph[i].filesz);
    if (end > IMAGE_LIMIT) loader_fail("probe image exceeds 512 MiB limit");
    uint64_t header_end = add_sat(phoff, m->phnum * 56u);
    const unsigned char *headers = loader_span(data, size, base, header_end);
    m->elf_size = end > header_end ? end : header_end;
    if (!(m->elf = calloc(m->elf_size ? m->elf_size : 1, 1))) loader_fail("allocation failed");
    memcpy(m->elf, headers, header_end);
    uint64_t (*covered)[2] = calloc(count + 1, sizeof(*covered));
    if (!covered) loader_fail("allocation failed");
    covered[0][0] = 0; covered[0][1] = header_end;
    unsigned ncovered = 1;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned char *s = loader_span(data, size, 32 + i * 32u, 32);
        uint64_t flags = rd64(s), offset = rd64(s + 8), length = rd64(s + 16), memory = rd64(s + 24);
        if (!(flags & 0x800)) continue;
        if (flags & 10) loader_fail("encrypted/compressed SELF segment is unsupported");
        unsigned index = (flags >> 20) & 4095;
        if (index >= m->phnum) loader_fail("invalid segment index");
        const LoaderPh *p = &m->ph[index];
        if (length != p->filesz || length != memory) loader_fail("unsupported blocked segment layout");
        memcpy(m->elf + p->offset, loader_span(data, size, offset, length), length);
        covered[ncovered][0] = p->offset; covered[ncovered][1] = p->offset + length;
        ++ncovered;
    }
    for (unsigned i = 0; i < m->phnum; ++i) {
        const LoaderPh *p = &m->ph[i];
        if (!p->filesz) continue;
        int present = 0;
        for (unsigned c = 0; c < ncovered && !present; ++c)
            present = covered[c][0] <= p->offset && p->offset + p->filesz <= covered[c][1];
        if (!present && (p->type == 1 || p->type == PT_DYNAMIC || p->type == PT_SCE_DYNLIBDATA || p->type == PT_SCE_RELRO))
            loader_fail("required segment %u is unavailable", i);
    }
    free(covered);
}

const LoaderPh *loader_find_ph(const LoaderModule *m, uint32_t type) {
    for (unsigned i = 0; i < m->phnum; ++i)
        if (m->ph[i].type == type) return &m->ph[i];
    return NULL;
}

/* The whole PT_DYNAMIC, or up to DT_NULL (stop_at_null), as each script read it. */
static void load_dynamic(LoaderModule *m, int stop_at_null) {
    const LoaderPh *dp = loader_find_ph(m, PT_DYNAMIC);
    if (!dp) loader_fail("no PT_DYNAMIC");
    size_t capacity = (size_t)(dp->filesz / 16 + 1);
    if (dp->filesz > m->elf_size || !(m->dynamic = calloc(capacity, sizeof(*m->dynamic))))
        loader_fail("out of bounds: dynamic section");
    m->dynamic_count = 0;
    for (uint64_t pos = 0; pos < dp->filesz; pos += 16) {
        const unsigned char *e = loader_span(m->elf, m->elf_size, add_sat(dp->offset, pos), 16);
        if (stop_at_null && !rd64(e)) break;
        m->dynamic[m->dynamic_count].tag = rd64(e);
        m->dynamic[m->dynamic_count++].value = rd64(e + 8);
    }
}

int loader_tag(const LoaderModule *m, uint64_t tag, uint64_t *value) {
    for (size_t i = m->dynamic_count; i--;)
        if (m->dynamic[i].tag == tag) { *value = m->dynamic[i].value; return 1; }
    return 0;
}
static uint64_t required_tag(const LoaderModule *m, uint64_t tag) {
    uint64_t value;
    if (!loader_tag(m, tag, &value)) loader_fail("dynamic tag %#llx missing", (unsigned long long)tag);
    return value;
}

static const unsigned char *blob_table(const LoaderModule *m, uint64_t offset_tag, uint64_t size_tag, size_t *size) {
    const LoaderPh *lp = loader_find_ph(m, PT_SCE_DYNLIBDATA);
    if (!lp) loader_fail("no PT_SCE_DYNLIBDATA");
    const unsigned char *blob = loader_span(m->elf, m->elf_size, lp->offset, lp->filesz);
    uint64_t offset = required_tag(m, offset_tag), length = required_tag(m, size_tag);
    *size = (size_t)length;
    return loader_span(blob, (size_t)lp->filesz, offset, length);
}

static const char *string_at(const LoaderModule *m, uint64_t offset) {
    loader_span(m->strings, m->strings_size, offset, 1);
    const unsigned char *s = m->strings + offset, *end = memchr(s, 0, m->strings_size - offset);
    if (!end) loader_fail("unterminated string at %#llx", (unsigned long long)offset);
    for (const unsigned char *c = s; c < end; ++c)
        if (*c >= 0x80) loader_fail("non-ASCII string at %#llx", (unsigned long long)offset);
    return (const char *)s;
}

void loader_encode_id(unsigned value, char key[4]) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    char reversed[4];
    int n = 0;
    do { reversed[n++] = alphabet[value & 63]; value >>= 6; } while (value);
    for (int i = 0; i < n; ++i) key[i] = reversed[n - 1 - i];
    key[n] = 0;
}

static void add_id(LoaderId **table, size_t *count, uint64_t value, const char *name) {
    char key[4];
    loader_encode_id((unsigned)(value >> 48), key);
    unsigned version = (unsigned)(value >> 32) & 65535;
    for (size_t i = 0; i < *count; ++i)
        if (!strcmp((*table)[i].key, key)) {
            if (strcmp((*table)[i].name, name) || (*table)[i].version != version)
                loader_fail("conflicting module/library IDs");
            return;
        }
    *table = realloc(*table, (*count + 1) * sizeof(**table));
    if (!*table) loader_fail("allocation failed");
    LoaderId *id = &(*table)[(*count)++];
    memcpy(id->key, key, 4);
    id->name = name;
    id->version = version;
}

static const LoaderId *find_id(const LoaderId *table, size_t count, const char *key, size_t length) {
    for (size_t i = 0; i < count; ++i)
        if (strlen(table[i].key) == length && !memcmp(table[i].key, key, length)) return &table[i];
    loader_fail("unknown library or module id %.*s", (int)length, key);
}

static LoaderIdentity identity(const LoaderModule *m, const char *name) {
    LoaderIdentity id = {0};
    const char *first = strchr(name, '#'), *second = first ? strchr(first + 1, '#') : NULL;
    if (!second || strchr(second + 1, '#')) return id; /* plain names such as module_start */
    id.valid = 1;
    id.nid = name;
    id.nid_length = (size_t)(first - name);
    id.library = find_id(m->libraries, m->library_count, first + 1, (size_t)(second - first - 1));
    id.module = find_id(m->modules, m->module_count, second + 1, strlen(second + 1));
    return id;
}

static void load_symbols(LoaderModule *m, int identities) {
    size_t size;
    const unsigned char *table = blob_table(m, DT_SCE_SYMTAB, DT_SCE_SYMTABSZ, &size);
    if (size % 24) loader_fail("bad symbol table size");
    m->symbol_count = size / 24;
    if (!(m->symbols = calloc(m->symbol_count ? m->symbol_count : 1, sizeof(*m->symbols)))) loader_fail("allocation failed");
    for (size_t i = 0; i < m->symbol_count; ++i) {
        const unsigned char *e = table + i * 24;
        LoaderSymbol *s = &m->symbols[i];
        s->name = string_at(m, rd32(e));
        s->type = e[4] & 15; s->binding = e[4] >> 4; s->section = rd16(e + 6);
        s->value = rd64(e + 8); s->size = rd64(e + 16);
        if (identities && s->name[0]) s->identity = identity(m, s->name);
    }
}

static void load_strings(LoaderModule *m) {
    m->strings = blob_table(m, DT_SCE_STRTAB, DT_SCE_STRSZ, &m->strings_size);
}

/* A module's ids, symbols with their identities, and both relocation tables. */
void loader_module(const char *path, LoaderModule *m) {
    size_t size;
    unsigned char *data = loader_read_file(path, &size);
    memset(m, 0, sizeof(*m));
    loader_parse_self(data, size, m);
    free(data);
    load_dynamic(m, 1);
    load_strings(m);
    for (size_t i = 0; i < m->dynamic_count; ++i) {
        uint64_t tag = m->dynamic[i].tag, value = m->dynamic[i].value;
        if (tag == 0x61000013 || tag == 0x61000015)
            add_id(&m->libraries, &m->library_count, value, string_at(m, value & 0xffffffff));
        else if (tag == 0x6100000d || tag == 0x6100000f)
            add_id(&m->modules, &m->module_count, value, string_at(m, value & 0xffffffff));
    }
    load_symbols(m, 1);
    static const uint64_t tables[2][2] = {{DT_SCE_JMPREL, DT_SCE_PLTRELSZ}, {DT_SCE_RELA, DT_SCE_RELASZ}};
    const unsigned char *rela[2];
    size_t rela_size[2];
    for (int t = 0; t < 2; ++t) {
        rela[t] = blob_table(m, tables[t][0], tables[t][1], &rela_size[t]);
        if (rela_size[t] % 24) loader_fail("bad relocation table size");
    }
    m->rela_count = rela_size[0] / 24 + rela_size[1] / 24;
    if (!(m->relas = calloc(m->rela_count ? m->rela_count : 1, sizeof(*m->relas)))) loader_fail("allocation failed");
    size_t n = 0;
    for (int t = 0; t < 2; ++t)
        for (size_t pos = 0; pos < rela_size[t]; pos += 24) {
            uint64_t info = rd64(rela[t] + pos + 8);
            m->relas[n].target = rd64(rela[t] + pos);
            m->relas[n].kind = (uint32_t)info;
            m->relas[n].symbol = (uint32_t)(info >> 32);
            m->relas[n++].addend = (int64_t)rd64(rela[t] + pos + 16);
        }
}

/* libc.prx's _init_env must be exactly RET before the native libc may run its initializers: the
 * runtime gets this as its capability bit. Its PT_DYNAMIC is read past DT_NULL, as the Python loader
 * this replaces did, since every title was validated that way. */
int loader_init_env_is_ret(const char *libc_path) {
    static const char init_env[] = "bzQExy189ZI", ios_base_init[] = "sqWytnhYdEg";
    size_t size;
    unsigned char *data = loader_read_file(libc_path, &size);
    LoaderModule m = {0};
    loader_parse_self(data, size, &m);
    free(data);
    load_dynamic(&m, 0);
    load_strings(&m);
    load_symbols(&m, 0);
    int is_ret = 0;
    for (size_t i = 0; i < m.symbol_count; ++i) {
        const LoaderSymbol *s = &m.symbols[i];
        size_t key = strcspn(s->name, "#");
        int env = key == 11 && !memcmp(s->name, init_env, 11);
        if (!(env || (key == 11 && !memcmp(s->name, ios_base_init, 11))) || !s->section || s->type != 2) continue;
        const LoaderPh *p = NULL;
        for (unsigned j = 0; j < m.phnum && !p; ++j)
            if (m.ph[j].type == 1 && (m.ph[j].flags & 1) && m.ph[j].vaddr <= s->value && s->value - m.ph[j].vaddr < m.ph[j].filesz)
                p = &m.ph[j];
        if (!p) loader_fail("libc symbol %s outside its code", s->name);
        const unsigned char *code = loader_span(m.elf, m.elf_size, p->offset + s->value - p->vaddr, s->size);
        if (env) is_ret = s->size == 1 && code[0] == 0xc3;
    }
    free(m.elf); free(m.ph); free(m.dynamic); free(m.symbols);
    return is_ret;
}

/* PT_GNU_EH_FRAME and the .eh_frame it points at, for sceKernelGetModuleInfoForUnwind. */
void loader_eh_frame(const LoaderModule *m, uint64_t base, RuntimeModule *out) {
    const LoaderPh *hdr = loader_find_ph(m, PT_GNU_EH_FRAME);
    out->eh_frame_hdr = out->eh_frame_hdr_size = out->eh_frame = out->eh_frame_size = 0;
    if (!hdr || hdr->filesz < 8) return;
    const unsigned char *data = loader_span(m->elf, m->elf_size, hdr->offset, 8);
    out->eh_frame_hdr = base + hdr->vaddr;
    out->eh_frame_hdr_size = hdr->memsz;
    if (data[0] != 1 || data[1] != 0x1b) return; /* version 1, eh_frame_ptr DW_EH_PE_pcrel|sdata4 */
    int64_t frame = (int64_t)hdr->vaddr + 4 + (int32_t)rd32(data + 4);
    int64_t end = 0;
    for (unsigned i = 0; i < m->phnum; ++i)
        if (m->ph[i].type == 1 && (int64_t)(m->ph[i].vaddr + m->ph[i].memsz) > end) end = (int64_t)(m->ph[i].vaddr + m->ph[i].memsz);
    /* As shadPS4: .eh_frame_hdr follows .eh_frame, else .eh_frame runs to the image end. */
    int64_t length = (int64_t)hdr->vaddr > frame ? (int64_t)hdr->vaddr - frame : end - frame;
    if ((int64_t)base + frame < 0) loader_fail("eh_frame before the image");
    out->eh_frame = base + (uint64_t)frame;
    out->eh_frame_size = length > 0 ? (uint64_t)length : 0;
}
