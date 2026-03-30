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
#include "libretro_ext.h"
#include "../frontend/mame/mame.h"
#include "../../../mame/capcom/cps1.h"

#include <string>
#include <vector>
#include <deque>
#include <cstring>
#include <cstdio>

#if defined(_WIN32)
  #define LIBRETRO_EXT_EXPORT extern "C" __declspec(dllexport)
#else
  #define LIBRETRO_EXT_EXPORT extern "C" __attribute__((visibility("default")))
#endif

bool g_extDebugExtensionsEnabled = false;

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
static std::vector<retro_memory_descriptor> g_memoryMapDescs;
static std::deque<std::string> g_memoryMapAddrspaces;

// DIP switch field cache — rebuilt whenever the machine pointer changes.
static running_machine* g_dipLastMachine = nullptr;
static std::vector<ioport_field*> g_dipFields;

static void libretro_ext_clear_native_watchpoints_impl()
{
    running_machine* mach = libretro_ext_machine();
    if (!mach)
        return;

    for (device_t& dev : device_enumerator(mach->root_device()))
    {
        if (dev.debug())
            dev.debug()->watchpoint_clear_all();
    }
}

static void libretro_ext_set_debug_extensions_enabled_impl(bool enabled)
{
    if (g_extDebugExtensionsEnabled == enabled)
        return;

    g_extDebugExtensionsEnabled = enabled;

    if (enabled)
    {
        running_machine* mach = libretro_ext_machine();
        if (mach)
        {
            bool has_debug_objects = false;
            for (device_t& dev : device_enumerator(mach->root_device()))
            {
                if (dev.debug())
                {
                    has_debug_objects = true;
                    break;
                }
            }

            if (!has_debug_objects)
            {
                log_cb(RETRO_LOG_WARN,
                       "libretro_ext: debug extensions enabled after machine start; device debug objects are unavailable. Enable before retro_load_game/reload content for watchpoints.\n");
            }
        }

        return;
    }

    g_extPcHistory.clear();
    g_extWatchRules.clear();
    std::memset(&g_extLastWatchHit, 0, sizeof(g_extLastWatchHit));
    std::memset(&g_lastExecHit, 0, sizeof(g_lastExecHit));
    g_execTriggers.clear();
    libretro_ext_clear_native_watchpoints_impl();
}

static bool libretro_ext_get_debug_extensions_enabled_impl()
{
    return g_extDebugExtensionsEnabled;
}

static bool libretro_ext_trigger_timing_capture_impl()
{
    running_machine* mach = libretro_ext_machine();
    if (!mach)
        return false;

    cps_state* cps = dynamic_cast<cps_state*>(&mach->root_device());
    if (!cps)
        return false;

    return cps->trigger_timing_capture();
}

// Lazy way of seeing all devices attached to the running machine
static void log_all_devices(running_machine& mach)
{
    device_enumerator iter(mach.root_device());
    for (device_t& dev : iter)
        log_cb(RETRO_LOG_INFO, "DEV tag=%s  name=%s\n", dev.tag(), dev.name());
}

void libretro_ext_record_pc(const char* cpuTag, uint64_t pc)
{
    if (!g_extDebugExtensionsEnabled || !cpuTag)
        return;

    g_extPcHistory[cpuTag].push(pc);
}

