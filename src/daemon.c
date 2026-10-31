#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "nvml_dl.h"

#ifndef NVFC_VERSION
#define NVFC_VERSION "dev"
#endif

#define NVFC_DEFAULT_CONFIG_PATH "/etc/nvidia-fan-control.conf"
#define NVFC_DEFAULT_STATUS_PATH "/run/nvidia-fan-control/status"

/* Classic (Smart Smooth off) write limits. */
#define NVFC_LEGACY_MIN_STEP 4
#define NVFC_LEGACY_MIN_INTERVAL 4

/* Smart Smooth temperature filter: follow rises fast, falls slowly. */
#define NVFC_FILTER_ATTACK 0.5
#define NVFC_FILTER_DECAY 0.1

/* Adaptive polling: back off only while cool and stable. */
#define NVFC_STABLE_BAND 2       /* a move of this many degrees snaps polling back to fast */
#define NVFC_STABLE_POLLS 3      /* quiet polls before the interval starts doubling */

#define NVFC_TEMP_READ_RETRIES 3
#define NVFC_STATUS_HEARTBEAT 60

#define NVFC_LOG_TEMP_DELTA 2
#define NVFC_LOG_SPEED_DELTA 4
#define NVFC_LOG_HOT_INTERVAL 6

static volatile sig_atomic_t g_keep_running = 1;
static volatile sig_atomic_t g_reload_config = 0;

typedef struct {
    int fan;               /* last speed written, -1 = not yet set */
    time_t last_write;
    double filt;           /* Smart Smooth filtered temperature */
    int have_filt;
    time_t down_since;     /* when a pending ramp-down was first seen, 0 = none */
    int anchor;            /* temperature the current quiet run started at */
    int stable;            /* consecutive quiet polls */
    int sleep_now;         /* current adaptive poll interval */
    int log_temp;
    int log_target;
    time_t log_time;
    int status_temp;
    int status_fan;
    int status_interval;
    time_t status_time;
    bool status_disabled;
} NvfcState;

static void nvfc_reset_state(NvfcState *st) {
    memset(st, 0, sizeof(*st));
    st->fan = -1;
    st->log_temp = INT_MIN;
    st->status_temp = INT_MIN;
    st->status_fan = INT_MIN;
    st->status_interval = INT_MIN;
}

/* After a config reload keep what the fan is doing so a reload by itself never
 * causes a write; only forget timing/backoff state. */
static void nvfc_soft_reset_state(NvfcState *st) {
    st->down_since = 0;
    st->stable = 0;
    st->sleep_now = 0;
    st->log_time = 0;
}

static void nvfc_handle_signal(int sig) {
    (void)sig;
    g_keep_running = 0;
}

static void nvfc_handle_sighup(int sig) {
    (void)sig;
    g_reload_config = 1;
}

