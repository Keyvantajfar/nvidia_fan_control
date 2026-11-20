#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "log.h"

#define NVFC_TEMP_MIN 0
#define NVFC_TEMP_MAX 120
#define NVFC_SPEED_MIN 0
#define NVFC_SPEED_MAX 100
#define NVFC_SLEEP_MIN 1

static void nvfc_trim(char *str) {
    if (!str) {
        return;
    }
    size_t len = strlen(str);
    while (len > 0 && isspace((unsigned char)str[len - 1])) {
        str[--len] = '\0';
    }
    size_t start = 0;
    while (str[start] && isspace((unsigned char)str[start])) {
        start++;
    }
    if (start > 0) {
        memmove(str, str + start, len - start + 1);
    }
}

static int nvfc_parse_int(const char *value, int *out) {
    if (!value || !out) {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    long v = strtol(value, &end, 10);
    if (errno != 0 || end == value) {
        return -1;
    }
    while (*end && isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return -1;
    }
    *out = (int)v;
    return 0;
}

static int nvfc_parse_list(const char *line, int *out, size_t max_points, int min_value, int max_value, int *count_out) {
    if (!line || !out || !count_out) {
        return -1;
    }
    char buffer[512];
    strncpy(buffer, line, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    int count = 0;
    char *token = strtok(buffer, ",");
    while (token && count < (int)max_points) {
        nvfc_trim(token);
        if (*token == '\0') {
            token = strtok(NULL, ",");
            continue;
        }
        int value = 0;
        if (nvfc_parse_int(token, &value) != 0) {
            return -1;
        }
        if (value < min_value) {
            value = min_value;
        } else if (value > max_value) {
            value = max_value;
        }
        out[count++] = value;
        token = strtok(NULL, ",");
    }

    if (token != NULL) {
        // More values than allowed
        return -1;
    }

    *count_out = count;
    return 0;
}

/* Ramp-down levels. "slow" matches the 1.0.0 behaviour. */
static const struct {
    const char *name;
    int tau, hold, step, interval;
} k_ramp[] = {
    {"slow", 25, 15, 5, 15},
    {"balanced", 10, 8, 8, 5},
    {"quick", 4, 3, 15, 2},
};

const char *nvfc_ramp_name(int level) {
    return (level >= 0 && level < 3) ? k_ramp[level].name : "custom";
}

static void nvfc_apply_ramp(NvfcConfig *cfg, int level) {
    cfg->smooth_ramp = level;
    cfg->smooth_ramp_custom = 0;
    cfg->smooth_decay_tau = k_ramp[level].tau;
    cfg->smooth_down_hold = k_ramp[level].hold;
    cfg->smooth_down_step = k_ramp[level].step;
    cfg->smooth_down_interval = k_ramp[level].interval;
}

/* Built-in profiles. They are fixed (not stored in the config file), so a new
 * release can improve them. Keep in sync with tui/nvfc_config.py
 * (tests/test_profile_sync.sh checks this). */
static const struct {
    const char *id;
    const char *name;
    int num_points;
    int temps[NVFC_MAX_POINTS];
    int speeds[NVFC_MAX_POINTS];
    int ramp;
} k_builtin[] = {
    {"quiet", "Quiet", 7, {0, 45, 60, 72, 82, 92, 100}, {15, 20, 30, 45, 60, 75, 90}, NVFC_RAMP_QUICK},
    {"balanced", "Balanced", 6, {0, 40, 55, 67, 75, 85}, {25, 30, 45, 65, 78, 99}, NVFC_RAMP_BALANCED},
    {"performance", "Performance", 7, {0, 35, 45, 55, 65, 75, 85}, {30, 40, 55, 70, 85, 95, 100}, NVFC_RAMP_SLOW},
};
#define NVFC_NUM_BUILTIN 3
#define NVFC_BALANCED 1

static void nvfc_set_profile(NvfcConfig *cfg, const char *id, const char *name) {
    snprintf(cfg->profile, sizeof(cfg->profile), "%s", id);
    snprintf(cfg->profile_name, sizeof(cfg->profile_name), "%s", name);
}

static void nvfc_apply_builtin(NvfcConfig *cfg, int idx) {
    cfg->num_points = k_builtin[idx].num_points;
    for (int i = 0; i < cfg->num_points; ++i) {
        cfg->temps[i] = k_builtin[idx].temps[i];
        cfg->speeds[i] = k_builtin[idx].speeds[i];
    }
    cfg->smooth_enabled = 1;
    cfg->smooth_deadband = 3;
    nvfc_apply_ramp(cfg, k_builtin[idx].ramp);
    cfg->smooth_emergency_temp = 85;
    nvfc_set_profile(cfg, k_builtin[idx].id, k_builtin[idx].name);
}

void nvfc_set_defaults(NvfcConfig *cfg) {
    if (!cfg) {
        return;
    }
    nvfc_apply_builtin(cfg, NVFC_BALANCED);
    cfg->gpu_index = 0;
    cfg->sleep_low = 5;
    cfg->sleep_high = 2;
    cfg->sleep_max = 10;
    cfg->high_temp_threshold = 42;
}

static int nvfc_parse_bool(const char *value, int *out) {
    static const char *const yes[] = {"1", "on", "yes", "true", "enabled"};
    static const char *const no[] = {"0", "off", "no", "false", "disabled"};
    for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); ++i) {
        if (strcasecmp(value, yes[i]) == 0) {
            *out = 1;
            return 0;
        }
    }
    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); ++i) {
        if (strcasecmp(value, no[i]) == 0) {
            *out = 0;
            return 0;
        }
    }
    return -1;
}

