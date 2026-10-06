/*
 * Guest entry points, compiled for wasm directly (NOT through the big-endian pipeline):
 * this code sees memory the way wasm does, so it can finish what the pipeline leaves to
 * run time before any game code runs.
 *
 *  - gcn_game_entry: byte-swaps the pointers in initialised data (gcn_ir.py lists their
 *    addresses in section gcn_befix, as wasm-ld wrote them), sets up the low-memory OS
 *    globals the console's boot ROM leaves, and starts the game (gcn_boot, guest/os.c).
 *  - gcn_thread_entry: first function of every guest thread the host starts.
 */
typedef unsigned int u32;

extern u32 *__start_gcn_befix[] __attribute__((weak));
extern u32 *__stop_gcn_befix[] __attribute__((weak));
extern unsigned char __heap_base[];
extern unsigned char __data_end[];

u32 gcn_host_disc_read(void *dst, u32 offset, u32 size);
void gcn_boot(u32 arena_lo);                 /* guest/os.c */
void *gcn_thread_main(void *(*fn)(void *), void *arg); /* guest/thread.c */
void gcn_spin_wait(void);                              /* guest/thread.c */

/*
 * Filled in after linking by Runtime/tools/gcn_place.py: magic, entry count, end of the
 * original data/bss, reserved, then (original address, linked address, size) of every
 * initialised global that lives at its original DOL address. The non-zero magic keeps the
 * table in initialised data, where the tool can write it.
 */
#define GCN_PLACE_MAX 16384
__attribute__((used)) u32 gcn_place_table[4 + 3 * GCN_PLACE_MAX] = {0x47504C43};

static u32 get32be(const unsigned char *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* The DOL's data sections at their addresses (what the console's apploader does), then the
 * initial contents of the placed globals over them. Data nobody placed keeps its original
 * bytes, so code that reads past a global into its neighbours finds what it expects. */
static void load_original_data(void)
{
    unsigned char dir[4], hdr[0x100];
    u32 dol, i, n;

    if (gcn_place_table[0] != 0x47504C43 || !gcn_place_table[2]) return;  /* gcn_place.py did not run */
    if (gcn_host_disc_read(dir, 0x420, 4) == 4 && gcn_host_disc_read(hdr, get32be(dir), 0x100) == 0x100)
    {
        dol = get32be(dir);
        for (i = 7; i < 18; i++)
        {
            u32 off = get32be(hdr + 4 * i), addr = get32be(hdr + 0x48 + 4 * i), size = get32be(hdr + 0x90 + 4 * i);
            /* sections below our own data (extab etc.) would overwrite it; the game never reads them */
            if (size && addr >= (u32)__heap_base) gcn_host_disc_read((void *)addr, dol + off, size);
        }
    }
    n = gcn_place_table[1];
    for (i = 0; i < n; i++)
    {
        const u32 *e = &gcn_place_table[4 + 3 * i];
        __builtin_memcpy((void *)e[0], (const void *)e[1], e[2]);
    }
}

static void put32(u32 addr, u32 value)
{
    *(volatile u32 *)addr = __builtin_bswap32(value);
}

__attribute__((export_name("gcn_game_entry"))) void gcn_game_entry(void)
{
    u32 **p;
    u32 arena_lo;

    load_original_data();
    for (p = __start_gcn_befix; p < __stop_gcn_befix; p++)
    {
        **p = __builtin_bswap32(**p);
    }
    /* boot ROM / IPL globals (OS_BASE_CACHED + offset), big-endian like everything the game reads */
    put32(0x80000020, 0x0D15EA5E);  /* boot magic */
    put32(0x80000024, 1);           /* version */
    put32(0x80000028, 0x01800000);  /* physical memory size */
    put32(0x8000002C, 0x00000003);  /* console type: retail */
    put32(0x800000CC, 0);           /* TV mode: NTSC */
    put32(0x800000F0, 0x01800000);  /* simulated memory size */
    put32(0x800000F8, 162000000);   /* bus clock */
    put32(0x800000FC, 486000000);   /* core clock */
    arena_lo = (u32)__heap_base;
    if (gcn_place_table[2] > arena_lo) arena_lo = gcn_place_table[2];
    gcn_boot(arena_lo);
}

__attribute__((export_name("gcn_thread_entry"))) void gcn_thread_entry(u32 fn, u32 arg)
{
    gcn_thread_main((void *(*)(void *))fn, (void *)arg);
}

/* The host calls this when the game spins on memory an interrupt would change. */
__attribute__((export_name("gcn_spin"))) void gcn_spin(void)
{
    gcn_spin_wait();
}

/* Lets the host find the data segment's end (diagnostics). */
__attribute__((export_name("gcn_data_end"))) u32 gcn_data_end(void)
{
    return (u32)__data_end;
}
