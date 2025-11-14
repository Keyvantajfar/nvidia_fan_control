#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <nvml.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

#include "config.h"
#include "log.h"

#define NVFC_DEFAULT_CONFIG_PATH "/etc/nvidia-fan-controlV2.conf"

#define NVFC_MIN_FAN_SPEED_STEP 4
#define NVFC_MIN_TEMP_DELTA_FOR_CHANGE 1
#define NVFC_MIN_SECONDS_BETWEEN_CHANGES 4
#define NVFC_MIN_SLEEP_SECONDS 1

#define NVFC_LOG_TEMP_DELTA 2
#define NVFC_LOG_INTERVAL_LOW 20
#define NVFC_LOG_INTERVAL_HIGH 6

static volatile sig_atomic_t g_keep_running = 1;
static volatile sig_atomic_t g_reload_config = 0;

static int g_current_fan_speed = -1;
static int g_last_change_temp = 0;
static time_t g_last_change_time = 0;
static int g_last_logged_temp = INT_MIN;
static int g_last_logged_speed = -1;
static time_t g_last_log_time = 0;

static void nvfc_reset_loop_state(void) {
    g_current_fan_speed = -1;
    g_last_change_temp = 0;
    g_last_change_time = 0;
    g_last_logged_temp = INT_MIN;
    g_last_logged_speed = -1;
    g_last_log_time = 0;
}

static void nvfc_handle_signal(int sig) {
    (void)sig;
    g_keep_running = 0;
}

static void nvfc_handle_sighup(int sig) {
    (void)sig;
    g_reload_config = 1;
}

static int nvfc_compute_speed(const NvfcConfig *cfg, int temperature) {
    if (!cfg || cfg->num_points <= 0) {
        return 0;
    }
    for (int i = 0; i < cfg->num_points - 1; ++i) {
        if (temperature < cfg->temps[i + 1]) {
            return cfg->speeds[i];
        }
    }
    return cfg->speeds[cfg->num_points - 1];
}

static void nvfc_join_values(const int *values, int count, char *buffer, size_t buffer_size) {
    if (!values || !buffer || buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';
    size_t offset = 0;
    for (int i = 0; i < count; ++i) {
        int written = snprintf(buffer + offset, buffer_size - offset, "%s%d", (i == 0 ? "" : ","), values[i]);
        if (written < 0) {
            buffer[buffer_size - 1] = '\0';
            return;
        }
        if ((size_t)written >= buffer_size - offset) {
            buffer[buffer_size - 1] = '\0';
            return;
        }
        offset += (size_t)written;
    }
}

static void nvfc_log_startup(const NvfcConfig *cfg) {
    char temps_buf[256];
    char speeds_buf[256];
    nvfc_join_values(cfg->temps, cfg->num_points, temps_buf, sizeof(temps_buf));
    nvfc_join_values(cfg->speeds, cfg->num_points, speeds_buf, sizeof(speeds_buf));
    nvfc_log_info(
        "NVIDIA Fan Control started (GPU=%d, points=%d, temps=[%s], speeds=[%s], sleep_low=%d, "
        "sleep_high=%d, high_temp_threshold=%d, smoothing={min_step=%d%%, min_temp_delta=%d°C, min_interval=%ds})",
        cfg->gpu_index,
        cfg->num_points,
        temps_buf,
        speeds_buf,
        cfg->sleep_low,
        cfg->sleep_high,
        cfg->high_temp_threshold,
        NVFC_MIN_FAN_SPEED_STEP,
        NVFC_MIN_TEMP_DELTA_FOR_CHANGE,
        NVFC_MIN_SECONDS_BETWEEN_CHANGES);
}

static bool nvfc_should_log_iteration(const NvfcConfig *cfg, int temperature, int desired_speed) {
    if (!cfg) {
        return false;
    }

    time_t now = time(NULL);
    if (g_last_logged_temp == INT_MIN || g_last_log_time == 0) {
        g_last_logged_temp = temperature;
        g_last_logged_speed = desired_speed;
        g_last_log_time = now;
        return true;
    }

    int temp_delta = temperature - g_last_logged_temp;
    if (temp_delta < 0) {
        temp_delta = -temp_delta;
    }

    int speed_delta = desired_speed - g_last_logged_speed;
    if (speed_delta < 0) {
        speed_delta = -speed_delta;
    }

    int interval = (temperature >= cfg->high_temp_threshold) ? NVFC_LOG_INTERVAL_HIGH : NVFC_LOG_INTERVAL_LOW;
    int elapsed = (int)difftime(now, g_last_log_time);

    if (temp_delta >= NVFC_LOG_TEMP_DELTA || speed_delta >= NVFC_MIN_FAN_SPEED_STEP || elapsed >= interval) {
        g_last_logged_temp = temperature;
        g_last_logged_speed = desired_speed;
        g_last_log_time = now;
        return true;
    }

    return false;
}

static const char *nvfc_select_config_path(int argc, char **argv) {
    const char *cli_path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                nvfc_log_error("--config requires a path argument");
                exit(EXIT_FAILURE);
            }
            cli_path = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--config <path>]\n", argv[0]);
            exit(EXIT_SUCCESS);
        } else {
            nvfc_log_warn("Ignoring unknown argument: %s", argv[i]);
        }
    }

    if (cli_path) {
        return cli_path;
    }
    const char *env_path = getenv("NVFC_CONFIG_PATH");
    if (env_path && *env_path) {
        return env_path;
    }
    return NVFC_DEFAULT_CONFIG_PATH;
}

