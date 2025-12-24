#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/view.h>
#include <gui/canvas.h>

#include "scd4x.h"

#define TAG "scd41"

// Graph settings
#define GRAPH_WIDTH 90
#define GRAPH_HEIGHT 40
#define GRAPH_X 36
#define GRAPH_Y 22
#define HISTORY_SIZE 90  // Number of data points to store

typedef struct {
    ViewDispatcher* view_dispatcher;
    View* view;

    FuriTimer* timer;

    // latest readings
    uint16_t co2_ppm;
    int16_t temp_c_x100;
    int16_t rh_x100;

    // CO2 history for plotting
    uint16_t co2_history[HISTORY_SIZE];
    uint8_t history_index;
    uint8_t history_count;

    // status
    bool sensor_ok;
    char status[32];
} Scd41App;

// Global pointer for draw callback (View model approach is complex, this is simpler)
static Scd41App* g_app = NULL;

static void scd41_draw_callback(Canvas* canvas, void* ctx) {
    UNUSED(ctx);
    Scd41App* app = g_app;
    if(!app) return;
    
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "SCD41");

    if(!app->sensor_ok) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 24, app->status);
        canvas_draw_str(canvas, 2, 36, "Check wiring / I2C");
        return;
    }

    // Draw current readings on the left side
    char line[16];
    canvas_set_font(canvas, FontSecondary);
    
    snprintf(line, sizeof(line), "%u", (unsigned)app->co2_ppm);
    canvas_draw_str(canvas, 2, 24, "CO2:");
    canvas_draw_str(canvas, 2, 34, line);
    
    // Convert C to F: F = (C * 9/5) + 32
    int32_t temp_f_x100 = (app->temp_c_x100 * 9 / 5) + 3200;
    snprintf(line, sizeof(line), "%ld.%ldF",
             (long)(temp_f_x100 / 100), (long)labs(temp_f_x100 % 100) / 10);
    canvas_draw_str(canvas, 2, 48, line);
    
    snprintf(line, sizeof(line), "%ld%%RH",
             (long)(app->rh_x100 / 100));
    canvas_draw_str(canvas, 2, 58, line);

    // Draw graph area
    canvas_draw_frame(canvas, GRAPH_X - 1, GRAPH_Y - 1, GRAPH_WIDTH + 2, GRAPH_HEIGHT + 2);

    // Draw the CO2 graph
    if(app->history_count > 1) {
        // Find min and max for scaling
        uint16_t min_val = app->co2_history[0];
        uint16_t max_val = app->co2_history[0];
        for(uint8_t i = 0; i < app->history_count; i++) {
            if(app->co2_history[i] < min_val) min_val = app->co2_history[i];
            if(app->co2_history[i] > max_val) max_val = app->co2_history[i];
        }
        
        // Add some padding to the range
        if(min_val > 50) min_val -= 50;
        max_val += 50;
        
        // Ensure minimum range to avoid division issues
        if(max_val - min_val < 100) {
            max_val = min_val + 100;
        }

        // Draw scale labels
        char scale_str[8];
        canvas_set_font(canvas, FontSecondary);
        snprintf(scale_str, sizeof(scale_str), "%u", max_val);
        canvas_draw_str(canvas, GRAPH_X + 2, GRAPH_Y + 6, scale_str);
        snprintf(scale_str, sizeof(scale_str), "%u", min_val);
        canvas_draw_str(canvas, GRAPH_X + 2, GRAPH_Y + GRAPH_HEIGHT - 2, scale_str);

        // Plot the data points
        uint16_t range = max_val - min_val;
        uint8_t start_idx = (app->history_index + HISTORY_SIZE - app->history_count) % HISTORY_SIZE;
        
        int16_t prev_x = -1, prev_y = -1;
        for(uint8_t i = 0; i < app->history_count; i++) {
            uint8_t idx = (start_idx + i) % HISTORY_SIZE;
            uint16_t val = app->co2_history[idx];
            
            // Calculate x position (oldest on left, newest on right)
            int16_t x = GRAPH_X + (i * (GRAPH_WIDTH - 1)) / (app->history_count - 1);
            
            // Calculate y position (inverted - higher values at top)
            int16_t y = GRAPH_Y + GRAPH_HEIGHT - 1 - 
                       ((val - min_val) * (GRAPH_HEIGHT - 1)) / range;
            
            // Clamp y to graph bounds
            if(y < GRAPH_Y) y = GRAPH_Y;
            if(y > GRAPH_Y + GRAPH_HEIGHT - 1) y = GRAPH_Y + GRAPH_HEIGHT - 1;
            
            // Draw line from previous point
            if(prev_x >= 0) {
                canvas_draw_line(canvas, prev_x, prev_y, x, y);
            }
            
            prev_x = x;
            prev_y = y;
        }
    } else {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, GRAPH_X + 15, GRAPH_Y + 22, "Collecting...");
    }

    if(app->status[0]) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 40, 10, app->status);
    }
}

