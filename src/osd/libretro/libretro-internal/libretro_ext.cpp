/* -------------------------------------------------------------------------
Optional (non-standard) extension API for enhanced frontends.
Author: Ryan Souders (ryan@deadsignallabs.com)

This is NOT part of the libretro API and is safe for other frontends:
- If a frontend doesn't know about it, it will never call it.
- The core remains a valid libretro core.

Intended use: lightweight introspection needed for features like
PC-triggered fast-skip, scripting, overlays, etc.

ABI rules:
- C ABI exports.
- Versioned, fixed-size struct.
- Functions must be safe to call when no machine is running.
-------------------------------------------------------------------------- */

#ifndef LIBRETRO_EXT_HPP
#define LIBRETRO_EXT_HPP

#ifndef LIBRETRO_EXT
#define LIBRETRO_EXT
#endif // LIBRETRO_EXT

#include "emu.h"
// #include "screen.h"
#include "libretro_ext.h"
#include "../frontend/mame/mame.h"

#include <string>
#include <vector>
#include <cstring>

#if defined(_WIN32)
  #define LIBRETRO_EXT_EXPORT extern "C" __declspec(dllexport)
#else
  #define LIBRETRO_EXT_EXPORT extern "C" __attribute__((visibility("default")))
#endif

extern retro_log_printf_t log_cb;

uint64_t g_extFrameCounter = 0;
libretro_ext_watch_hit g_extLastWatchHit{};
std::vector<libretro_ext_watch_rule> g_extWatchRules;
std::unordered_map<std::string, libretro_ext_pc_ring> g_extPcHistory;

static bool g_regionTagsValid = false;
static bool g_execHooksInstalled = false;
static std::vector<std::string> g_regionTags;
static libretro_ext_exec_hit g_lastExecHit = {};
static std::vector<libretro_ext_exec_trigger> g_execTriggers;

// Lazy way of seeing all devices attached to the running machine
static void log_all_devices(running_machine& mach)
{
    device_enumerator iter(mach.root_device());
    for (device_t& dev : iter)
        log_cb(RETRO_LOG_INFO, "DEV tag=%s  name=%s\n", dev.tag(), dev.name());
}

void libretro_ext_record_pc(const char* cpuTag, uint64_t pc)
{
    if (!cpuTag)
        return;

    g_extPcHistory[cpuTag].push(pc);
}

void libretro_ext_record_watch_hit(const char* cpuTag,
                                   uint64_t pc,
                                   uint64_t address,
                                   uint32_t value,
                                   uint8_t access,
                                   uint8_t width,
                                   uint64_t totalCycles)
{
    if (!cpuTag)
        return;

    for (const auto& rule : g_extWatchRules)
    {
        if (!rule.enabled)
            continue;
        if (rule.cpuTag != cpuTag)
            continue;
        if (!(rule.access & access))
            continue;
        if (rule.width && rule.width != width)
            continue;
        if (address < rule.start || address > rule.end)
            continue;

        g_extLastWatchHit = {};
        g_extLastWatchHit.hit = true;
        std::strncpy(g_extLastWatchHit.cpuTag, cpuTag, sizeof(g_extLastWatchHit.cpuTag) - 1);
        g_extLastWatchHit.pc = pc;
        g_extLastWatchHit.address = address;
        g_extLastWatchHit.value = value;
        g_extLastWatchHit.access = access;
        g_extLastWatchHit.width = width;
        g_extLastWatchHit.frame = g_extFrameCounter;
        g_extLastWatchHit.totalCycles = totalCycles;

        auto it = g_extPcHistory.find(cpuTag);
        if (it != g_extPcHistory.end())
        {
            const libretro_ext_pc_ring& ring = it->second;
            uint32_t count = ring.filled ? 256 : ring.head;
            g_extLastWatchHit.historyCount = count;

            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t idx = ring.filled ? ((ring.head + i) % 256) : i;
                g_extLastWatchHit.pcHistory[i] = ring.pcs[idx];
            }
        }
        break;
    }
}

static void invalidate_region_cache()
{
    g_regionTagsValid = false;
    g_regionTags.clear();
}

