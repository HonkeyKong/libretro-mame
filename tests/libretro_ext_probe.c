#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct libretro_ext_api_head
{
    uint32_t abi_version;
    uint32_t sizeof_struct;
};

struct libretro_ext_rollback_api_head
{
    uint32_t abi_version;
    uint32_t sizeof_struct;
};

typedef const void* (*get_api_fn_t)(void);
typedef uint32_t (*get_u32_fn_t)(void);

static void print_symbol_status(const char* name, void* sym)
{
    printf("%s: %s\n", name, sym ? "present" : "missing");
}

static int validate_main_api(void* handle)
{
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
        fprintf(stderr, "No libretro_ext API symbol found.\n");
        return 2;
    }

    const struct libretro_ext_api_head* head = (const struct libretro_ext_api_head*)get_api();
    if (!head)
    {
        fprintf(stderr, "libretro_ext_get_api returned NULL.\n");
        return 3;
    }

    uint32_t abi = sym_get_abi ? ((get_u32_fn_t)sym_get_abi)() : head->abi_version;
    uint32_t size = sym_get_size ? ((get_u32_fn_t)sym_get_size)() : head->sizeof_struct;
    printf("main_api abi=%u size=%u\n", abi, size);

    if (abi != 5)
    {
        fprintf(stderr, "Unexpected main API ABI version: %u\n", abi);
        return 4;
    }

    if (size < sizeof(struct libretro_ext_api_head))
    {
        fprintf(stderr, "Main API struct size too small: %u\n", size);
        return 5;
    }

    return 0;
}

static int validate_rollback_api(void* handle)
{
    void* sym_get_api = dlsym(handle, "libretro_ext_get_rollback_api");
    void* sym_get_api_v1 = dlsym(handle, "libretro_ext_get_rollback_api_v1");
    void* sym_get_abi = dlsym(handle, "libretro_ext_get_rollback_api_abi_version");
    void* sym_get_size = dlsym(handle, "libretro_ext_get_rollback_api_struct_size");

    print_symbol_status("libretro_ext_get_rollback_api", sym_get_api);
    print_symbol_status("libretro_ext_get_rollback_api_v1", sym_get_api_v1);
    print_symbol_status("libretro_ext_get_rollback_api_abi_version", sym_get_abi);
    print_symbol_status("libretro_ext_get_rollback_api_struct_size", sym_get_size);

    get_api_fn_t get_api = sym_get_api ? (get_api_fn_t)sym_get_api : (get_api_fn_t)sym_get_api_v1;
    if (!get_api)
    {
        fprintf(stderr, "Rollback API symbol missing.\n");
        return 10;
    }

    const struct libretro_ext_rollback_api_head* head = (const struct libretro_ext_rollback_api_head*)get_api();
    if (!head)
    {
        fprintf(stderr, "libretro_ext_get_rollback_api returned NULL.\n");
        return 11;
    }

    uint32_t abi = sym_get_abi ? ((get_u32_fn_t)sym_get_abi)() : head->abi_version;
    uint32_t size = sym_get_size ? ((get_u32_fn_t)sym_get_size)() : head->sizeof_struct;
    printf("rollback_api abi=%u size=%u\n", abi, size);

    if (abi != 1)
    {
        fprintf(stderr, "Unexpected rollback API ABI version: %u\n", abi);
        return 12;
    }

    if (size < sizeof(struct libretro_ext_rollback_api_head))
    {
        fprintf(stderr, "Rollback API struct size too small: %u\n", size);
        return 13;
    }

    return 0;
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

    int rc = validate_main_api(handle);
    if (rc == 0)
        rc = validate_rollback_api(handle);

    dlclose(handle);
    return rc;
}
