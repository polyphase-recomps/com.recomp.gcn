/*
 * Calls recompiled game functions directly, without the runtime: a test bed for the recompiler's
 * instruction semantics on real game code (e.g. the C library's math against the host's).
 *
 *   call_test <main.dol> <function address> f <float arg>...     (float arguments in f1..)
 *   call_test <main.dol> <function address> i <int arg>...       (integer arguments in r3..)
 *
 * Loads the DOL's sections into guest memory, sets r1/r2/r13, calls the function and prints r3
 * and f1. Every HLE function and runtime hook is a stub that aborts (call_test_stubs.c,
 * generated from funcs.h by make_call_test_stubs.py), so only self-contained code works.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gcn_recomp.h"

typedef struct gcnr_function
{
    uint32_t addr;
    gcnr_func fn;
} gcnr_function;
extern const gcnr_function gcnr_functions[];
extern const uint32_t gcnr_function_count;
extern const uint32_t gcnr_r2, gcnr_r13;
uint32_t gcnr_gqr[8];

static uint8_t* sMem;

gcnr_func gcnr_lookup(uint32_t addr)
{
    for (uint32_t i = 0; i < gcnr_function_count; i++)
    {
        if (gcnr_functions[i].addr == addr) return gcnr_functions[i].fn;
    }
    fprintf(stderr, "no function at %08X\n", addr);
    exit(2);
}

gcnr_func gcnr_lookup_from(uint32_t addr, const gcnr_ctx* c)
{
    (void)c;
    return gcnr_lookup(addr);
}

static uint32_t be32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: call_test main.dol ADDR f|i args...\n");
        return 2;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* dol = (uint8_t*)malloc((size_t)n);
    fread(dol, 1, (size_t)n, f);
    fclose(f);
    sMem = (uint8_t*)calloc(1, GCNR_LC_OFFSET + GCNR_LC_MASK + 1 + 16);
    for (int s = 0; s < 18; s++)
    {
        const uint32_t off = be32(dol + s * 4), addr = be32(dol + 0x48 + s * 4), size = be32(dol + 0x90 + s * 4);
        if (size) memcpy(GCNR_PTR(sMem, addr), dol + off, size);
    }
    gcnr_ctx c;
    memset(&c, 0, sizeof(c));
    c.r[1] = 0x817F0000u;
    c.r[2] = gcnr_r2;
    c.r[13] = gcnr_r13;
    gcnr_gqr[2] = 0x00040004u; /* OSInitFastCast */
    gcnr_gqr[3] = 0x00050005u;
    gcnr_gqr[4] = 0x00060006u;
    gcnr_gqr[5] = 0x00070007u;
    const uint32_t target = (uint32_t)strtoul(argv[2], NULL, 16);
    const int isFloat = argv[3][0] == 'f';
    for (int i = 4, k = 0; i < argc && k < 8; i++, k++)
    {
        if (isFloat) c.f[1 + k].ps0 = c.f[1 + k].ps1 = atof(argv[i]);
        else c.r[3 + k] = (uint32_t)strtoul(argv[i], NULL, 0);
    }
    gcnr_lookup(target)(sMem, &c);
    printf("r3 %08X  f1 %.9g  (ps1 %.9g)\n", c.r[3], c.f[1].ps0, c.f[1].ps1);
    return 0;
}
