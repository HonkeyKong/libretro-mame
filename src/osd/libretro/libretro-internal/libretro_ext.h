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

#ifndef LIBRETRO_EXT_H
#define LIBRETRO_EXT_H

#define LIBRETRO_EXT_DEBUG 1

#include <string>
#include <cstdint>

#include "emu.h"
#include "libretro.h"

#if defined(_WIN32)
  #define LIBRETRO_EXT_EXPORT extern "C" __declspec(dllexport)
#else
  #define LIBRETRO_EXT_EXPORT extern "C" __attribute__((visibility("default")))
#endif

enum libretro_ext_watch_access : uint8_t
{
    LIBRETRO_EXT_WATCH_READ  = 1,
    LIBRETRO_EXT_WATCH_WRITE = 2
};

struct libretro_ext_exec_trigger
{
    std::string cpuTag;
    uint64_t pcStart = 0;
    uint64_t pcEnd   = 0;
    bool enabled     = false;
    bool oneShot     = false;
    bool disableAfterHit = false;
};

struct libretro_ext_exec_hit
{
    bool hit = false;
    char cpuTag[64]{};
    uint64_t pc = 0;
    uint64_t frame = 0;
    uint64_t totalCycles = 0;
};

struct libretro_ext_pc_ring
{
    static constexpr size_t kSize = 256;
    uint64_t pcs[kSize]{};
    uint32_t head = 0;
    bool filled = false;

    void push(uint64_t pc)
    {
        pcs[head] = pc;
        head = (head + 1) % kSize;
        if (head == 0)
            filled = true;
    }
};

struct libretro_ext_watch_rule
{
    std::string cpuTag;
    uint64_t start = 0;
    uint64_t end = 0;
    uint8_t access = 0; // read/write
    uint8_t width = 0;  // 0=any, 1/2/4
    bool enabled = false;
};

struct libretro_ext_watch_hit
{
    bool hit = false;
    char cpuTag[64]{};
    uint64_t address = 0;
    uint64_t pc = 0;
    uint64_t frame = 0;
    uint64_t totalCycles = 0;
    uint32_t value = 0;
    uint8_t access = 0;
    uint8_t width = 0;
    uint32_t historyCount = 0;
    uint64_t pcHistory[256]{};
};

enum libretro_ext_cpu_state_id : int32_t
{
    LIBRETRO_EXT_STATE_GENPC = -1,
    LIBRETRO_EXT_STATE_GENPCBASE = -2,
    LIBRETRO_EXT_STATE_GENFLAGS = -3
};

// DIP switch descriptor structs (versioned, fixed-size, C ABI safe)
#define LIBRETRO_EXT_DIP_NAME_LEN      128
#define LIBRETRO_EXT_DIP_PORT_TAG_LEN   64
#define LIBRETRO_EXT_DIP_MAX_SETTINGS   64

// One named setting (e.g. "Normal" = 0x04) within a DIP switch field.
struct libretro_ext_dip_setting
{
    char     name[LIBRETRO_EXT_DIP_NAME_LEN]; // human-readable label
    uint32_t value;                            // raw masked value for this setting
};

// Complete descriptor for one logical DIP switch field.
struct libretro_ext_dip_info
{
    char     name[LIBRETRO_EXT_DIP_NAME_LEN];        // e.g. "Difficulty"
    char     port_tag[LIBRETRO_EXT_DIP_PORT_TAG_LEN]; // e.g. ":DSWB"
    uint32_t mask;                                    // bit mask within the port
    uint32_t current_value;                           // current value (within mask)
    uint32_t default_value;                           // default value (within mask)
    uint32_t setting_count;                           // number of valid entries in settings[]
    libretro_ext_dip_setting settings[LIBRETRO_EXT_DIP_MAX_SETTINGS];
};

extern uint64_t g_extFrameCounter;
extern libretro_ext_watch_hit g_extLastWatchHit;
extern std::vector<libretro_ext_watch_rule> g_extWatchRules;
extern std::unordered_map<std::string, libretro_ext_pc_ring> g_extPcHistory;