static void nvfc_apply_smoothing(int current_temp, int desired_speed, nvmlDevice_t device, unsigned int fan_index) {
    if (g_current_fan_speed < 0) {
        if (nvmlDeviceSetFanSpeed_v2(device, fan_index, desired_speed) != NVML_SUCCESS) {
            nvfc_log_error("Failed to set fan speed to %d%%", desired_speed);
            return;
        }
        g_current_fan_speed = desired_speed;
        g_last_change_temp = current_temp;
        g_last_change_time = time(NULL);
        nvfc_log_info("Updated Fan Speed: %d%%", desired_speed);
        return;
    }

    int delta_speed = desired_speed - g_current_fan_speed;
    if (delta_speed < 0) {
        delta_speed = -delta_speed;
    }
    if (delta_speed < NVFC_MIN_FAN_SPEED_STEP) {
        return;
    }

    int delta_temp = current_temp - g_last_change_temp;
    if (delta_temp < 0) {
        delta_temp = -delta_temp;
    }

    time_t now = time(NULL);
    int delta_time = (int)difftime(now, g_last_change_time);
    if (delta_temp < NVFC_MIN_TEMP_DELTA_FOR_CHANGE && delta_time < NVFC_MIN_SECONDS_BETWEEN_CHANGES) {
        return;
    }

    if (nvmlDeviceSetFanSpeed_v2(device, fan_index, desired_speed) != NVML_SUCCESS) {
        nvfc_log_error("Failed to set fan speed to %d%%", desired_speed);
        return;
    }
    g_current_fan_speed = desired_speed;
    g_last_change_temp = current_temp;
    g_last_change_time = now;
    nvfc_log_info("Updated Fan Speed: %d%%", desired_speed);
}

static void nvfc_maybe_reload_config(const char *config_path, NvfcConfig *cfg) {
    if (!g_reload_config) {
        return;
    }
    g_reload_config = 0;
    NvfcConfig new_cfg = *cfg;
    nvfc_set_defaults(&new_cfg);
    if (nvfc_load_config(config_path, &new_cfg) == 0) {
        *cfg = new_cfg;
        nvfc_reset_loop_state();
//         g_current_fan_speed = -1;
//         g_last_change_temp = 0;
//         g_last_change_time = 0;
        nvfc_log_info("Reloaded config from %s", config_path);
    } else {
        nvfc_log_warn("Failed to reload config from %s, keeping previous values", config_path);
    }
}

int main(int argc, char **argv) {
    const char *config_path = nvfc_select_config_path(argc, argv);

    struct sigaction term_action = {0};
    term_action.sa_handler = nvfc_handle_signal;
    sigaction(SIGINT, &term_action, NULL);
    sigaction(SIGTERM, &term_action, NULL);

    struct sigaction hup_action = {0};
    hup_action.sa_handler = nvfc_handle_sighup;
    sigaction(SIGHUP, &hup_action, NULL);

    NvfcConfig cfg;
    nvfc_set_defaults(&cfg);
    NvfcConfig loaded = cfg;
    if (nvfc_load_config(config_path, &loaded) == 0) {
        cfg = loaded;
    } else {
        nvfc_log_warn("Unable to load %s; using built-in defaults", config_path);
    }

    if (nvmlInit() != NVML_SUCCESS) {
        nvfc_log_error("Failed to initialize NVML");
        return EXIT_FAILURE;
    }

    nvmlDevice_t device;
    if (nvmlDeviceGetHandleByIndex((unsigned int)cfg.gpu_index, &device) != NVML_SUCCESS) {
        nvfc_log_error("Failed to get GPU handle for index %d", cfg.gpu_index);
        nvmlShutdown();
        return EXIT_FAILURE;
    }

    unsigned int fan_count = 0;
    if (nvmlDeviceGetNumFans(device, &fan_count) != NVML_SUCCESS || fan_count == 0) {
        nvfc_log_error("Failed to determine GPU fan count");
        nvmlShutdown();
        return EXIT_FAILURE;
    }

    nvfc_reset_loop_state();
    nvfc_log_startup(&cfg);

    while (g_keep_running) {
        nvfc_maybe_reload_config(config_path, &cfg);

        unsigned int temperature_raw = 0;
        nvmlReturn_t temp_status = nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &temperature_raw);
        if (temp_status != NVML_SUCCESS) {
            nvfc_log_error("Failed to read GPU temperature");
            break;
        }
        int temperature = (int)temperature_raw;
        int desired_speed = nvfc_compute_speed(&cfg, temperature);
        if (desired_speed < 0) {
            desired_speed = 0;
        } else if (desired_speed > 100) {
            desired_speed = 100;
        }

        if (nvfc_should_log_iteration(&cfg, temperature, desired_speed)) {
            nvfc_log_info("Temp: %d°C -> Fan Speed: %d%%", temperature, desired_speed);
        }
//         nvfc_log_info("Temp: %d°C -> Fan Speed: %d%%", temperature, desired_speed);

        nvfc_apply_smoothing(temperature, desired_speed, device, 0);

        int sleep_time = temperature >= cfg.high_temp_threshold ? cfg.sleep_high : cfg.sleep_low;
        if (sleep_time < NVFC_MIN_SLEEP_SECONDS) {
            sleep_time = NVFC_MIN_SLEEP_SECONDS;
        }
        sleep((unsigned int)sleep_time);
    }

    nvmlDeviceSetDefaultFanSpeed_v2(device, 0);
    nvmlShutdown();
    nvfc_log_info("Fan control stopped, resetting to auto mode.");
    return EXIT_SUCCESS;
}