static void build_region_cache(running_machine& mach)
{
    if (g_regionTagsValid)
        return;

    g_regionTags.clear();

    // MAME typically exposes regions via memory().regions()
    // This is the most direct “region tag” list for ROM/patching/extraction.
    for (const auto& [tag, region] : mach.memory().regions()) {
        if (region && !tag.empty())
            g_regionTags.push_back(tag);
    }

    g_regionTagsValid = true;
}

static running_machine* libretro_ext_machine()
{
    auto* mm = mame_machine_manager::instance();
    return mm ? mm->machine() : nullptr;
}

#include "screen.h"

static screen_device* libretro_ext_first_screen(running_machine& mach)
{
    auto iter = device_enumerator(mach.root_device());
    auto it = std::find_if(iter.begin(), iter.end(), [](device_t& dev) {
        screen_device* scr = dynamic_cast<screen_device*>(&dev);
        return scr != nullptr;
    });
    if (it != iter.end())
        return dynamic_cast<screen_device*>(&(*it));
    return nullptr;
}

static uint64_t libretro_ext_get_frame_number_impl()
{
    /* running_machine* mach = libretro_ext_machine();
    if (!mach) return 0;

    screen_device* scr = libretro_ext_first_screen(*mach);
    if (!scr) return 0;

    return (uint64_t)scr->frame_number(); */
    return g_extFrameCounter;
}

static uint64_t libretro_ext_get_cpu_total_cycles_by_tag_impl(const char* cpu_tag)
{
    running_machine* mach = libretro_ext_machine();
    if (!mach || !cpu_tag) return 0;

    device_t* dev = mach->root_device().subdevice(cpu_tag);
    if (!dev) return 0;

    device_execute_interface* exec = nullptr;
    if (!dev->interface(exec)) return 0;

    return (uint64_t)exec->total_cycles();
}

static uint64_t libretro_ext_get_time_attoseconds_impl()
{
    running_machine* mach = libretro_ext_machine();
    if (!mach) return 0;

    // attotime is seconds + attoseconds; we only return attoseconds part + seconds*1e18-ish
    // Safer: pack into attoseconds total. 1 second = 1e18 attoseconds.
    const attotime t = mach->time();
    return (uint64_t)t.seconds() * 1000000000000000000ULL + (uint64_t)t.attoseconds();
}

static device_t* libretro_ext_find_device(running_machine& mach, const char* tag)
{
    if (!tag || !tag[0]) return nullptr;
    return mach.root_device().subdevice(tag);
}

static uint64_t libretro_ext_get_cpu_total_cycles_impl(const char* cpu_tag)
{
    running_machine* mach = libretro_ext_machine();
    if (!mach) return 0;

    device_t* dev = libretro_ext_find_device(*mach, cpu_tag);
    if (!dev) return 0;

    device_execute_interface* exec = nullptr;
    if (!dev->interface(exec)) return 0;

    return (uint64_t)exec->total_cycles();
}

static int libretro_ext_get_region_count_impl()
{
    running_machine* mach = libretro_ext_machine(); // whatever you already use to get the running_machine*
    if (!mach)
        return 0;

    build_region_cache(*mach);
    return (int)g_regionTags.size();
}

static const char* libretro_ext_get_region_tag_impl(int index)
{
    running_machine* mach = libretro_ext_machine();
    if (!mach)
        return nullptr;

    build_region_cache(*mach);

    if (index < 0 || index >= (int)g_regionTags.size())
        return nullptr;

    // Safe: points to stable storage in g_regionTags until cache invalidated
    return g_regionTags[index].c_str();
}

static int space_name_to_id(const char* name)
{
    if (!name) return -1;
    if (!strcmp(name, "program")) return AS_PROGRAM;
    if (!strcmp(name, "data"))    return AS_DATA;
    if (!strcmp(name, "io"))      return AS_IO;
    if (!strcmp(name, "opcodes")) return AS_OPCODES;
    return -1;
}

