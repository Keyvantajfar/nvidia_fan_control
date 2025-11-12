#ifndef NVML_H_MOCK
#define NVML_H_MOCK

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef void* nvmlDevice_t;

typedef enum {
    NVML_SUCCESS = 0,
    NVML_ERROR_UNINITIALIZED = 1,
    NVML_ERROR_UNKNOWN = 999
} nvmlReturn_t;

#define NVML_TEMPERATURE_GPU 0

// Functions used by your program
nvmlReturn_t nvmlInit(void);
nvmlReturn_t nvmlShutdown(void);
nvmlReturn_t nvmlDeviceGetHandleByIndex(unsigned int index, nvmlDevice_t *device);
nvmlReturn_t nvmlDeviceGetNumFans(nvmlDevice_t device, unsigned int *fanCount);
nvmlReturn_t nvmlDeviceGetTemperature(nvmlDevice_t device, unsigned int sensorType, unsigned int *temp);
nvmlReturn_t nvmlDeviceSetFanSpeed_v2(nvmlDevice_t device, unsigned int fanIndex, unsigned int speed);
nvmlReturn_t nvmlDeviceSetDefaultFanSpeed_v2(nvmlDevice_t device, unsigned int fanIndex);

#ifdef __cplusplus
}
#endif

#endif // NVML_H_MOCK
