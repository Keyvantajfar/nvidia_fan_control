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

void nvfc_set_defaults(NvfcConfig *cfg) {
    if (!cfg) {
        return;
    }
    int default_temps[] = {0, 40, 55, 67, 75, 85};
    int default_speeds[] = {25, 30, 45, 65, 78, 99};
    cfg->num_points = (int)(sizeof(default_temps) / sizeof(default_temps[0]));
    for (int i = 0; i < cfg->num_points; ++i) {
        cfg->temps[i] = default_temps[i];
        cfg->speeds[i] = default_speeds[i];
    }
    cfg->gpu_index = 0;
    cfg->sleep_low = 5;
    cfg->sleep_high = 2;
    cfg->sleep_max = 10;
    cfg->high_temp_threshold = 42;
    cfg->smooth_enabled = 1;
    cfg->smooth_deadband = 3;
    nvfc_apply_ramp(cfg, NVFC_RAMP_BALANCED);
    cfg->smooth_emergency_temp = 85;
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

int nvfc_load_config(const char *path, NvfcConfig *cfg) {
    if (!path || !cfg) {
        return -1;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    NvfcConfig result = *cfg;  // start from existing values (typically defaults)
    char temps_line[512] = {0};
    char speeds_line[512] = {0};
    int have_temps = 0;
    int have_speeds = 0;
    int have_run_section = 0;

    int ramp_level = -1;  /* set when ramp_down appears in the file */
    int ov_hold = -1, ov_step = -1, ov_interval = -1;  /* explicit numeric overrides */

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

        if (strcmp(section, "curve") == 0) {
            if (strcmp(key, "temps") == 0) {
                strncpy(temps_line, value, sizeof(temps_line) - 1);
                temps_line[sizeof(temps_line) - 1] = '\0';
                have_temps = 1;
            } else if (strcmp(key, "speeds") == 0) {
                strncpy(speeds_line, value, sizeof(speeds_line) - 1);
                speeds_line[sizeof(speeds_line) - 1] = '\0';
                have_speeds = 1;
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
        } else if (strcmp(section, "smooth") == 0) {
            if (strcmp(key, "enabled") == 0) {
                int flag = 0;
                if (nvfc_parse_bool(value, &flag) == 0) {
                    result.smooth_enabled = flag;
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
                    ramp_level = found;
                }
            } else {
                int v = 0;
                if (nvfc_parse_int(value, &v) != 0) {
                    nvfc_log_warn("Invalid integer for %s in %s", key, path);
                } else if (strcmp(key, "deadband") == 0) {
                    result.smooth_deadband = nvfc_clamp_warn("deadband", v, 1, 25, path);
                } else if (strcmp(key, "down_hold") == 0) {
                    ov_hold = nvfc_clamp_warn("down_hold", v, 0, 600, path);
                } else if (strcmp(key, "down_step") == 0) {
                    ov_step = nvfc_clamp_warn("down_step", v, 1, 100, path);
                } else if (strcmp(key, "down_interval") == 0) {
                    ov_interval = nvfc_clamp_warn("down_interval", v, 1, 600, path);
                } else if (strcmp(key, "emergency_temp") == 0) {
                    result.smooth_emergency_temp = nvfc_clamp_warn("emergency_temp", v, NVFC_TEMP_MIN, NVFC_TEMP_MAX, path);
                }
            }
        }
    }

    fclose(fp);

    int temps[NVFC_MAX_POINTS];
    int speeds[NVFC_MAX_POINTS];
    int num_temps = 0;
    int num_speeds = 0;
    int curve_valid = 1;

    if (have_temps && have_speeds) {
        if (nvfc_parse_list(temps_line, temps, NVFC_MAX_POINTS, NVFC_TEMP_MIN, NVFC_TEMP_MAX, &num_temps) != 0) {
            curve_valid = 0;
        }
        if (nvfc_parse_list(speeds_line, speeds, NVFC_MAX_POINTS, NVFC_SPEED_MIN, NVFC_SPEED_MAX, &num_speeds) != 0) {
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
        nvfc_log_warn("Invalid fan curve in %s; using defaults for curve points", path);
        // Retain existing temps/speeds from result (defaults) but ensure num_points is accurate
        result.num_points = cfg->num_points;
    } else {
        result.num_points = num_temps;
        for (int i = 0; i < num_temps; ++i) {
            result.temps[i] = temps[i];
            result.speeds[i] = speeds[i];
        }
    }

    if (!have_run_section) {
        nvfc_log_warn("[run] section missing in %s; using defaults for runtime options", path);
    }

    /* Level first, then any explicit numbers on top of it. */
    if (ramp_level >= 0) {
        nvfc_apply_ramp(&result, ramp_level);
    }
    if (ov_hold >= 0) { result.smooth_down_hold = ov_hold; result.smooth_ramp_custom = 1; }
    if (ov_step >= 0) { result.smooth_down_step = ov_step; result.smooth_ramp_custom = 1; }
    if (ov_interval >= 0) { result.smooth_down_interval = ov_interval; result.smooth_ramp_custom = 1; }

    if (result.sleep_max < result.sleep_low) {
        result.sleep_max = result.sleep_low;
    }

    *cfg = result;
    return 0;
}