void libretro_ext_record_pc(const char* cpuTag, uint64_t pc);

void libretro_ext_set_memory_maps(retro_environment_t environ_cb);

void libretro_ext_record_watch_hit(const char* cpuTag,
                                   uint64_t pc,
                                   uint64_t address,
                                   uint32_t value,
                                   uint8_t access,
                                   uint8_t width,
                                   uint64_t totalCycles);


struct libretro_ext_api
{
    uint32_t abi_version   = 5;
    uint32_t sizeof_struct = sizeof(libretro_ext_api);

    const char* (*get_driver_name)();
    int         (*get_cpu_count)();
    const char* (*get_cpu_tag)(int cpu_index);
    uint64_t    (*get_cpu_pc)(int cpu_index);

    uint8_t  (*read_u8)(const char* cpu_tag, const char* space, uint64_t addr);
    uint16_t (*read_u16)(const char* cpu_tag, const char* space, uint64_t addr);
    uint32_t (*read_u32)(const char* cpu_tag, const char* space, uint64_t addr);

    void (*write_u8)(const char* cpu_tag, const char* space, uint64_t addr, uint8_t v);
    void (*write_u16)(const char* cpu_tag, const char* space, uint64_t addr, uint16_t v);
    void (*write_u32)(const char* cpu_tag, const char* space, uint64_t addr, uint32_t v);

    int         (*get_region_count)();
    const char* (*get_region_tag)(int index);
    uint64_t    (*get_region_size)(const char* region_tag);

    uint64_t (*read_region)(const char* region_tag, uint64_t offset, void* dst, uint64_t bytes);
    uint64_t (*write_region)(const char* region_tag, uint64_t offset, const void* src, uint64_t bytes);

    uint64_t (*get_frame_number)();
    uint64_t (*get_time_attoseconds)();
    uint64_t (*get_cpu_total_cycles_by_tag)(const char* cpu_tag);

    void (*clear_watch_rules)();
    void (*add_watch_rule)(const char* cpuTag, uint64_t start, uint64_t end, uint8_t access, uint8_t width);
    bool (*get_last_watch_hit)(libretro_ext_watch_hit* outHit);
    void (*clear_last_watch_hit)();
    void (*check_watch_hit)(const char* cpuTag, uint64_t pc, uint64_t address, uint32_t value, uint8_t access, uint8_t width, uint64_t totalCycles);

    int  (*get_dip_count)();
    bool (*get_dip_info)(int index, libretro_ext_dip_info* out);
    bool (*set_dip_value)(int index, uint32_t value);

    void (*set_debug_extensions_enabled)(bool enabled);
    bool (*get_debug_extensions_enabled)();

    // Restart the current driver's timing capture window if supported.
    bool (*trigger_timing_capture)();

    // Extended CPU accessors — added after v5 base; check sizeof_struct before using.
    uint64_t (*get_cpu_pc_by_tag)(const char* cpu_tag);
    bool     (*read_cpu_state_u64_by_index)(int cpu_index, int state_id, uint64_t* out_value);
    bool     (*read_cpu_state_u64_by_tag)(const char* cpu_tag, int state_id, uint64_t* out_value);
};

static void invalidate_region_cache();
static void ext_clear_exec_triggers_impl();
static void log_all_devices(running_machine& mach);
static void libretro_ext_clear_last_exec_hit_impl();
static void build_region_cache(running_machine& mach);
static void libretro_ext_install_exec_hooks_if_needed();
static void libretro_ext_check_exec_triggers(device_t& dev, uint64_t pc);
static void libretro_ext_set_debug_extensions_enabled_impl(bool enabled);
static void libretro_ext_write_u8_impl(const char* cpu_tag, const char* space_name, uint64_t addr, uint8_t v);
static void libretro_ext_write_u16_impl(const char* cpu_tag, const char* space_name, uint64_t addr, uint16_t v);
static void libretro_ext_write_u32_impl(const char* cpu_tag, const char* space_name, uint64_t addr, uint32_t v);
static void libretro_ext_add_exec_trigger_impl(const char* cpuTag, uint64_t pcStart, uint64_t pcEnd,
                                      bool oneShot, bool disableAfterHit);
