/*******************************************************************************
* File Name: TouchController.c
* Description: Capacitive touch driver implementation using FT5406 on PSOC Edge.
********************************************************************************/

#include "TouchController.h"
#include "cy_pdl.h"
#include "cycfg_peripherals.h"
#include "mtb_ctp_ft5406.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

#define TOUCH_TASK_STACK_SIZE   (1024 / sizeof(StackType_t))
#define TOUCH_TASK_PRIORITY     (tskIDLE_PRIORITY + 2)
#define TOUCH_POLL_INTERVAL_MS  20

static mtb_ctp_ft5406_config_t s_ft_config;
static TaskHandle_t s_touch_task_handle = NULL;

static volatile int  s_cur_x = -1;
static volatile int  s_cur_y = -1;
static volatile bool s_cur_down = false;

/* Tap event buffer (single-tap latch with debounce) */
static volatile int  s_tap_x = -1;
static volatile int  s_tap_y = -1;
static volatile bool s_tap_pending = false;
static uint32_t      s_last_tap_time = 0;

static void TouchPollTask(void* pvParameters)
{
    (void)pvParameters;
    bool prev_down = false;

    for (;;)
    {
        mtb_ctp_touch_event_t ev = MTB_CTP_TOUCH_RESERVED;
        int raw_x = 0;
        int raw_y = 0;

        cy_en_scb_i2c_status_t res = mtb_ctp_ft5406_get_single_touch(&ev, &raw_x, &raw_y);

        if (res == CY_SCB_I2C_SUCCESS &&
            (ev == MTB_CTP_TOUCH_DOWN || ev == MTB_CTP_TOUCH_CONTACT))
        {
            /* Coordinate conversion for Waveshare 4.3" 800x480 */
            int sx = 800 - raw_x;
            int sy = 480 - raw_y;

            if (sx < 0) sx = 0;
            if (sx >= 800) sx = 799;
            if (sy < 0) sy = 0;
            if (sy >= 480) sy = 479;

            s_cur_x = sx;
            s_cur_y = sy;
            s_cur_down = true;

            uint32_t now = (uint32_t)xTaskGetTickCount();

            /* Leading edge tap trigger with 200 ms debounce */
            if (!prev_down && (now - s_last_tap_time >= pdMS_TO_TICKS(200)))
            {
                s_tap_x = sx;
                s_tap_y = sy;
                s_tap_pending = true;
                s_last_tap_time = now;
            }

            prev_down = true;
        }
        else
        {
            s_cur_down = false;
            prev_down = false;
        }

        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_INTERVAL_MS));
    }
}

bool TouchController_Init(struct cy_stc_scb_i2c_context* i2c_context)
{
    if (!i2c_context) return false;

    s_ft_config.i2c_base = CYBSP_I2C_CONTROLLER_HW;
    s_ft_config.i2c_context = i2c_context;

    cy_en_scb_i2c_status_t status = mtb_ctp_ft5406_init(&s_ft_config);
    if (status != CY_SCB_I2C_SUCCESS)
    {
        printf("TouchController: FT5406 init failed with status 0x%08X\r\n", (unsigned int)status);
        return false;
    }

    printf("TouchController: FT5406 Capacitive Touch initialized at I2C address 0x38.\r\n");

    if (s_touch_task_handle == NULL)
    {
        BaseType_t task_res = xTaskCreate(TouchPollTask,
                                          "TouchPoll",
                                          TOUCH_TASK_STACK_SIZE,
                                          NULL,
                                          TOUCH_TASK_PRIORITY,
                                          &s_touch_task_handle);
        if (task_res != pdPASS)
        {
            printf("TouchController: Failed to spawn TouchPoll task!\r\n");
            return false;
        }
    }

    return true;
}

bool TouchController_GetLiveTouch(int* x, int* y, bool* is_down)
{
    if (x) *x = s_cur_x;
    if (y) *y = s_cur_y;
    if (is_down) *is_down = s_cur_down;
    return s_cur_down;
}

bool TouchController_GetTap(int* tap_x, int* tap_y)
{
    if (s_tap_pending)
    {
        if (tap_x) *tap_x = s_tap_x;
        if (tap_y) *tap_y = s_tap_y;
        s_tap_pending = false;
        return true;
    }
    return false;
}
