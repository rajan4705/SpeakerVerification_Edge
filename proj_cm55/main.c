/******************************************************************************
* File Name : main.c
*
* Description :
* Main entry point for Cortex-M55 core running ECAPA-TDNN speaker verification.
********************************************************************************/

#include "cybsp.h"
#include "retarget_io_init.h"
#include "SpeakerVerification.hpp"
#include "FreeRTOS.h"
#include "task.h"

#define APP_TASK_STACK_WORDS  (32768 / sizeof(StackType_t))
#define APP_TASK_PRIORITY     (configMAX_PRIORITIES - 2)

static StackType_t s_app_task_stack[APP_TASK_STACK_WORDS];
static StaticTask_t s_app_task_tcb;

static void AppTask(void* pvParameters)
{
    (void)pvParameters;

    /* Run the ECAPA-TDNN Speaker Verification pipeline (interactive CLI menu) */
    RunSpeakerVerificationApp();

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

int main(void)
{
    cy_rslt_t result;

    /* Initialize the device and board peripherals */
    result = cybsp_init();
    if (CY_RSLT_SUCCESS != result)
    {
        CY_ASSERT(0);
    }

    /* Enable global interrupts */
    __enable_irq();

    /* Initialize retarget-io middleware for debug UART */
    init_retarget_io();

    /* Create static task for Speaker Verification */
    xTaskCreateStatic(AppTask,
                      "SpeakerVerif",
                      APP_TASK_STACK_WORDS,
                      NULL,
                      APP_TASK_PRIORITY,
                      s_app_task_stack,
                      &s_app_task_tcb);

    /* Start the FreeRTOS Scheduler */
    vTaskStartScheduler();

    /* Should never reach here */
    for (;;)
    {
        Cy_SysPm_CpuEnterSleep(CY_SYSPM_WAIT_FOR_INTERRUPT);
    }

    return 0;
}
