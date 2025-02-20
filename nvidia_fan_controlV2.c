#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <nvml.h>
#include <stdbool.h>

// Fan curve parameters (temperature -> fan speed)
#define NUM_POINTS 6
int temperature_points[NUM_POINTS] = {0, 40, 55, 67, 75, 85};
int fan_speed_points[NUM_POINTS] = {25, 30, 45, 65, 78, 99};

// Sleep intervals (dynamically set based on temperature)
#define SLEEP_LOW 5 // E.G. 5 seconds when temp < 42°C
#define SLEEP_HIGH 2 // E.G. 2 seconds when temp >= 42°C

// Global flag for termination
volatile int keep_running = 1;

// Signal handler for graceful shutdown
void handle_signal(int sig) {
    keep_running = 0;
}

// Function to get GPU temperature
int get_temperature(nvmlDevice_t device) {
    unsigned int temperature;
    if (nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &temperature) != NVML_SUCCESS) {
        fprintf(stderr, "Failed to get GPU temperature!\n");
        fflush(stdout);
        return -1;
    }
    return (int)temperature;
}

// Function to set GPU fan speed only if it has changed
void set_fan_speed(nvmlDevice_t device, unsigned int fanIndex, int speed) {
    static int last_fan_speed = -1; // Store last set fan speed
    if (speed != last_fan_speed) {
        if (nvmlDeviceSetFanSpeed_v2(device, fanIndex, speed) != NVML_SUCCESS) {
            fprintf(stderr, "Failed to set fan speed!\n");
            fflush(stdout);
        } else {
            printf("Updated Fan Speed: %d%%\n", speed);
            fflush(stdout);
            last_fan_speed = speed;
        }
    } else {
        // printf("Fan speed unchanged: %d%%\n", speed);
    }
}

// Function to find appropriate fan speed for a given temperature
int get_fan_speed(int temperature) {
    for (int i = 0; i < NUM_POINTS - 1; i++) {
        if (temperature < temperature_points[i + 1]) {
            return fan_speed_points[i];
        }
    }
    return fan_speed_points[NUM_POINTS - 1];
}

int main() {
    // Register signal handlers
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Initialize NVML
    if (nvmlInit() != NVML_SUCCESS) {
        fprintf(stderr, "Failed to initialize NVML!\n");
        return 1;
    }

    // Get GPU handle
    nvmlDevice_t device;
    if (nvmlDeviceGetHandleByIndex(0, &device) != NVML_SUCCESS) {
        fprintf(stderr, "Failed to get GPU handle!\n");
        nvmlShutdown();
        return 1;
    }

    // Get fan count
    unsigned int fanCount;
    if (nvmlDeviceGetNumFans(device, &fanCount) != NVML_SUCCESS || fanCount < 1) {
        fprintf(stderr, "Failed to get GPU fan count!\n");
        nvmlShutdown();
        return 1;
    }

    printf("NVIDIA Fan Control started...\n");
    int high_temp_mode = false;

    while (keep_running) {
        int temperature = get_temperature(device);
        if (temperature < 0) {
            break;
        }

        int fan_speed = get_fan_speed(temperature);
        set_fan_speed(device, 0, fan_speed);

        // printf("Temp: %d°C -> Fan Speed: %d%%\n", temperature, fan_speed);
        
        int new_high_temp_mode = (temperature >= 42);
        if (new_high_temp_mode != high_temp_mode) {
            high_temp_mode = new_high_temp_mode;
            printf("Temp: %d°C -> Fan Speed: %d%%\n", temperature, fan_speed);
            fflush(stdout);
        }

        // Dynamic sleep time based on temperature
        int sleep_time = high_temp_mode ? SLEEP_HIGH : SLEEP_LOW; 
	    sleep(sleep_time);
        // sleep(high_temp_mode ? SLEEP_HIGH : SLEEP_LOW); 
    }

    // Reset fan control to auto
    nvmlDeviceSetDefaultFanSpeed_v2(device, 0);
    
    // Shutdown NVML
    nvmlShutdown();
    printf("Fan control stopped, resetting to auto mode.\n");

    return 0;
}
