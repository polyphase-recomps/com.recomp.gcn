/**
 * @file decode_test.cpp
 * @brief Disassembles every text section of a DOL with the recompiler's decoder, one line per
 *        instruction ("80003100:\tmfspr   r0,8"), for comparison with
 *        `powerpc-eabi-objdump -M gekko,raw` (compare_objdump.py).
 *
 *   decode_test main.dol > ours.txt
 */
#include "../gekko.h"

#include <cstdio>
#include <vector>

static uint32_t be32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: decode_test main.dol\n");
        return 2;
    }
    FILE* f = std::fopen(argv[1], "rb");
    if (f == nullptr)
    {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    std::vector<uint8_t> dol;
    uint8_t buf[65536];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0)
    {
        dol.insert(dol.end(), buf, buf + got);
    }
    std::fclose(f);
    if (dol.size() < 0x100)
    {
        return 1;
    }
    for (int s = 0; s < 7; s++)
    {
        const uint32_t off = be32(&dol[s * 4]);
        const uint32_t addr = be32(&dol[0x48 + s * 4]);
        const uint32_t size = be32(&dol[0x90 + s * 4]);
        if (size == 0 || off + size > dol.size())
        {
            continue;
        }
        for (uint32_t i = 0; i + 4 <= size; i += 4)
        {
            const gekko::Insn insn = gekko::decode(be32(&dol[off + i]), addr + i);
            std::printf("%08x:\t%s\n", addr + i, gekko::disasm(insn).c_str());
        }
    }
    return 0;
}