static void libretro_ext_check_watch_hit_impl(const char* cpuTag,
                                uint64_t pc,
                                uint64_t address,
                                uint32_t value,
                                uint8_t access,
                                uint8_t width,
                                uint64_t totalCycles);

static inline void check_exec_triggers(const char* cpuTag, uint32_t pc);

static int libretro_ext_cpu_count_impl();
static int space_name_to_id(const char* name);
static int libretro_ext_get_region_count_impl();

static const char* libretro_ext_get_driver_name_impl();
static const char* libretro_ext_get_region_tag_impl(int index);
static const char* libretro_ext_get_cpu_tag_impl(int cpu_index);

static bool libretro_ext_get_last_exec_hit_impl(libretro_ext_exec_hit* outHit);
static bool libretro_ext_get_debug_extensions_enabled_impl();
static bool libretro_ext_get_cpu_by_index(running_machine& mach, int cpu_index, device_t*& out_dev);

static uint8_t libretro_ext_read_u8_impl(const char* cpu_tag, const char* space_name, uint64_t addr);
static uint16_t libretro_ext_read_u16_impl(const char* cpu_tag, const char* space_name, uint64_t addr);
static uint32_t libretro_ext_read_u32_impl(const char* cpu_tag, const char* space_name, uint64_t addr);

static uint64_t libretro_ext_get_frame_number_impl();
static uint64_t libretro_ext_get_time_attoseconds_impl();
static uint64_t libretro_ext_get_cpu_pc_impl(int cpu_index);
static uint64_t libretro_ext_get_cpu_pc_by_tag_impl(const char* cpu_tag);
static uint64_t libretro_ext_get_cpu_genpc_impl(int cpu_index);
static uint64_t libretro_ext_get_region_size_impl(const char* tag);
static uint64_t libretro_ext_get_cpu_genpcbase_impl(int cpu_index);
static bool libretro_ext_read_cpu_state_u64_by_index_impl(int cpu_index, int state_id, uint64_t* out_value);
static bool libretro_ext_read_cpu_state_u64_by_tag_impl(const char* cpu_tag, int state_id, uint64_t* out_value);
static uint64_t get_state_u64(device_state_interface& st, int state_id);
static uint64_t libretro_ext_get_cpu_total_cycles_impl(const char* cpu_tag);
static uint64_t libretro_ext_get_cpu_total_cycles_by_tag_impl(const char* cpu_tag);
static uint64_t libretro_ext_read_region_impl(const char* tag, uint64_t offset, void* dst, uint64_t bytes);
static uint64_t libretro_ext_write_region_impl(const char* tag, uint64_t offset, const void* src, uint64_t bytes);

static running_machine* libretro_ext_machine();

static screen_device* libretro_ext_first_screen(running_machine& mach);

static memory_region* find_region(running_machine& mach, const char* tag);

static device_t* find_device_by_tag(running_machine& mach, const char* tag);
static device_t* libretro_ext_find_device(running_machine& mach, const char* tag);

static address_space* get_space_by_tag(running_machine& mach, const char* cpu_tag, const char* space_name);

static int  libretro_ext_get_dip_count_impl();
static bool libretro_ext_get_dip_info_impl(int index, libretro_ext_dip_info* out);
static bool libretro_ext_set_dip_value_impl(int index, uint32_t value);
static bool libretro_ext_trigger_timing_capture_impl();

extern "C" {
    LIBRETRO_EXT_EXPORT const libretro_ext_api* libretro_ext_get_api();
    LIBRETRO_EXT_EXPORT const libretro_ext_api* libretro_ext_get_api_v5();
    LIBRETRO_EXT_EXPORT uint32_t libretro_ext_get_api_abi_version();
    LIBRETRO_EXT_EXPORT uint32_t libretro_ext_get_api_struct_size();
}

#endif // LIBRETRO_EXT_H
