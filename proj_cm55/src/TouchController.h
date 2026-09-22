/*******************************************************************************
* File Name: TouchController.h
* Description: Capacitive touch input driver for Waveshare 4.3" display on PSOC Edge.
********************************************************************************/

#ifndef TOUCH_CONTROLLER_H
#define TOUCH_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

/* Forward-declare the I2C context struct (defined in cy_scb_i2c.h).
 * We avoid including cy_pdl.h here because it triggers C++ linkage
 * errors from startup_edge.h when included in .cpp translation units. */
struct cy_stc_scb_i2c_context;

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the FT5406 capacitive touch controller and background polling task */
bool TouchController_Init(struct cy_stc_scb_i2c_context* i2c_context);

/* Get current live touch state */
bool TouchController_GetLiveTouch(int* x, int* y, bool* is_down);

/* Consume a single tap event if available (debounced edge-detection) */
bool TouchController_GetTap(int* tap_x, int* tap_y);

/* Helper to check if point (px, py) is inside rectangle (rx, ry, rw, rh) */
static inline bool Touch_HitTest(int px, int py, int rx, int ry, int rw, int rh) {
    return (px >= rx && px < (rx + rw) && py >= ry && py < (ry + rh));
}

#ifdef __cplusplus
}
#endif

#endif /* TOUCH_CONTROLLER_H */