static address_space* get_space_by_tag(running_machine& mach,
                                       const char* cpu_tag,
                                       const char* space_name)
{
    device_t* dev = mach.root_device().subdevice(cpu_tag);
    if (!dev)
        return nullptr;

    device_memory_interface* mem = nullptr;
    if (!dev->interface(mem))
        return nullptr;

    int id = space_name_to_id(space_name);
    if (id < 0)
        return nullptr;

    if (!mem->has_space(id))
        return nullptr;

    return &mem->space(id);
}

static device_t* find_device_by_tag(running_machine& mach, const char* tag)
{
    if (!tag || !tag[0]) return nullptr;
    // tag examples: ":maincpu" ":audiocpu"
    return mach.root_device().subdevice(tag);
}

static uint8_t libretro_ext_read_u8_impl(const char* cpu_tag, const char* space_name, uint64_t addr)
{
    running_machine* mach = libretro_ext_machine();
    if (!mach) return 0;

    address_space* sp = get_space_by_tag(*mach, cpu_tag, space_name);
    if (!sp) return 0;

    return (uint8_t)sp->read_byte((offs_t)addr);
}

static uint16_t libretro_ext_read_u16_impl(const char* cpu_tag, const char* space_name, uint64_t addr)
{
    running_machine* mach = libretro_ext_machine();
    if (!mach) return 0;

    address_space* sp = get_space_by_tag(*mach, cpu_tag, space_name);
    if (!sp) return 0;

    return (uint16_t)sp->read_word((offs_t)addr);
}

static uint32_t libretro_ext_read_u32_impl(const char* cpu_tag, const char* space_name, uint64_t addr)
{
    running_machine* mach = libretro_ext_machine();
    if (!mach) return 0;

    address_space* sp = get_space_by_tag(*mach, cpu_tag, space_name);
    if (!sp) return 0;

    return (uint32_t)sp->read_dword((offs_t)addr);
}

static void libretro_ext_write_u8_impl(const char* cpu_tag, const char* space_name, uint64_t addr, uint8_t v)
{
    running_machine* mach = libretro_ext_machine();
    if (!mach) return;

    address_space* sp = get_space_by_tag(*mach, cpu_tag, space_name);
    if (!sp) return;

    sp->write_byte((offs_t)addr, v);
}

static void libretro_ext_write_u16_impl(const char* cpu_tag, const char* space_name, uint64_t addr, uint16_t v)
{
    running_machine* mach = libretro_ext_machine();
    if (!mach) return;

    address_space* sp = get_space_by_tag(*mach, cpu_tag, space_name);
    if (!sp) return;

    sp->write_word((offs_t)addr, v);
}

static void libretro_ext_write_u32_impl(const char* cpu_tag, const char* space_name, uint64_t addr, uint32_t v)
{
    running_machine* mach = libretro_ext_machine();
    if (!mach) return;

    address_space* sp = get_space_by_tag(*mach, cpu_tag, space_name);
    if (!sp) return;

    sp->write_dword((offs_t)addr, v);
}

static memory_region* find_region(running_machine& mach, const char* tag)
{
    if (!tag) return nullptr;
    return mach.root_device().memregion(tag);
}

static uint64_t libretro_ext_get_region_size_impl(const char* tag)
{
    running_machine* mach = libretro_ext_machine();
    if (!mach) return 0;

    memory_region* r = find_region(*mach, tag);
    if (!r) return 0;

    return r->bytes();
}

static uint64_t libretro_ext_read_region_impl(const char* tag, uint64_t offset, void* dst, uint64_t bytes)
{
    running_machine* mach = libretro_ext_machine();
    if (!mach) return 0;

    memory_region* r = find_region(*mach, tag);
    if (!r) return 0;

    uint64_t size = r->bytes();
    if (offset >= size) return 0;

    if (offset + bytes > size)
        bytes = size - offset;

    memcpy(dst, r->base() + offset, bytes);
    return bytes;
}

static uint64_t libretro_ext_write_region_impl(const char* tag, uint64_t offset, const void* src, uint64_t bytes)
{
    running_machine* mach = libretro_ext_machine();
    if (!mach) return 0;

    memory_region* r = find_region(*mach, tag);
    if (!r) return 0;

    uint64_t size = r->bytes();
    if (offset >= size) return 0;

    if (offset + bytes > size)
        bytes = size - offset;

    memcpy(r->base() + offset, src, bytes);
    return bytes;
}

