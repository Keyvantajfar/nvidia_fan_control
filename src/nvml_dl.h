#ifndef NVFC_NVML_DL_H
#define NVFC_NVML_DL_H

/* Minimal runtime binding to libnvidia-ml.
 *
 * The daemon only needs seven NVML calls, so instead of linking against the
 * NVIDIA dev package (and needing nvml.h at build time) they are resolved with
 * dlopen()/dlsym() when the daemon starts. The driver always ships
 * libnvidia-ml.so.1, so nothing but gcc + make is required to build. */

typedef void *NvfcDevice;

typedef struct {
    void *lib;
    int (*init)(void);
    int (*shutdown)(void);
    int (*get_handle)(unsigned int index, NvfcDevice *device);
    int (*num_fans)(NvfcDevice device, unsigned int *count);
    int (*get_temp)(NvfcDevice device, unsigned int sensor, unsigned int *temp);
    int (*set_fan)(NvfcDevice device, unsigned int fan, unsigned int speed);
    int (*set_default_fan)(NvfcDevice device, unsigned int fan);
} NvfcNvml;

#define NVFC_NVML_SUCCESS 0
#define NVFC_NVML_TEMPERATURE_GPU 0

/* Returns 0 on success; on failure a message has been logged. */
int nvfc_nvml_load(NvfcNvml *api);
void nvfc_nvml_unload(NvfcNvml *api);

#endif  /* NVFC_NVML_DL_H */
