#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct libretro_ext_api_head
{
    uint32_t abi_version;
    uint32_t sizeof_struct;
};

typedef const void* (*get_api_fn_t)(void);
typedef uint32_t (*get_u32_fn_t)(void);

static void print_symbol_status(const char* name, void* ptr)
{
    printf("%-34s : %s\n", name, ptr ? "FOUND" : "MISSING");
}

int main(int argc, char** argv)
{
    const char* core_path = (argc > 1) ? argv[1] : "./capcom_libretro.so";

    printf("Core: %s\n", core_path);
    void* handle = dlopen(core_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle)
    {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    void* sym_get_api = dlsym(handle, "libretro_ext_get_api");
    void* sym_get_api_v5 = dlsym(handle, "libretro_ext_get_api_v5");
    void* sym_get_abi = dlsym(handle, "libretro_ext_get_api_abi_version");
    void* sym_get_size = dlsym(handle, "libretro_ext_get_api_struct_size");

    print_symbol_status("libretro_ext_get_api", sym_get_api);
    print_symbol_status("libretro_ext_get_api_v5", sym_get_api_v5);
    print_symbol_status("libretro_ext_get_api_abi_version", sym_get_abi);
    print_symbol_status("libretro_ext_get_api_struct_size", sym_get_size);

    get_api_fn_t get_api = sym_get_api ? (get_api_fn_t)sym_get_api : (get_api_fn_t)sym_get_api_v5;
    if (!get_api)
    {
        fprintf(stderr, "No API getter symbol found.\n");
        dlclose(handle);
        return 2;
    }

    const void* api_ptr = get_api();
    if (!api_ptr)
    {
        fprintf(stderr, "API getter returned NULL.\n");
        dlclose(handle);
        return 3;
    }

    uint32_t abi = 0;
    uint32_t size = 0;

    if (sym_get_abi)
        abi = ((get_u32_fn_t)sym_get_abi)();
    if (sym_get_size)
        size = ((get_u32_fn_t)sym_get_size)();

    if (!abi || !size)
    {
        const struct libretro_ext_api_head* head = (const struct libretro_ext_api_head*)api_ptr;
        if (!abi)
            abi = head->abi_version;
        if (!size)
            size = head->sizeof_struct;
    }

    printf("api_ptr                             : %p\n", api_ptr);
    printf("abi_version                         : %u\n", (unsigned)abi);
    printf("sizeof_struct                       : %u\n", (unsigned)size);

    if (abi != 5)
        fprintf(stderr, "WARNING: expected abi_version 5, got %u\n", (unsigned)abi);

    if (size < sizeof(struct libretro_ext_api_head))
    {
        fprintf(stderr, "ERROR: struct size too small (%u)\n", (unsigned)size);
        dlclose(handle);
        return 4;
    }

    dlclose(handle);
    return (abi == 5) ? 0 : 5;
}