static int nvfc_clamp_warn(const char *key, int value, int lo, int hi, const char *path) {
    int clamped = value < lo ? lo : (value > hi ? hi : value);
    if (clamped != value) {
        nvfc_log_warn("%s clamped to %d (got %d) in %s", key, clamped, value, path);
    }
    return clamped;
}

/* Everything a profile section can hold. -1 / empty means "not set in the file".
 * Schema 1 files keep this in [curve] + [smooth]; schema 2 in [custom1]/[custom2]. */
typedef struct {
    char name[NVFC_PROFILE_NAME_MAX];
    char temps[512];
    char speeds[512];
    int have_temps, have_speeds;
    int enabled, deadband, emergency;
    int ramp;
    int hold, step, interval;
    int present;
} NvfcProfileBuf;

static void nvfc_buf_init(NvfcProfileBuf *b) {
    memset(b, 0, sizeof(*b));
    b->enabled = b->deadband = b->emergency = b->ramp = -1;
    b->hold = b->step = b->interval = -1;
}

static void nvfc_buf_set(NvfcProfileBuf *b, const char *key, const char *value, const char *path) {
    int v = 0;
    if (strcmp(key, "name") == 0) {
        snprintf(b->name, sizeof(b->name), "%s", value);
        b->present = 1;
    } else if (strcmp(key, "temps") == 0) {
        snprintf(b->temps, sizeof(b->temps), "%s", value);
        b->have_temps = b->present = 1;
    } else if (strcmp(key, "speeds") == 0) {
        snprintf(b->speeds, sizeof(b->speeds), "%s", value);
        b->have_speeds = b->present = 1;
    } else if (strcmp(key, "enabled") == 0) {
        int flag = 0;
        if (nvfc_parse_bool(value, &flag) == 0) {
            b->enabled = flag;
            b->present = 1;
        } else {
            nvfc_log_warn("Invalid boolean for smooth.enabled in %s", path);
        }
    } else if (strcmp(key, "ramp_down") == 0) {
        int found = -1;
        for (int i = 0; i < 3; ++i) {
            if (strcasecmp(value, k_ramp[i].name) == 0) {
                found = i;
            }
        }
        if (found < 0) {
            nvfc_log_warn("ramp_down must be quick, balanced or slow (got '%s') in %s", value, path);
        } else {
            b->ramp = found;
            b->present = 1;
        }
    } else if (strcmp(key, "deadband") == 0 || strcmp(key, "down_hold") == 0 ||
               strcmp(key, "down_step") == 0 || strcmp(key, "down_interval") == 0 ||
               strcmp(key, "emergency_temp") == 0) {
        if (nvfc_parse_int(value, &v) != 0) {
            nvfc_log_warn("Invalid integer for %s in %s", key, path);
            return;
        }
        b->present = 1;
        if (strcmp(key, "deadband") == 0) {
            b->deadband = nvfc_clamp_warn("deadband", v, 1, 25, path);
        } else if (strcmp(key, "down_hold") == 0) {
            b->hold = nvfc_clamp_warn("down_hold", v, 0, 600, path);
        } else if (strcmp(key, "down_step") == 0) {
            b->step = nvfc_clamp_warn("down_step", v, 1, 100, path);
        } else if (strcmp(key, "down_interval") == 0) {
            b->interval = nvfc_clamp_warn("down_interval", v, 1, 600, path);
        } else {
            b->emergency = nvfc_clamp_warn("emergency_temp", v, NVFC_TEMP_MIN, NVFC_TEMP_MAX, path);
        }
    }
}

