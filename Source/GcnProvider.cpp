/**
 * @file GcnProvider.cpp
 * @brief The GameCube runtime's RecompProvider (see GcnProvider.h).
 */

#include "GcnProvider.h"

#include "GcnGuestHost.h"
#include "Gcn/gcnw_backend.h"
#include "Gcn/gcnw_module.h"

#include <cmath>
#include <cstring>

namespace
{
RecompType ToRecompType(int type)
{
    switch (type)
    {
    case GCNW_VAR_U8: return RecompType::U8;
    case GCNW_VAR_S8: return RecompType::S8;
    case GCNW_VAR_U16: return RecompType::U16;
    case GCNW_VAR_S16: return RecompType::S16;
    case GCNW_VAR_U32: return RecompType::U32;
    case GCNW_VAR_F32: return RecompType::F32;
    case GCNW_VAR_STR: return RecompType::Str;
    default: return RecompType::S32;
    }
}

int ToGcnType(RecompType type)
{
    switch (type)
    {
    case RecompType::U8: return GCNW_VAR_U8;
    case RecompType::S8: return GCNW_VAR_S8;
    case RecompType::U16: return GCNW_VAR_U16;
    case RecompType::S16: return GCNW_VAR_S16;
    case RecompType::U32: return GCNW_VAR_U32;
    case RecompType::F32: return GCNW_VAR_F32;
    case RecompType::Str: return GCNW_VAR_STR;
    default: return GCNW_VAR_S32;
    }
}

int TypeBytes(int type)
{
    switch (type)
    {
    case GCNW_VAR_U8:
    case GCNW_VAR_S8:
    case GCNW_VAR_STR: return 1;
    case GCNW_VAR_U16:
    case GCNW_VAR_S16: return 2;
    default: return 4;
    }
}

bool ReadTyped(uint32_t addr, int type, int stride, RecompValue& out)
{
    if (type == GCNW_VAR_STR)
    {
        out = RecompValue::Text(GcnGuestHost::ReadString(addr, stride > 1 ? (uint32_t)stride : 256u));
        return true;
    }
    const uint32_t raw = GcnGuestHost::Read(addr, TypeBytes(type)); // big-endian, assembled by the host
    double v;
    switch (type)
    {
    case GCNW_VAR_S8: v = (int8_t)raw; break;
    case GCNW_VAR_S16: v = (int16_t)raw; break;
    case GCNW_VAR_S32: v = (int32_t)raw; break;
    case GCNW_VAR_F32:
    {
        float f;
        memcpy(&f, &raw, 4);
        v = f;
        break;
    }
    default: v = raw; break;
    }
    out = RecompValue::Number(v);
    return true;
}

bool WriteTyped(uint32_t addr, int type, int stride, const RecompValue& value)
{
    if (type == GCNW_VAR_STR)
    {
        if (!value.isText) return false;
        const size_t cap = stride > 1 ? (size_t)stride : value.text.size() + 1;
        for (size_t i = 0; i < cap; ++i)
        {
            GcnGuestHost::Write(addr + (uint32_t)i, i < value.text.size() ? (uint8_t)value.text[i] : 0, 1);
        }
        return true;
    }
    if (value.isText) return false;
    uint32_t raw;
    if (type == GCNW_VAR_F32)
    {
        const float f = (float)value.number;
        memcpy(&raw, &f, 4);
    }
    else
    {
        raw = (uint32_t)(int64_t)std::llround(value.number);
    }
    GcnGuestHost::Write(addr, raw, TypeBytes(type));
    return true;
}
}

GcnProvider& GcnProvider::Get()
{
    static GcnProvider sProvider;
    return sProvider;
}

void GcnProvider::SetFrame(int width, int height)
{
    mWidth = width;
    mHeight = height;
}

std::string GcnProvider::GamePackage() const
{
    const GcnwModule* module = GcnGuestHost::GetModule();
    return (module != nullptr && module->package != nullptr) ? module->package : std::string();
}

bool GcnProvider::IsLive() const
{
    return GcnGuestHost::GetState() == GcnGuestHost::State::Running;
}

void GcnProvider::Variables(std::vector<RecompVarInfo>& out) const
{
    for (const GcnGuestHost::BridgeVar& v : GcnGuestHost::BridgeVariables())
    {
        RecompVarInfo info;
        info.name = v.name;
        info.help = v.help;
        info.type = ToRecompType(v.type);
        info.count = v.count;
        out.push_back(info);
    }
}

void GcnProvider::Requests(std::vector<RecompRequestInfo>& out) const
{
    for (const GcnGuestHost::BridgeRequestInfo& r : GcnGuestHost::BridgeRequests())
    {
        out.push_back({r.name, r.help});
    }
}

// A published variable or any decomp global by name (as Lua Gcn.Read).
bool GcnProvider::Get(const std::string& name, int index, RecompValue& out)
{
    uint32_t addr = 0, size = 0;
    int type = 0, count = 0, stride = 0;
    if (!IsLive() || !GcnGuestHost::Resolve(name, addr, size, type, count, stride))
    {
        return false;
    }
    if (count > 0 && index >= count)
    {
        return false;
    }
    return ReadTyped(addr + (uint32_t)(index * (stride > 0 ? stride : TypeBytes(type))), type, stride, out);
}

bool GcnProvider::Set(const std::string& name, int index, const RecompValue& value)
{
    uint32_t addr = 0, size = 0;
    int type = 0, count = 0, stride = 0;
    if (!IsLive() || !GcnGuestHost::Resolve(name, addr, size, type, count, stride))
    {
        return false;
    }
    return WriteTyped(addr + (uint32_t)(index * (stride > 0 ? stride : TypeBytes(type))), type, stride, value);
}

int GcnProvider::Request(const std::string& name, const std::vector<int>& args)
{
    return GcnGuestHost::BridgeRequest(name, args);
}

bool GcnProvider::Result(int id, int& result)
{
    return GcnGuestHost::BridgeResult(id, result);
}

bool GcnProvider::ReadAddress(uint64_t address, RecompType type, RecompValue& out)
{
    return IsLive() && ReadTyped((uint32_t)address, ToGcnType(type), 0, out);
}

bool GcnProvider::WriteAddress(uint64_t address, RecompType type, const RecompValue& value)
{
    return IsLive() && WriteTyped((uint32_t)address, ToGcnType(type), 0, value);
}

bool GcnProvider::ResolveSymbol(const std::string& name, uint64_t& address)
{
    uint32_t addr = 0, size = 0;
    int type = 0, count = 0, stride = 0;
    if (!GcnGuestHost::Resolve(name, addr, size, type, count, stride))
    {
        return false;
    }
    address = addr;
    return true;
}

RecompFrameInfo GcnProvider::FrameInfo() const
{
    RecompFrameInfo info;
    info.width = mWidth;
    info.height = mHeight;
    info.displayAspect = 4.0f / 3.0f;
    return info;
}