static uint64_t get_state_u64(device_state_interface& st, int state_id)
{
    // state_int() is commonly available and returns a 64-bit-ish signed int.
    // If your branch uses a different type, adjust here once in one place.
    return (uint64_t)st.state_int(state_id);
}

static bool libretro_ext_get_cpu_by_index(running_machine& mach, int cpu_index, device_t*& out_dev)
{
    int i = 0;
    device_enumerator iter(mach.root_device());
    for (device_t& dev : iter)
    {
        device_execute_interface* exec = nullptr;
        if (!dev.interface(exec))
            continue;

        if (i == cpu_index)
        {
            out_dev = &dev;
            return true;
        }
        i++;
    }
    return false;
}

static uint64_t libretro_ext_get_cpu_genpc_impl(int cpu_index)
{
    running_machine* mach = libretro_ext_machine();
    if (!mach) return 0;

    device_t* dev = nullptr;
    if (!libretro_ext_get_cpu_by_index(*mach, cpu_index, dev)) return 0;

    device_state_interface* st = nullptr;
    if (!dev->interface(st)) return 0;

    return get_state_u64(*st, STATE_GENPC);
}

static uint64_t libretro_ext_get_cpu_genpcbase_impl(int cpu_index)
{
    running_machine* mach = libretro_ext_machine();
    if (!mach) return 0;

    device_t* dev = nullptr;
    if (!libretro_ext_get_cpu_by_index(*mach, cpu_index, dev)) return 0;

    device_state_interface* st = nullptr;
    if (!dev->interface(st)) return 0;

    return get_state_u64(*st, STATE_GENPCBASE);
}

static int libretro_ext_cpu_count_impl()
{
    running_machine* mach = libretro_ext_machine();
    if (!mach)
        return 0;

    int count = 0;
    device_enumerator iter(mach->root_device());
    for (device_t& dev : iter)
    {
        device_execute_interface* exec = nullptr;
        if (dev.interface(exec))
            count++;
    }
    return count;
}

static const char* libretro_ext_get_driver_name_impl()
{
    running_machine* mach = libretro_ext_machine();
    if (!mach)
        return nullptr;
    return mach->system().name;
}

static const char* libretro_ext_get_cpu_tag_impl(int cpu_index)
{
    running_machine* mach = libretro_ext_machine();
    if (!mach)
        return nullptr;

    device_t* dev = nullptr;
    if (!libretro_ext_get_cpu_by_index(*mach, cpu_index, dev))
        return nullptr;
    return dev->tag();
}

static uint64_t libretro_ext_get_cpu_pc_impl(int cpu_index)
{
    running_machine* mach = libretro_ext_machine();
    if (!mach)
        return 0;

    device_t* dev = nullptr;
    if (!libretro_ext_get_cpu_by_index(*mach, cpu_index, dev))
        return 0;

    device_state_interface* st = nullptr;
    if (!dev->interface(st))
        return 0;

    // Debugger-style PC (preferred)
    const auto genpc = st->state_int(STATE_GENPC);
    if (genpc != 0)
        return (uint64_t)genpc;

    // Fallback
    return (uint64_t)st->pc();
}

static inline void check_exec_triggers(const char* cpuTag, uint64_t pc)
{
    for (auto& t : g_execTriggers)
    {
        if (!t.enabled)
            continue;

        if (t.cpuTag == cpuTag && pc >= t.pcStart && pc <= t.pcEnd)
        {
            g_lastExecHit.hit = true;
            g_lastExecHit.pc = pc;
            std::strncpy(g_lastExecHit.cpuTag, cpuTag, sizeof(g_lastExecHit.cpuTag) - 1);
            g_lastExecHit.cpuTag[sizeof(g_lastExecHit.cpuTag) - 1] = '\0';
            g_lastExecHit.frame = g_extFrameCounter;

            if (t.disableAfterHit || t.oneShot)
                t.enabled = false;
        }
    }
}

static void ext_clear_exec_triggers_impl()
{
    g_execTriggers.clear();
    std::memset(&g_lastExecHit, 0, sizeof(g_lastExecHit));
}

