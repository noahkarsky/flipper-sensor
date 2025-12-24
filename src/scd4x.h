#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    Scd4xStatusOk = 0,
    Scd4xStatusError = -1,
    Scd4xStatusI2c = -2,
    Scd4xStatusCrc = -3,
    Scd4xStatusNotReady = -4,
} Scd4xStatus;

typedef struct {
    uint16_t co2_ppm;
    int16_t temp_c_x100;
    int16_t rh_x100;
} Scd4xReading;

Scd4xStatus scd4x_start_periodic_measurement(void);
Scd4xStatus scd4x_stop_periodic_measurement(void);
Scd4xStatus scd4x_read_measurement(Scd4xReading* out);

// Scans the external I2C bus for any responding devices.
// Stores up to addrs_cap 7-bit addresses into addrs and returns the count.
Scd4xStatus scd4x_scan(uint8_t* addrs, size_t addrs_cap, size_t* out_count);

#ifdef __cplusplus
}
#endif