/* Lay a profile section over `result` (which already holds the fallback values). */
static void nvfc_apply_buf(NvfcConfig *result, const NvfcProfileBuf *b, const char *path, int warn_if_missing) {
    int temps[NVFC_MAX_POINTS];
    int speeds[NVFC_MAX_POINTS];
    int num_temps = 0;
    int num_speeds = 0;
    int curve_valid = 1;

    if (b->have_temps && b->have_speeds) {
        if (nvfc_parse_list(b->temps, temps, NVFC_MAX_POINTS, NVFC_TEMP_MIN, NVFC_TEMP_MAX, &num_temps) != 0) {
            curve_valid = 0;
        }
        if (nvfc_parse_list(b->speeds, speeds, NVFC_MAX_POINTS, NVFC_SPEED_MIN, NVFC_SPEED_MAX, &num_speeds) != 0) {
            curve_valid = 0;
        }
        if (curve_valid && num_temps != num_speeds) {
            curve_valid = 0;
        }
        if (curve_valid && (num_temps < 3 || num_temps > NVFC_MAX_POINTS)) {
            curve_valid = 0;
        }
        if (curve_valid) {
            for (int i = 1; i < num_temps; ++i) {
                if (temps[i] <= temps[i - 1]) {
                    curve_valid = 0;
                    break;
                }
            }
        }
    } else {
        curve_valid = 0;
    }

    if (!curve_valid) {
        /* A custom profile that simply has no curve yet just uses the fallback. */
        if (warn_if_missing || b->have_temps || b->have_speeds) {
            nvfc_log_warn("Invalid fan curve in %s; using defaults for curve points", path);
        }
    } else {
        result->num_points = num_temps;
        for (int i = 0; i < num_temps; ++i) {
            result->temps[i] = temps[i];
            result->speeds[i] = speeds[i];
        }
    }

    if (b->enabled >= 0) { result->smooth_enabled = b->enabled; }
    if (b->deadband >= 0) { result->smooth_deadband = b->deadband; }
    if (b->emergency >= 0) { result->smooth_emergency_temp = b->emergency; }

    /* Level first, then any explicit numbers on top of it. */
    if (b->ramp >= 0) {
        nvfc_apply_ramp(result, b->ramp);
    }
    if (b->hold >= 0) { result->smooth_down_hold = b->hold; result->smooth_ramp_custom = 1; }
    if (b->step >= 0) { result->smooth_down_step = b->step; result->smooth_ramp_custom = 1; }
    if (b->interval >= 0) { result->smooth_down_interval = b->interval; result->smooth_ramp_custom = 1; }
}

