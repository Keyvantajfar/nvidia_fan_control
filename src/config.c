#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    cfg->high_temp_threshold = 42;
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

    *cfg = result;
    return 0;
}
