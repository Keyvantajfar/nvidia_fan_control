#define _POSIX_C_SOURCE 200809L

#include "nvml_dl.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

/* dlsym returns void*; this idiom avoids the object->function pointer cast
 * warning under -Wpedantic. */
static void *nvfc_sym(void *lib, const char *versioned, const char *plain) {
    void *p = dlsym(lib, versioned);
    if (!p && plain) {
        p = dlsym(lib, plain);
    }
    return p;
}

#define NVFC_BIND(field, versioned, plain)                         \
    do {                                                           \
        void *sym_ = nvfc_sym(api->lib, (versioned), (plain));     \
        if (!sym_) {                                               \
            nvfc_log_error("NVML symbol missing: %s", (versioned)); \
            nvfc_nvml_unload(api);                                 \
            return -1;                                             \
        }                                                          \
        memcpy(&api->field, &sym_, sizeof(sym_));                  \
    } while (0)

int nvfc_nvml_load(NvfcNvml *api) {
    memset(api, 0, sizeof(*api));

#ifdef NVFC_MOCK_NVML
    /* Test builds only: pick the mock library, never an arbitrary env path in
     * production. */
    const char *override = getenv("NVFC_NVML_LIB");
    if (override && *override) {
        api->lib = dlopen(override, RTLD_NOW);
    }
#ifdef NVFC_MOCK_NVML_PATH
    if (!api->lib) {
        api->lib = dlopen(NVFC_MOCK_NVML_PATH, RTLD_NOW);
    }
#endif
#endif
    if (!api->lib) {
        api->lib = dlopen("libnvidia-ml.so.1", RTLD_NOW);
    }
    if (!api->lib) {
        api->lib = dlopen("libnvidia-ml.so", RTLD_NOW);
    }
    if (!api->lib) {
        nvfc_log_error("Unable to load libnvidia-ml.so.1 (is the NVIDIA driver installed?)");
        return -1;
    }

    NVFC_BIND(init, "nvmlInit_v2", "nvmlInit");
    NVFC_BIND(shutdown, "nvmlShutdown", NULL);
    NVFC_BIND(get_handle, "nvmlDeviceGetHandleByIndex_v2", "nvmlDeviceGetHandleByIndex");
    NVFC_BIND(num_fans, "nvmlDeviceGetNumFans", NULL);
    NVFC_BIND(get_temp, "nvmlDeviceGetTemperature", NULL);
    NVFC_BIND(set_fan, "nvmlDeviceSetFanSpeed_v2", NULL);
    NVFC_BIND(set_default_fan, "nvmlDeviceSetDefaultFanSpeed_v2", NULL);
    return 0;
}

void nvfc_nvml_unload(NvfcNvml *api) {
    if (api && api->lib) {
        dlclose(api->lib);
    }
    if (api) {
        memset(api, 0, sizeof(*api));
    }
}
