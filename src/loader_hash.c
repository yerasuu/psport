/* The identity of a game's executable: the SHA-256 of eboot.bin's loadable segments placed at their
 * addresses. It is the same whatever tool dumped the game, since the SELF headers around the
 * segments, which differ between dumpers, are left out. */
#define _GNU_SOURCE
#include "loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PT_SCE_RELRO 0x61000010

typedef struct {
    uint32_t state[8];
    unsigned char block[64];
    size_t cb_block;
    uint64_t cb_total;
} Sha256;

static const uint32_t s_round_constants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotate_right(uint32_t value, unsigned bits) { return value >> bits | value << (32 - bits); }

static void sha256_block(Sha256 *p_sha, const unsigned char *p_block) {
    uint32_t schedule[64], s[8];
    for (int i = 0; i < 16; ++i)
        schedule[i] = (uint32_t)p_block[4 * i] << 24 | (uint32_t)p_block[4 * i + 1] << 16 |
                      (uint32_t)p_block[4 * i + 2] << 8 | p_block[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotate_right(schedule[i - 15], 7) ^ rotate_right(schedule[i - 15], 18) ^ schedule[i - 15] >> 3;
        uint32_t s1 = rotate_right(schedule[i - 2], 17) ^ rotate_right(schedule[i - 2], 19) ^ schedule[i - 2] >> 10;
        schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }
    memcpy(s, p_sha->state, sizeof(s));
    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = s[7] + (rotate_right(s[4], 6) ^ rotate_right(s[4], 11) ^ rotate_right(s[4], 25)) +
                      ((s[4] & s[5]) ^ (~s[4] & s[6])) + s_round_constants[i] + schedule[i];
        uint32_t t2 = (rotate_right(s[0], 2) ^ rotate_right(s[0], 13) ^ rotate_right(s[0], 22)) +
                      ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
        memmove(s + 1, s, 7 * sizeof(*s));
        s[4] += t1;
        s[0] = t1 + t2;
    }
    for (int i = 0; i < 8; ++i) p_sha->state[i] += s[i];
}

static void sha256_update(Sha256 *p_sha, const unsigned char *p_data, size_t cb_data) {
    p_sha->cb_total += cb_data;
    while (cb_data) {
        size_t cb_take = 64 - p_sha->cb_block < cb_data ? 64 - p_sha->cb_block : cb_data;
        if (!p_sha->cb_block && cb_data >= 64) {
            sha256_block(p_sha, p_data);
            cb_take = 64;
        } else {
            memcpy(p_sha->block + p_sha->cb_block, p_data, cb_take);
            if ((p_sha->cb_block += cb_take) == 64) {
                sha256_block(p_sha, p_sha->block);
                p_sha->cb_block = 0;
            }
        }
        p_data += cb_take;
        cb_data -= cb_take;
    }
}

static void sha256_finish(Sha256 *p_sha, unsigned char *p_out_digest) {
    uint64_t bits = p_sha->cb_total * 8;
    static const unsigned char s_padding[64] = {0x80};
    sha256_update(p_sha, s_padding, p_sha->cb_block < 56 ? 56 - p_sha->cb_block : 120 - p_sha->cb_block);
    unsigned char length[8];
    for (int i = 0; i < 8; ++i) length[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha256_update(p_sha, length, 8);
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) p_out_digest[4 * i + j] = (unsigned char)(p_sha->state[i] >> (24 - 8 * j));
}

void loader_eboot_sha256(const char *sz_game, unsigned char *p_out_digest) {
    char sz_path[4096];
    if ((size_t)snprintf(sz_path, sizeof(sz_path), "%s/eboot.bin", sz_game) >= sizeof(sz_path))
        loader_fail("path too long: %s", sz_game);
    size_t cb_file;
    unsigned char *p_file = loader_read_file(sz_path, &cb_file);
    LoaderModule eboot = {0};
    loader_file = sz_path;
    loader_parse_self(p_file, cb_file, &eboot);
    free(p_file);
    uint64_t cb_image = 0;
    for (unsigned i = 0; i < eboot.phnum; ++i) {
        const LoaderPh *p_ph = &eboot.ph[i];
        if ((p_ph->type == 1 || p_ph->type == PT_SCE_RELRO) && p_ph->vaddr + p_ph->memsz > cb_image)
            cb_image = p_ph->vaddr + p_ph->memsz;
    }
    if (cb_image > 512u * 1024 * 1024) loader_fail("invalid memory image");
    unsigned char *p_image = calloc(cb_image ? cb_image : 1, 1);
    if (!p_image) loader_fail("allocation failed");
    for (unsigned i = 0; i < eboot.phnum; ++i) {
        const LoaderPh *p_ph = &eboot.ph[i];
        if ((p_ph->type != 1 && p_ph->type != PT_SCE_RELRO) || p_ph->filesz > p_ph->memsz) continue;
        memcpy(p_image + p_ph->vaddr, loader_span(eboot.elf, eboot.elf_size, p_ph->offset, p_ph->filesz), p_ph->filesz);
    }
    loader_file = NULL;
    Sha256 sha = {.state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
                            0x5be0cd19}};
    sha256_update(&sha, p_image, cb_image);
    sha256_finish(&sha, p_out_digest);
    free(p_image);
    free(eboot.elf);
    free(eboot.ph);
}
