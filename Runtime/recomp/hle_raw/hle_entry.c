/*
 * Entry points of the recomp build's HLE module (the runtime's SDK replacement, Runtime/guest,
 * compiled without the decomp's game code), compiled for wasm directly like guest_raw/entry.c.
 *
 *  - gcn_game_entry: loads the whole DOL from the disc (code and data at their addresses, bss
 *    zeroed: the recompiled game reads its data where the original did), puts the initial
 *    contents of the HLE globals that live at DOL addresses in place (gcn_hle_place.py),
 *    byte-swaps the pointers in the HLE's initialised data (gcn_befix), sets the boot globals
 *    and starts the game (gcn_boot, guest/sdk/os.c), whose main() is the recompiled one.
 *  - gcn_thread_entry / gcn_spin: as in guest_raw/entry.c.
 */
typedef unsigned int u32;

extern u32 *__start_gcn_befix[] __attribute__((weak));
extern u32 *__stop_gcn_befix[] __attribute__((weak));

u32 gcn_host_disc_read(void *dst, u32 offset, u32 size);
void gcn_host_fatal(const char *msg);
void gcn_boot(u32 arena_lo);                           /* guest/sdk/os.c */
void *gcn_thread_main(void *(*fn)(void *), void *arg); /* guest/sdk/thread.c */
void gcn_spin_wait(void);                              /* guest/sdk/thread.c */

/* Filled in by Runtime/tools/recomp/gcn_hle_place.py: magic, count, 0, 0, then (DOL address,
 * linked address, size) of every initialised HLE global placed at its DOL address. */
#define GCN_HLE_PLACE_MAX 4096
__attribute__((used)) u32 gcn_hle_place_table[4 + 3 * GCN_HLE_PLACE_MAX] = {0x47504C43};

static u32 get32be(const unsigned char *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void put32(u32 addr, u32 value)
{
    *(volatile u32 *)addr = __builtin_bswap32(value);
}

/* the DOL as the apploader leaves it; returns the end of its code, data and bss */
static u32 load_dol(void)
{
    unsigned char dir[4], hdr[0x100];
    u32 dol, i, end = 0x80003100;

    if (gcn_host_disc_read(dir, 0x420, 4) != 4 || gcn_host_disc_read(hdr, get32be(dir), 0x100) != 0x100)
    {
        gcn_host_fatal("recomp: cannot read the DOL from the disc");
    }
    dol = get32be(dir);
    {
        const u32 bss = get32be(hdr + 0xD8), bss_size = get32be(hdr + 0xDC);
        if (bss_size)
        {
            __builtin_memset((void *)bss, 0, bss_size);
            if (bss + bss_size > end) end = bss + bss_size;
        }
    }
    for (i = 0; i < 18; i++)
    {
        const u32 off = get32be(hdr + 4 * i), addr = get32be(hdr + 0x48 + 4 * i), size = get32be(hdr + 0x90 + 4 * i);
        if (!size) continue;
        if (gcn_host_disc_read((void *)addr, dol + off, size) != size)
        {
            gcn_host_fatal("recomp: short read of a DOL section");
        }
        if (addr + size > end) end = addr + size;
    }
    return end;
}

__attribute__((export_name("gcn_game_entry"))) void gcn_game_entry(void)
{
    u32 **p, i, n, arena_lo;

    arena_lo = load_dol();
    n = gcn_hle_place_table[0] == 0x47504C43 ? gcn_hle_place_table[1] : 0;
    for (i = 0; i < n; i++)
    {
        const u32 *e = &gcn_hle_place_table[4 + 3 * i];
        __builtin_memcpy((void *)e[0], (const void *)e[1], e[2]);
    }
    for (p = __start_gcn_befix; p < __stop_gcn_befix; p++)
    {
        **p = __builtin_bswap32(**p);
    }
    put32(0x80000020, 0x0D15EA5E);  /* boot magic */
    put32(0x80000024, 1);           /* version */
    put32(0x80000028, 0x01800000);  /* physical memory size */
    put32(0x8000002C, 0x00000003);  /* console type: retail */
    put32(0x800000CC, 0);           /* TV mode: NTSC */
    put32(0x800000F0, 0x01800000);  /* simulated memory size */
    put32(0x800000F8, 162000000);   /* bus clock */
    put32(0x800000FC, 486000000);   /* core clock */
    gcn_boot((arena_lo + 31) & ~31u);
}

__attribute__((export_name("gcn_thread_entry"))) void gcn_thread_entry(u32 fn, u32 arg)
{
    gcn_thread_main((void *(*)(void *))fn, (void *)arg);
}

__attribute__((export_name("gcn_spin"))) void gcn_spin(void)
{
    gcn_spin_wait();
}

/* recompiled loops (GCNR_LOOP_ANY): pending interrupts, when they are enabled */
void gcn_interrupt_point(void); /* guest/sdk/thread.c */
__attribute__((export_name("gcn_poll"))) void gcn_poll(void)
{
    gcn_interrupt_point();
}