/* Classic stepped curve: a point means "from this temperature up". */
static int nvfc_compute_step(const NvfcConfig *cfg, int temperature) {
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

/* Smart Smooth curve: straight lines between points, flat outside them. */
static double nvfc_interpolate(const NvfcConfig *cfg, double t) {
    int n = cfg->num_points;
    if (n <= 0) {
        return 0.0;
    }
    if (t <= cfg->temps[0]) {
        return cfg->speeds[0];
    }
    if (t >= cfg->temps[n - 1]) {
        return cfg->speeds[n - 1];
    }
    for (int i = 0; i < n - 1; ++i) {
        if (t < cfg->temps[i + 1]) {
            double span = (double)(cfg->temps[i + 1] - cfg->temps[i]);
            double frac = (t - cfg->temps[i]) / span;
            return cfg->speeds[i] + (cfg->speeds[i + 1] - cfg->speeds[i]) * frac;
        }
    }
    return cfg->speeds[n - 1];
}

static int nvfc_clamp_percent(int v) {
    return v < 0 ? 0 : (v > 100 ? 100 : v);
}

static int nvfc_abs(int v) {
    return v < 0 ? -v : v;
}

/* Decide what (if anything) to write. Returns the new speed, or -1 for "leave
 * the fan alone". *target_out receives the curve's desired speed. */
static int nvfc_decide(const NvfcConfig *cfg, NvfcState *st, int temp, time_t now, int *target_out) {
    bool emergency = temp >= cfg->smooth_emergency_temp;

    if (!cfg->smooth_enabled) {
        int target = nvfc_clamp_percent(nvfc_compute_step(cfg, temp));
        *target_out = target;
        if (st->fan < 0) {
            return target;
        }
        if (nvfc_abs(target - st->fan) < NVFC_LEGACY_MIN_STEP) {
            return -1;
        }
        if (!emergency && (int)difftime(now, st->last_write) < NVFC_LEGACY_MIN_INTERVAL) {
            return -1;
        }
        return target;
    }

    if (!st->have_filt || emergency) {
        st->filt = temp;
        st->have_filt = 1;
    } else if (temp > st->filt) {
        st->filt += NVFC_FILTER_ATTACK * (temp - st->filt);
    } else {
        st->filt += NVFC_FILTER_DECAY * (temp - st->filt);
    }

    int target = nvfc_clamp_percent((int)(nvfc_interpolate(cfg, st->filt) + 0.5));
    *target_out = target;
    if (st->fan < 0) {
        return target;
    }

    int diff = target - st->fan;
    if (diff > 0) {
        st->down_since = 0;
        if (emergency || diff >= cfg->smooth_deadband || target >= 100) {
            return target;
        }
        return -1;
    }
    if (diff == 0) {
        st->down_since = 0;
        return -1;
    }
    if (-diff < cfg->smooth_deadband) {
        st->down_since = 0;
        return -1;
    }
    if (emergency) {
        st->down_since = 0;
        return target;
    }
    /* Ramp-down: lower target must persist, then drop in small steps. */
    if (st->down_since == 0) {
        st->down_since = now;
    }
    if ((int)difftime(now, st->down_since) < cfg->smooth_down_hold) {
        return -1;
    }
    int next = st->fan - cfg->smooth_down_step;
    if (next < target) {
        next = target;
    }
    st->down_since = now;
    return next;
}

/* Poll interval for the next iteration. Backs off (doubling up to sleep_max)
 * only while the GPU is below high_temp_threshold, quiet, and no ramp-down is
 * pending. Any real movement or a hot GPU returns to the base interval at once. */
static int nvfc_next_sleep(const NvfcConfig *cfg, NvfcState *st, int temp) {
    bool hot = temp >= cfg->high_temp_threshold;
    int base = hot ? cfg->sleep_high : cfg->sleep_low;
    if (base < 1) {
        base = 1;
    }

    if (hot || st->down_since != 0 || st->sleep_now == 0 ||
        nvfc_abs(temp - st->anchor) >= NVFC_STABLE_BAND) {
        st->anchor = temp;
        st->stable = 0;
        st->sleep_now = base;
        return base;
    }

    if (++st->stable >= NVFC_STABLE_POLLS) {
        int cap = cfg->sleep_max > base ? cfg->sleep_max : base;
        int doubled = st->sleep_now * 2;
        st->sleep_now = doubled > cap ? cap : doubled;
    }
    if (st->sleep_now < base) {
        st->sleep_now = base;
    }
    return st->sleep_now;
}

static void nvfc_join_values(const int *values, int count, char *buffer, size_t buffer_size) {
    if (!values || !buffer || buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';
    size_t offset = 0;
    for (int i = 0; i < count; ++i) {
        int written = snprintf(buffer + offset, buffer_size - offset, "%s%d", (i == 0 ? "" : ","), values[i]);
        if (written < 0 || (size_t)written >= buffer_size - offset) {
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
        "NVIDIA Fan Control %s started (GPU=%d, points=%d, temps=[%s], speeds=[%s], sleep_low=%d, "
        "sleep_high=%d, sleep_max=%d, high_temp_threshold=%d)",
        NVFC_VERSION, cfg->gpu_index, cfg->num_points, temps_buf, speeds_buf,
        cfg->sleep_low, cfg->sleep_high, cfg->sleep_max, cfg->high_temp_threshold);
    if (cfg->smooth_enabled) {
        nvfc_log_info("Smart Smooth: on (deadband=%d%%, down_hold=%ds, down_step=%d%%, emergency_temp=%d°C)",
                      cfg->smooth_deadband, cfg->smooth_down_hold, cfg->smooth_down_step,
                      cfg->smooth_emergency_temp);
    } else {
        nvfc_log_info("Smart Smooth: off (stepped curve, min_step=%d%%, min_interval=%ds)",
                      NVFC_LEGACY_MIN_STEP, NVFC_LEGACY_MIN_INTERVAL);
    }
}

static bool nvfc_should_log(const NvfcConfig *cfg, NvfcState *st, int temp, int target, time_t now) {
    bool log_it = false;
    if (st->log_time == 0 || st->log_temp == INT_MIN) {
        log_it = true;
    } else if (nvfc_abs(temp - st->log_temp) >= NVFC_LOG_TEMP_DELTA ||
               nvfc_abs(target - st->log_target) >= NVFC_LOG_SPEED_DELTA) {
        log_it = true;
    } else if (temp >= cfg->high_temp_threshold &&
               (int)difftime(now, st->log_time) >= NVFC_LOG_HOT_INTERVAL) {
        log_it = true;
    }
    if (log_it) {
        st->log_temp = temp;
        st->log_target = target;
        st->log_time = now;
    }
    return log_it;
}

/* Live status for `nvfc` / the TUI. Lives on tmpfs (/run), never touches the
 * GPU, and is only rewritten when something changed (plus a slow heartbeat). */
static const char *nvfc_status_path(void) {
    const char *env = getenv("NVFC_STATUS_PATH");
    return (env && *env) ? env : NVFC_DEFAULT_STATUS_PATH;
}

static void nvfc_status_update(const NvfcConfig *cfg, NvfcState *st, int temp, int target, int interval, time_t now) {
    if (st->status_disabled) {
        return;
    }
    if (temp == st->status_temp && st->fan == st->status_fan && interval == st->status_interval &&
        (int)difftime(now, st->status_time) < NVFC_STATUS_HEARTBEAT) {
        return;
    }
    const char *path = nvfc_status_path();
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        st->status_disabled = true;
        return;
    }
    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        st->status_disabled = true;  /* e.g. not running under systemd; stay quiet */
        return;
    }
    fprintf(fp, "temp=%d\nfan=%d\ntarget=%d\nsmooth=%d\ninterval=%d\nts=%ld\n", temp, st->fan, target,
            cfg->smooth_enabled, interval, (long)now);
    fclose(fp);
    chmod(tmp, 0644);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        st->status_disabled = true;
        return;
    }
    st->status_temp = temp;
    st->status_fan = st->fan;
    st->status_interval = interval;
    st->status_time = now;
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
            printf("Usage: %s [--config <path>] [--version]\n", argv[0]);
            exit(EXIT_SUCCESS);
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("nvidia-fan-controld %s\n", NVFC_VERSION);
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

static void nvfc_maybe_reload_config(const char *config_path, NvfcConfig *cfg, NvfcState *st) {
    if (!g_reload_config) {
        return;
    }
    g_reload_config = 0;
    NvfcConfig new_cfg = *cfg;
    nvfc_set_defaults(&new_cfg);
    if (nvfc_load_config(config_path, &new_cfg) == 0) {
        *cfg = new_cfg;
        nvfc_soft_reset_state(st);
        nvfc_log_info("Reloaded config from %s", config_path);
        nvfc_log_startup(cfg);
    } else {
        nvfc_log_warn("Failed to reload config from %s, keeping previous values", config_path);
    }
}

static bool nvfc_read_temperature(const NvfcNvml *api, NvfcDevice device, int *temp) {
    for (int attempt = 0; attempt < NVFC_TEMP_READ_RETRIES && g_keep_running; ++attempt) {
        unsigned int raw = 0;
        if (api->get_temp(device, NVFC_NVML_TEMPERATURE_GPU, &raw) == NVFC_NVML_SUCCESS) {
            *temp = (int)raw;
            return true;
        }
        if (attempt + 1 < NVFC_TEMP_READ_RETRIES) {
            sleep(1);
        }
    }
    return false;
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

    NvfcNvml api;
    if (nvfc_nvml_load(&api) != 0) {
        return EXIT_FAILURE;
    }
    if (api.init() != NVFC_NVML_SUCCESS) {
        nvfc_log_error("Failed to initialize NVML");
        nvfc_nvml_unload(&api);
        return EXIT_FAILURE;
    }

    NvfcDevice device;
    if (api.get_handle((unsigned int)cfg.gpu_index, &device) != NVFC_NVML_SUCCESS) {
        nvfc_log_error("Failed to get GPU handle for index %d", cfg.gpu_index);
        api.shutdown();
        nvfc_nvml_unload(&api);
        return EXIT_FAILURE;
    }

    unsigned int fan_count = 0;
    if (api.num_fans(device, &fan_count) != NVFC_NVML_SUCCESS || fan_count == 0) {
        nvfc_log_error("Failed to determine GPU fan count");
        api.shutdown();
        nvfc_nvml_unload(&api);
        return EXIT_FAILURE;
    }

    NvfcState st;
    nvfc_reset_state(&st);
    nvfc_log_startup(&cfg);

    while (g_keep_running) {
        nvfc_maybe_reload_config(config_path, &cfg, &st);

        int temperature = 0;
        if (!nvfc_read_temperature(&api, device, &temperature)) {
            if (g_keep_running) {
                nvfc_log_error("Failed to read GPU temperature");
            }
            break;
        }

        time_t now = time(NULL);
        int target = 0;
        int write_to = nvfc_decide(&cfg, &st, temperature, now, &target);

        if (write_to >= 0) {
            if (api.set_fan(device, 0, (unsigned int)write_to) == NVFC_NVML_SUCCESS) {
                st.fan = write_to;
                st.last_write = now;
                nvfc_log_info("Updated Fan Speed: %d%% (temp %d°C)", write_to, temperature);
            } else {
                nvfc_log_error("Failed to set fan speed to %d%%", write_to);
            }
        }

        if (nvfc_should_log(&cfg, &st, temperature, target, now)) {
            nvfc_log_info("Temp: %d°C -> target %d%%, fan %d%%", temperature, target, st.fan);
        }

        int interval = nvfc_next_sleep(&cfg, &st, temperature);
        nvfc_status_update(&cfg, &st, temperature, target, interval, now);
        sleep((unsigned int)interval);
    }

    api.set_default_fan(device, 0);
    api.shutdown();
    nvfc_nvml_unload(&api);
    if (!st.status_disabled) {
        unlink(nvfc_status_path());
    }
    nvfc_log_info("Fan control stopped, resetting to auto mode.");
    return EXIT_SUCCESS;
}