static void libretro_ext_check_watch_hit(const char* cpuTag,
                                uint64_t pc,
                                uint64_t address,
                                uint32_t value,
                                uint8_t access,
                                uint8_t width,
                                uint64_t totalCycles)
{
    if (!g_extDebugExtensionsEnabled || !cpuTag)
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
        g_extLastWatchHit.address = address;
        g_extLastWatchHit.pc = pc;
        g_extLastWatchHit.frame = g_extFrameCounter;
        g_extLastWatchHit.totalCycles = totalCycles;
        g_extLastWatchHit.value = value;
        g_extLastWatchHit.access = access;
        g_extLastWatchHit.width = width;

        const auto it = g_extPcHistory.find(cpuTag);
        if (it != g_extPcHistory.end())
        {
            const libretro_ext_pc_ring& ring = it->second;
            const uint32_t count = ring.filled ? 256 : ring.head;
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

void libretro_ext_record_watch_hit(const char* cpuTag,
                                   uint64_t pc,
                                   uint64_t address,
                                   uint32_t value,
                                   uint8_t access,
                                   uint8_t width,
                                   uint64_t totalCycles)
{
    /* log_cb(RETRO_LOG_INFO, "Watch hit: CPU=%s PC=%llX Address=%llX Value=%llX Access=%08X Width=%02X TotalCycles=%llX\n",
           cpuTag, pc, address, value, access, width, totalCycles); */
    libretro_ext_check_watch_hit(cpuTag, pc, address, value, access, width, totalCycles);
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

static void libretro_ext_check_watch_hit_impl(const char* cpuTag,
                                uint64_t pc,
                                uint64_t address,
                                uint32_t value,
                                uint8_t access,
                                uint8_t width,
                                uint64_t totalCycles)
{
    libretro_ext_check_watch_hit(cpuTag, pc, address, value, access, width, totalCycles);
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

/* static uint64_t libretro_ext_get_cpu_total_cycles_impl(const char* cpu_tag)
{
    running_machine* mach = libretro_ext_machine();
    if (!mach) return 0;

    device_t* dev = libretro_ext_find_device(*mach, cpu_tag);
    if (!dev) return 0;

    device_execute_interface* exec = nullptr;
    if (!dev->interface(exec)) return 0;

    return (uint64_t)exec->total_cycles();
} */

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
    if (!g_extDebugExtensionsEnabled)
        return;

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
    if (!g_extDebugExtensionsEnabled)
        return;

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
    if (!g_extDebugExtensionsEnabled)
        return;

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

void libretro_ext_set_memory_maps(retro_environment_t environ_cb)
{
    if (!environ_cb)
        return;

    running_machine* mach = libretro_ext_machine();
    if (!mach)
        return;

    g_memoryMapDescs.clear();
    g_memoryMapAddrspaces.clear();

    int device_index = 0;
    memory_interface_enumerator iter(mach->root_device());
    for (device_memory_interface& memory : iter)
    {
        if (!memory.has_space(AS_PROGRAM))
        {
            device_index++;
            continue;
        }

        address_space& space = memory.space(AS_PROGRAM);
        const bool big_endian = (space.endianness() == ENDIANNESS_BIG);

        // Build address-space name: empty for the primary device, "CPUn" for others.
        if (device_index == 0)
            g_memoryMapAddrspaces.emplace_back("");
        else
        {
            char buf[9];
            std::snprintf(buf, sizeof(buf), "CPU%d", device_index);
            g_memoryMapAddrspaces.emplace_back(buf);
        }
        const std::string& asname = g_memoryMapAddrspaces.back();

        for (address_map_entry& entry : space.map()->m_entrylist)
        {
            const bool is_ram_read  = (entry.m_read.m_type  == AMH_RAM);
            const bool is_ram_write = (entry.m_write.m_type == AMH_RAM);

            if (!is_ram_read && !is_ram_write)
                continue;

            void* ptr = space.get_read_ptr(entry.m_addrstart);
            if (!ptr)
                continue;

            retro_memory_descriptor desc = {};
            desc.ptr        = ptr;
            desc.start      = (size_t)entry.m_addrstart;
            desc.len        = (size_t)(entry.m_addrend - entry.m_addrstart + 1);
            desc.offset     = 0;
            desc.select     = 0;
            desc.disconnect = 0;

            uint64_t flags = 0;
            if (is_ram_read && is_ram_write)
                flags |= RETRO_MEMDESC_SYSTEM_RAM;
            else if (is_ram_read)
                flags |= RETRO_MEMDESC_CONST;

            if (big_endian)
                flags |= RETRO_MEMDESC_BIGENDIAN;

            desc.flags     = flags;
            desc.addrspace = asname.empty() ? nullptr : asname.c_str();

            g_memoryMapDescs.push_back(desc);

            log_cb(RETRO_LOG_DEBUG,
                   "libretro_ext: mmap dev=%s start=%08X end=%08X flags=%04llX ptr=%p\n",
                   memory.device().tag(),
                   (unsigned)entry.m_addrstart,
                   (unsigned)entry.m_addrend,
                   (unsigned long long)flags,
                   ptr);
        }

        device_index++;
    }

    if (g_memoryMapDescs.empty())
    {
        log_cb(RETRO_LOG_WARN, "libretro_ext: SET_MEMORY_MAPS: no mappable RAM regions found\n");
        return;
    }

    retro_memory_map mmap = {};
    mmap.descriptors     = g_memoryMapDescs.data();
    mmap.num_descriptors = (unsigned)g_memoryMapDescs.size();

    if (!environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &mmap))
        log_cb(RETRO_LOG_WARN, "libretro_ext: SET_MEMORY_MAPS not supported by frontend\n");
    else
        log_cb(RETRO_LOG_INFO, "libretro_ext: SET_MEMORY_MAPS registered %u descriptor(s)\n",
               mmap.num_descriptors);
}

static void libretro_ext_clear_watch_rules_impl()
{
    g_extWatchRules.clear();
    libretro_ext_clear_native_watchpoints_impl();
}

static void libretro_ext_add_watch_rule_impl(const char* cpuTag,
                                             uint64_t start,
                                             uint64_t end,
                                             uint8_t access,
                                             uint8_t width)
{
    if (!g_extDebugExtensionsEnabled)
        return;

    if (!cpuTag || !cpuTag[0])
        return;

    libretro_ext_watch_rule rule{};
    rule.cpuTag = cpuTag;
    rule.start = start;
    rule.end = end;
    rule.access = access;
    rule.width = width;
    rule.enabled = true;
    g_extWatchRules.push_back(rule);

    running_machine* mach = libretro_ext_machine();
    if (!mach)
        return;

    device_t* dev = mach->root_device().subdevice(cpuTag);
    if (!dev)
    {
        log_cb(RETRO_LOG_INFO, "libretro_ext: no device for tag %s\n", cpuTag);
        return;
    }

    if (!dev->debug())
    {
        log_cb(RETRO_LOG_WARN,
               "libretro_ext: device %s has no debug object; watchpoints require debug extensions enabled before retro_load_game/reload content.\n",
               cpuTag);
        return;
    }

    device_memory_interface* mem = nullptr;
    if (!dev->interface(mem) || !mem->has_space(AS_PROGRAM))
    {
        log_cb(RETRO_LOG_INFO, "libretro_ext: device %s has no AS_PROGRAM space\n", cpuTag);
        return;
    }

    address_space& space = mem->space(AS_PROGRAM);

    offs_t address = (offs_t)start;
    offs_t length  = (offs_t)((end >= start) ? (end - start) : 0);

    // access: 1=read, 2=write, 3=read|write
    if (access & 1)
        dev->debug()->watchpoint_set(space, read_or_write::READ, address, length, nullptr, {});

    if (access & 2)
        dev->debug()->watchpoint_set(space, read_or_write::WRITE, address, length, nullptr, {});

    log_cb(RETRO_LOG_INFO,
           "libretro_ext: installed watch rule cpu=%s start=%llX end=%llX access=%u width=%u\n",
           cpuTag,
           (unsigned long long)start,
           (unsigned long long)end,
           (unsigned)access,
           (unsigned)width);
}

static bool libretro_ext_get_last_watch_hit_impl(libretro_ext_watch_hit* outHit)
{
    if (!outHit)
        return false;

    *outHit = g_extLastWatchHit;
    return g_extLastWatchHit.hit;
}


static void libretro_ext_clear_last_watch_hit_impl()
{
    std::memset(&g_extLastWatchHit, 0, sizeof(g_extLastWatchHit));
}

// ---------------------------------------------------------------------------
// DIP switch implementation
// ---------------------------------------------------------------------------

static void libretro_ext_build_dip_cache(running_machine& mach)
{
    if (&mach == g_dipLastMachine)
        return;

    g_dipFields.clear();
    g_dipLastMachine = &mach;

    for (auto& [tag, port] : mach.ioport().ports())
    {
        for (ioport_field& field : port->fields())
        {
            if (field.type() == IPT_DIPSWITCH)
                g_dipFields.push_back(&field);
        }
    }
}

static int libretro_ext_get_dip_count_impl()
{
    running_machine* mach = libretro_ext_machine();
    if (!mach)
        return 0;

    libretro_ext_build_dip_cache(*mach);
    return (int)g_dipFields.size();
}

static bool libretro_ext_get_dip_info_impl(int index, libretro_ext_dip_info* out)
{
    if (!out)
        return false;

    running_machine* mach = libretro_ext_machine();
    if (!mach)
        return false;

    libretro_ext_build_dip_cache(*mach);

    if (index < 0 || index >= (int)g_dipFields.size())
        return false;

    ioport_field* field = g_dipFields[index];
    std::memset(out, 0, sizeof(*out));

    std::strncpy(out->name, field->name().c_str(),
                 sizeof(out->name) - 1);
    std::strncpy(out->port_tag, field->port().tag(),
                 sizeof(out->port_tag) - 1);

    out->mask          = (uint32_t)field->mask();
    out->default_value = (uint32_t)(field->defvalue() & field->mask());

    ioport_field::user_settings us;
    field->get_user_settings(us);
    out->current_value = (uint32_t)(us.value & field->mask());

    out->setting_count = 0;
    for (const ioport_setting& s : field->settings())
    {
        if (out->setting_count >= LIBRETRO_EXT_DIP_MAX_SETTINGS)
            break;
        libretro_ext_dip_setting& ds = out->settings[out->setting_count++];
        std::strncpy(ds.name, s.name() ? s.name() : "", sizeof(ds.name) - 1);
        ds.value = (uint32_t)(s.value() & field->mask());
    }

    return true;
}

static bool libretro_ext_set_dip_value_impl(int index, uint32_t value)
{
    running_machine* mach = libretro_ext_machine();
    if (!mach)
        return false;

    libretro_ext_build_dip_cache(*mach);

    if (index < 0 || index >= (int)g_dipFields.size())
        return false;

    ioport_field* field = g_dipFields[index];
    ioport_field::user_settings us;
    field->get_user_settings(us);
    us.value = value & field->mask();
    field->set_user_settings(us);
    return true;
}

static const libretro_ext_api g_ext_api = {
    5,
    sizeof(libretro_ext_api),

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

    libretro_ext_get_cpu_total_cycles_by_tag_impl,

    libretro_ext_clear_watch_rules_impl,
    libretro_ext_add_watch_rule_impl,
    libretro_ext_get_last_watch_hit_impl,
    libretro_ext_clear_last_watch_hit_impl,
    libretro_ext_check_watch_hit_impl,

    libretro_ext_get_dip_count_impl,
    libretro_ext_get_dip_info_impl,
    libretro_ext_set_dip_value_impl,

    libretro_ext_set_debug_extensions_enabled_impl,
    libretro_ext_get_debug_extensions_enabled_impl,

    libretro_ext_trigger_timing_capture_impl
};

extern "C" {
    LIBRETRO_EXT_EXPORT const libretro_ext_api* libretro_ext_get_api()
    {
        return &g_ext_api;
    }
}
#endif // LIBRETRO_EXT_HPP