int nvfc_load_config(const char *path, NvfcConfig *cfg) {
    if (!path || !cfg) {
        return -1;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    NvfcConfig result = *cfg;  // start from existing values (typically defaults)
    int have_run_section = 0;

    NvfcProfileBuf legacy, custom[2];  /* legacy = schema 1 [curve]+[smooth] */
    nvfc_buf_init(&legacy);
    nvfc_buf_init(&custom[0]);
    nvfc_buf_init(&custom[1]);
    char active[32] = "";
    int have_active = 0;

    char section[32] = "";
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        nvfc_trim(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (!end) {
                fclose(fp);
                nvfc_log_warn("Invalid section header in %s", path);
                return -1;
            }
            size_t len = (size_t)(end - line - 1);
            if (len >= sizeof(section)) {
                len = sizeof(section) - 1;
            }
            strncpy(section, line + 1, len);
            section[len] = '\0';
            for (size_t i = 0; section[i]; ++i) {
                section[i] = (char)tolower((unsigned char)section[i]);
            }
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        nvfc_trim(key);
        nvfc_trim(value);
        for (char *p = key; *p; ++p) {
            *p = (char)tolower((unsigned char)*p);
        }

        if (strcmp(section, "curve") == 0 || strcmp(section, "smooth") == 0) {
            nvfc_buf_set(&legacy, key, value, path);
        } else if (strcmp(section, "custom1") == 0) {
            nvfc_buf_set(&custom[0], key, value, path);
        } else if (strcmp(section, "custom2") == 0) {
            nvfc_buf_set(&custom[1], key, value, path);
        } else if (strcmp(section, "profile") == 0) {
            if (strcmp(key, "active") == 0) {
                snprintf(active, sizeof(active), "%s", value);
                have_active = 1;
            }
        } else if (strcmp(section, "nvfc") == 0) {
            int ver = 0;
            if (strcmp(key, "config_version") == 0 && nvfc_parse_int(value, &ver) == 0 &&
                ver > NVFC_CONFIG_VERSION) {
                nvfc_log_warn("%s uses config schema %d, newer than this build (%d); unknown settings are ignored",
                              path, ver, NVFC_CONFIG_VERSION);
            }
        } else if (strcmp(section, "run") == 0) {
            have_run_section = 1;
            int parsed_value = 0;
            if (nvfc_parse_int(value, &parsed_value) != 0) {
                nvfc_log_warn("Invalid integer for %s in %s", key, path);
                continue;
            }
            if (strcmp(key, "gpu_index") == 0) {
                if (parsed_value < 0) {
                    nvfc_log_warn("gpu_index must be >= 0 (got %d) in %s", parsed_value, path);
                    parsed_value = 0;
                }
                result.gpu_index = parsed_value;
            } else if (strcmp(key, "sleep_low") == 0) {
                if (parsed_value < NVFC_SLEEP_MIN) {
                    nvfc_log_warn("sleep_low clamped to %d (got %d) in %s", NVFC_SLEEP_MIN, parsed_value, path);
                    parsed_value = NVFC_SLEEP_MIN;
                }
                result.sleep_low = parsed_value;
            } else if (strcmp(key, "sleep_high") == 0) {
                if (parsed_value < NVFC_SLEEP_MIN) {
                    nvfc_log_warn("sleep_high clamped to %d (got %d) in %s", NVFC_SLEEP_MIN, parsed_value, path);
                    parsed_value = NVFC_SLEEP_MIN;
                }
                result.sleep_high = parsed_value;
            } else if (strcmp(key, "sleep_max") == 0) {
                result.sleep_max = nvfc_clamp_warn("sleep_max", parsed_value, NVFC_SLEEP_MIN, 600, path);
            } else if (strcmp(key, "high_temp_threshold") == 0) {
                if (parsed_value < NVFC_TEMP_MIN || parsed_value > NVFC_TEMP_MAX) {
                    int clamped = parsed_value < NVFC_TEMP_MIN ? NVFC_TEMP_MIN : NVFC_TEMP_MAX;
                    nvfc_log_warn("high_temp_threshold clamped to %d (got %d) in %s", clamped, parsed_value, path);
                    parsed_value = clamped;
                }
                result.high_temp_threshold = parsed_value;
            }
        }
    }

    fclose(fp);

    if (!have_run_section) {
        nvfc_log_warn("[run] section missing in %s; using defaults for runtime options", path);
    }

    /* Pick the profile. No [profile] section: a schema 1 file runs as "custom1"
     * (exactly what it did before), anything else as "balanced". */
    const char *want = have_active ? active : (legacy.present ? "legacy" : "balanced");
    int builtin = -1;
    for (int i = 0; i < NVFC_NUM_BUILTIN; ++i) {
        if (strcasecmp(want, k_builtin[i].id) == 0) {
            builtin = i;
        }
    }

    if (builtin >= 0) {
        nvfc_apply_builtin(&result, builtin);
    } else if (strcmp(want, "legacy") == 0) {
        nvfc_apply_buf(&result, &legacy, path, 1);
        nvfc_set_profile(&result, "custom1", "Previous config");
    } else if (strcasecmp(want, "custom1") == 0 || strcasecmp(want, "custom2") == 0) {
        int slot = want[6] == '2' ? 1 : 0;
        nvfc_apply_builtin(&result, NVFC_BALANCED);  /* fallback for anything the section omits */
        nvfc_apply_buf(&result, &custom[slot], path, 0);
        char fallback[NVFC_PROFILE_NAME_MAX];
        snprintf(fallback, sizeof(fallback), "Custom %d", slot + 1);
        nvfc_set_profile(&result, slot ? "custom2" : "custom1", custom[slot].name[0] ? custom[slot].name : fallback);
    } else {
        nvfc_log_warn("Unknown profile '%s' in %s; using balanced", want, path);
        nvfc_apply_builtin(&result, NVFC_BALANCED);
    }

    if (result.sleep_max < result.sleep_low) {
        result.sleep_max = result.sleep_low;
    }

    *cfg = result;
    return 0;
}
