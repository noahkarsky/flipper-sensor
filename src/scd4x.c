#include "scd4x.h"

#include <furi.h>
#include <furi_hal.h>

// Sensirion SCD4x I2C address
// Flipper HAL expects 8-bit address (7-bit address << 1)
#define SCD4X_ADDR (0x62 << 1)

// Commands (big-endian)
#define CMD_START_PERIODIC_MEASUREMENT (0x21B1)
#define CMD_STOP_PERIODIC_MEASUREMENT (0x3F86)
#define CMD_READ_MEASUREMENT (0xEC05)
#define CMD_GET_DATA_READY_STATUS (0xE4B8)

#define TAG "scd4x"

Scd4xStatus scd4x_scan(uint8_t* addrs, size_t addrs_cap, size_t* out_count) {
    if(out_count) *out_count = 0;
    if(!addrs || addrs_cap == 0 || !out_count) return Scd4xStatusError;

    size_t found = 0;

    furi_hal_i2c_acquire(&furi_hal_i2c_handle_external);
    for(uint8_t addr = 0x03; addr <= 0x77; addr++) {
        if(furi_hal_i2c_is_device_ready(&furi_hal_i2c_handle_external, addr, 20)) {
            if(found < addrs_cap) {
                addrs[found] = addr;
            }
            found++;
        }
        // Small delay to be gentle with the bus.
        furi_delay_ms(1);
    }
    furi_hal_i2c_release(&furi_hal_i2c_handle_external);

    size_t stored = (found > addrs_cap) ? addrs_cap : found;
    for(size_t i = 0; i < stored; i++) {
        FURI_LOG_I(TAG, "I2C device at 0x%02X", addrs[i]);
    }
    if(found == 0) {
        FURI_LOG_W(TAG, "No I2C devices responded on external bus");
    }

    *out_count = found;
    return Scd4xStatusOk;
}

static uint8_t scd4x_crc8(const uint8_t* data, size_t len) {
    // CRC-8 with polynomial 0x31, init 0xFF
    uint8_t crc = 0xFF;
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(uint8_t b = 0; b < 8; b++) {
            if(crc & 0x80) {
                crc = (uint8_t)((crc << 1) ^ 0x31);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static Scd4xStatus scd4x_write_cmd(uint16_t cmd) {
    uint8_t buf[2] = {(uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF)};

    // External I2C bus
    furi_hal_i2c_acquire(&furi_hal_i2c_handle_external);
    bool ok = furi_hal_i2c_tx(&furi_hal_i2c_handle_external, SCD4X_ADDR, buf, sizeof(buf), 50);
    furi_hal_i2c_release(&furi_hal_i2c_handle_external);

    return ok ? Scd4xStatusOk : Scd4xStatusI2c;
}

static Scd4xStatus scd4x_read_words(uint16_t cmd, uint8_t* rx, size_t rx_len) {
    uint8_t tx[2] = {(uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF)};

    furi_hal_i2c_acquire(&furi_hal_i2c_handle_external);

    bool ok_tx = furi_hal_i2c_tx(&furi_hal_i2c_handle_external, SCD4X_ADDR, tx, sizeof(tx), 50);
    bool ok_rx = false;
    if(ok_tx) {
        // Give the sensor a moment to prepare the response.
        furi_delay_ms(2);
        ok_rx = furi_hal_i2c_rx(&furi_hal_i2c_handle_external, SCD4X_ADDR, rx, rx_len, 50);
    }

    furi_hal_i2c_release(&furi_hal_i2c_handle_external);

    if(!ok_tx || !ok_rx) return Scd4xStatusI2c;
    return Scd4xStatusOk;
}

static Scd4xStatus scd4x_get_data_ready(bool* ready) {
    // Returns 2 bytes + CRC
    uint8_t rx[3] = {0};
    Scd4xStatus st = scd4x_read_words(CMD_GET_DATA_READY_STATUS, rx, sizeof(rx));
    if(st != Scd4xStatusOk) return st;

    if(scd4x_crc8(rx, 2) != rx[2]) return Scd4xStatusCrc;

    uint16_t status = (uint16_t)((rx[0] << 8) | rx[1]);

    // Datasheet: if any of bits 0..10 are non-zero, data is ready
    *ready = (status & 0x07FF) != 0;
    return Scd4xStatusOk;
}

Scd4xStatus scd4x_start_periodic_measurement(void) {
    // Datasheet recommends waiting a little after power-up before first commands.
    furi_delay_ms(30);

    furi_hal_i2c_acquire(&furi_hal_i2c_handle_external);
    bool present = furi_hal_i2c_is_device_ready(&furi_hal_i2c_handle_external, SCD4X_ADDR, 50);
    furi_hal_i2c_release(&furi_hal_i2c_handle_external);
    if(!present) return Scd4xStatusI2c;

    return scd4x_write_cmd(CMD_START_PERIODIC_MEASUREMENT);
}

Scd4xStatus scd4x_stop_periodic_measurement(void) {
    Scd4xStatus st = scd4x_write_cmd(CMD_STOP_PERIODIC_MEASUREMENT);
    // Datasheet: allow time after stop
    furi_delay_ms(500);
    return st;
}

Scd4xStatus scd4x_read_measurement(Scd4xReading* out) {
    if(!out) return Scd4xStatusError;

    bool ready = false;
    Scd4xStatus st_ready = scd4x_get_data_ready(&ready);
    if(st_ready != Scd4xStatusOk) return st_ready;
    if(!ready) return Scd4xStatusNotReady;

    // 3 words: CO2, Temp, RH; each word has CRC: 3*(2+1) = 9 bytes
    uint8_t rx[9] = {0};
    Scd4xStatus st = scd4x_read_words(CMD_READ_MEASUREMENT, rx, sizeof(rx));
    if(st != Scd4xStatusOk) return st;

    for(size_t i = 0; i < 9; i += 3) {
        if(scd4x_crc8(&rx[i], 2) != rx[i + 2]) return Scd4xStatusCrc;
    }

    uint16_t co2_raw = (uint16_t)((rx[0] << 8) | rx[1]);
    uint16_t t_raw = (uint16_t)((rx[3] << 8) | rx[4]);
    uint16_t rh_raw = (uint16_t)((rx[6] << 8) | rx[7]);

    // Datasheet conversion:
    // T [C] = -45 + 175 * (t_raw / 65536)
    // RH [%] = 100 * (rh_raw / 65536)
    // We return fixed-point x100
    int32_t temp_c_x100 = -4500 + (int32_t)((17500LL * (int64_t)t_raw) / 65536LL);
    int32_t rh_x100 = (int32_t)((10000LL * (int64_t)rh_raw) / 65536LL);

    out->co2_ppm = co2_raw;
    out->temp_c_x100 = (int16_t)temp_c_x100;
    out->rh_x100 = (int16_t)rh_x100;

    return Scd4xStatusOk;
}