static void scd41_app_timer_cb(void* ctx) {
    Scd41App* app = ctx;

    // Simple polling approach: try read, update status; driver handles readiness.
    Scd4xReading reading = {0};
    Scd4xStatus st = scd4x_read_measurement(&reading);

    if(st == Scd4xStatusOk) {
        app->sensor_ok = true;
        app->co2_ppm = reading.co2_ppm;
        app->temp_c_x100 = reading.temp_c_x100;
        app->rh_x100 = reading.rh_x100;
        app->status[0] = 0;
        
        // Store in history for plotting
        app->co2_history[app->history_index] = app->co2_ppm;
        app->history_index = (app->history_index + 1) % HISTORY_SIZE;
        if(app->history_count < HISTORY_SIZE) {
            app->history_count++;
        }
    } else if(st == Scd4xStatusNotReady) {
        app->sensor_ok = true;
        snprintf(app->status, sizeof(app->status), "Waiting...");
    } else {
        app->sensor_ok = false;
        if(st == Scd4xStatusI2c) {
            snprintf(app->status, sizeof(app->status), "I2C error (no ACK?)");
        } else {
            snprintf(app->status, sizeof(app->status), "Sensor error (%d)", (int)st);
        }
    }

    // Trigger a redraw by updating the view
    if(app->view) {
        view_commit_model(app->view, false);
    }
}

static bool scd41_app_on_back(void* context) {
    Scd41App* app = context;
    view_dispatcher_stop(app->view_dispatcher);
    return true;
}

int32_t scd41_sensor_app(void* p) {
    UNUSED(p);

    Scd41App* app = malloc(sizeof(Scd41App));

    app->view_dispatcher = view_dispatcher_alloc();
    app->view = view_alloc();

    app->sensor_ok = false;
    app->history_index = 0;
    app->history_count = 0;
    snprintf(app->status, sizeof(app->status), "Starting...");

    // Set global pointer for draw callback
    g_app = app;

    // Set up view with draw callback
    view_set_draw_callback(app->view, scd41_draw_callback);

    // Attach to GUI
    Gui* gui = furi_record_open(RECORD_GUI);
    view_dispatcher_attach_to_gui(app->view_dispatcher, gui, ViewDispatcherTypeFullscreen);

    // Use view directly as the single view
    view_dispatcher_add_view(app->view_dispatcher, 0, app->view);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);

    // Event handling
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, scd41_app_on_back);

    // Init sensor
    Scd4xStatus init_st = scd4x_start_periodic_measurement();
    if(init_st != Scd4xStatusOk) {
        app->sensor_ok = false;
        if(init_st == Scd4xStatusI2c) {
            uint8_t addrs[8] = {0};
            size_t found = 0;
            if(scd4x_scan(addrs, sizeof(addrs), &found) == Scd4xStatusOk) {
                if(found == 0) {
                    snprintf(app->status, sizeof(app->status), "I2C: none (no pullups?)");
                } else {
                    // Show up to first 2 addresses to fit in 32 chars.
                    if(found == 1) {
                        snprintf(app->status, sizeof(app->status), "I2C found: 0x%02X (need 0x62)", addrs[0]);
                    } else {
                        snprintf(app->status, sizeof(app->status), "I2C: 0x%02X 0x%02X (need 0x62)", addrs[0], addrs[1]);
                    }
                }
            } else {
                snprintf(app->status, sizeof(app->status), "No I2C device at 0x62");
            }
        } else {
            snprintf(app->status, sizeof(app->status), "Init failed (%d)", (int)init_st);
        }
    } else {
        app->sensor_ok = true;
        snprintf(app->status, sizeof(app->status), "Warming up...");
    }

    // Poll timer (SCD41 updates ~5s)
    app->timer = furi_timer_alloc(scd41_app_timer_cb, FuriTimerTypePeriodic, app);
    furi_timer_start(app->timer, furi_kernel_get_tick_frequency() * 1);

    view_dispatcher_run(app->view_dispatcher);

    // Cleanup
    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);

    scd4x_stop_periodic_measurement();

    view_dispatcher_remove_view(app->view_dispatcher, 0);
    view_free(app->view);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_GUI);

    g_app = NULL;
    free(app);

    return 0;
}
