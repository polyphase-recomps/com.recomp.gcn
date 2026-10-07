/**
 * @file generator.h
 * @brief Code generation behind an interface: CGenerator writes C for the AOT build; a Live
 *        generator (machine code at run time from the player's disc) can implement the same.
 */
#pragma once

#include "analysis.h"
#include "program.h"

#include <set>
#include <string>

namespace gcnr
{
struct GenOptions
{
    bool comments = true; // the disassembly above each instruction
    std::set<uint32_t> traced; // functions that log their arguments on entry (debugging)
    bool checkSp = false;      // every call checks the callee gave r1 back unchanged (debugging)
};

class Generator
{
public:
    virtual ~Generator() = default;
    // one guest function
    virtual void function(const Program& program, const Analysis& analysis, std::string& out) = 0;
};

class CGenerator : public Generator
{
public:
    explicit CGenerator(const GenOptions& options) : mOptions(options) {}
    void function(const Program& program, const Analysis& analysis, std::string& out) override;

private:
    GenOptions mOptions;
};
} // namespace gcnr