static void libretro_ext_add_exec_trigger_impl(const char* cpuTag, uint64_t pcStart, uint64_t pcEnd,
                                      bool oneShot, bool disableAfterHit)
{
    if (!cpuTag || !cpuTag[0])
        return;

    libretro_ext_exec_trigger t;
    t.cpuTag = cpuTag;
    t.pcStart = pcStart;
    t.pcEnd = pcEnd;
    t.enabled = true;
    t.oneShot = oneShot;
    t.disableAfterHit = disableAfterHit;
    g_execTriggers.push_back(t);
}

static bool libretro_ext_get_last_exec_hit_impl(libretro_ext_exec_hit* outHit)
{
    if (!outHit)
        return false;

    *outHit = g_lastExecHit;
    return g_lastExecHit.hit;
}

static void libretro_ext_clear_last_exec_hit_impl()
{
    std::memset(&g_lastExecHit, 0, sizeof(g_lastExecHit));
}

static void libretro_ext_check_exec_triggers(device_t& dev, uint64_t pc)
{
    const char* tag = dev.tag();
    if (!tag)
        return;

    for (auto& trig : g_execTriggers)
    {
        if (!trig.enabled)
            continue;

        if (trig.cpuTag != tag)
            continue;

        if (pc < trig.pcStart || pc > trig.pcEnd)
            continue;

        g_lastExecHit.hit = true;
        std::strncpy(g_lastExecHit.cpuTag, tag, sizeof(g_lastExecHit.cpuTag) - 1);
        g_lastExecHit.pc = pc;
        g_lastExecHit.frame = g_extFrameCounter;

        device_execute_interface* exec = nullptr;
        if (dev.interface(exec))
            g_lastExecHit.totalCycles = (uint64_t)exec->total_cycles();
        else
            g_lastExecHit.totalCycles = 0;

        if (trig.disableAfterHit || trig.oneShot)
            trig.enabled = false;
    }
}

static void libretro_ext_install_exec_hooks_if_needed()
{
    if (g_execHooksInstalled)
        return;

    running_machine* mach = libretro_ext_machine();
    if (!mach)
        return;

    for (device_t& dev : device_enumerator(mach->root_device()))
    {
        device_execute_interface* exec = nullptr;
        if (!dev.interface(exec))
            continue;
    }

    g_execHooksInstalled = true;
}

static const libretro_ext_api_v1 g_ext_api_v1 = {
    1,
    &libretro_ext_get_driver_name_impl,
    &libretro_ext_cpu_count_impl,
    &libretro_ext_get_cpu_tag_impl,
    &libretro_ext_get_cpu_pc_impl,
};

static const libretro_ext_api_v2 g_ext_api_v2 = {
    2,
    sizeof(libretro_ext_api_v2),

    libretro_ext_get_driver_name_impl,
    libretro_ext_cpu_count_impl,
    libretro_ext_get_cpu_tag_impl,
    libretro_ext_get_cpu_pc_impl,

    libretro_ext_read_u8_impl,
    libretro_ext_read_u16_impl,
    libretro_ext_read_u32_impl,

    libretro_ext_write_u8_impl,
    libretro_ext_write_u16_impl,
    libretro_ext_write_u32_impl,

    libretro_ext_get_region_count_impl,
    libretro_ext_get_region_tag_impl,
    libretro_ext_get_region_size_impl,

    libretro_ext_read_region_impl,
    libretro_ext_write_region_impl,

    libretro_ext_get_frame_number_impl,
    libretro_ext_get_time_attoseconds_impl,

    libretro_ext_get_cpu_total_cycles_impl,
    libretro_ext_get_cpu_total_cycles_by_tag_impl
};

extern "C" {
    LIBRETRO_EXT_EXPORT const libretro_ext_api_v1* libretro_ext_get_api_v1()
    {
        return &g_ext_api_v1;
    }

    LIBRETRO_EXT_EXPORT const libretro_ext_api_v2* libretro_ext_get_api_v2()
    {
        return &g_ext_api_v2;
    }
}

#endif // LIBRETRO_EXT_HPP
