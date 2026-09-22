#include "PdmMic.hpp"
#include "cy_pdl.h"
#include "cybsp.h"
#include "cy_pdm_pcm_v2.h"
#include "cycfg_peripherals.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

#define PDM_LEFT_CH            (2u)
#define PDM_RIGHT_CH           (3u)
#define PDM_HW_FIFO_SIZE       (64u)
#define PDM_FIFO_TRIGGER       (32u)
#define PDM_SOFTWARE_GAIN_DB   (30)   // 30 dB digital mic gain

#define PDM_RING_SIZE          (64000u) // 4.0 seconds @ 16 kHz

#define SOCMEM_BSS __attribute__((section(".cy_socmem_bss"), aligned(16)))

static int16_t s_pdm_ring[PDM_RING_SIZE] SOCMEM_BSS;
static volatile uint32_t s_head = 0;
static volatile uint32_t s_tail = 0;
static volatile bool s_is_running = false;

/* PDM interrupt configuration */
static const cy_stc_sysint_t s_pdm_irq_cfg = {
    .intrSrc = (IRQn_Type)CYBSP_PDM_CHANNEL_3_IRQ,
    .intrPriority = 6
};

static void PdmInterruptHandler(void)
{
    uint32_t int_stat = Cy_PDM_PCM_Channel_GetInterruptStatusMasked(PDM0, PDM_RIGHT_CH);

    if (CY_PDM_PCM_INTR_RX_TRIGGER & int_stat)
    {
        for (uint8_t i = 0; i < PDM_FIFO_TRIGGER; ++i)
        {
            // Read both channels to keep FIFOs synchronized
            int32_t left_raw = (int32_t)Cy_PDM_PCM_Channel_ReadFifo(PDM0, PDM_LEFT_CH);
            int32_t right_raw = (int32_t)Cy_PDM_PCM_Channel_ReadFifo(PDM0, PDM_RIGHT_CH);
            (void)left_raw;

            int32_t scaled = right_raw;
            (void)Cy_PDM_PCM_ApplyPCM_Gain(&scaled, PDM_SOFTWARE_GAIN_DB, CY_PDM_PCM_16BIT, &scaled);

            uint32_t next_h = s_head + 1;
            if (next_h >= PDM_RING_SIZE) {
                next_h = 0;
            }

            if (next_h != s_tail) {
                s_pdm_ring[s_head] = (int16_t)scaled;
                s_head = next_h;
            } else {
                // Ring buffer overflow: advance tail to drop oldest sample
                uint32_t next_t = s_tail + 1;
                if (next_t >= PDM_RING_SIZE) {
                    next_t = 0;
                }
                s_tail = next_t;
                s_pdm_ring[s_head] = (int16_t)scaled;
                s_head = next_h;
            }
        }
        Cy_PDM_PCM_Channel_ClearInterrupt(PDM0, PDM_RIGHT_CH, CY_PDM_PCM_INTR_RX_TRIGGER);
    }

    if ((CY_PDM_PCM_INTR_RX_FIR_OVERFLOW | CY_PDM_PCM_INTR_RX_OVERFLOW |
         CY_PDM_PCM_INTR_RX_IF_OVERFLOW | CY_PDM_PCM_INTR_RX_UNDERFLOW) & int_stat)
    {
        Cy_PDM_PCM_Channel_ClearInterrupt(PDM0, PDM_RIGHT_CH, CY_PDM_PCM_INTR_MASK);
    }
}

cy_rslt_t PdmMic_Init(void)
{
    // 1. Initialize PDM top-level block
    cy_en_pdm_pcm_status_t status = Cy_PDM_PCM_Init(PDM0, &CYBSP_PDM_config);
    if (CY_PDM_PCM_SUCCESS != status) {
        printf("[PDM] Cy_PDM_PCM_Init failed: %d\r\n", (int)status);
        return CY_RSLT_TYPE_ERROR;
    }

    // 2. Enable channels
    Cy_PDM_PCM_Channel_Enable(PDM0, PDM_LEFT_CH);
    Cy_PDM_PCM_Channel_Enable(PDM0, PDM_RIGHT_CH);

    // 3. Configure channel parameters
    Cy_PDM_PCM_Channel_Init(PDM0, &channel_2_config, PDM_LEFT_CH);
    Cy_PDM_PCM_Channel_Init(PDM0, &channel_3_config, PDM_RIGHT_CH);

    // 4. Set hardware decimation filter gains
    Cy_PDM_PCM_SetGain(PDM0, PDM_LEFT_CH, CY_PDM_PCM_SEL_GAIN_23DB);
    Cy_PDM_PCM_SetGain(PDM0, PDM_RIGHT_CH, CY_PDM_PCM_SEL_GAIN_23DB);

    // 5. Configure interrupts for Right channel
    Cy_PDM_PCM_Channel_ClearInterrupt(PDM0, PDM_RIGHT_CH, CY_PDM_PCM_INTR_MASK);
    Cy_PDM_PCM_Channel_SetInterruptMask(PDM0, PDM_RIGHT_CH, CY_PDM_PCM_INTR_MASK);

    // 6. Register IRQ handler
    if (CY_SYSINT_SUCCESS != Cy_SysInt_Init(&s_pdm_irq_cfg, &PdmInterruptHandler)) {
        printf("[PDM] Cy_SysInt_Init failed!\r\n");
        return CY_RSLT_TYPE_ERROR;
    }

    PdmMic_Clear();
    return CY_RSLT_SUCCESS;
}

void PdmMic_Start(void)
{
    if (s_is_running) return;

    PdmMic_Clear();

    NVIC_ClearPendingIRQ(s_pdm_irq_cfg.intrSrc);
    NVIC_EnableIRQ(s_pdm_irq_cfg.intrSrc);

    Cy_PDM_PCM_Activate_Channel(PDM0, PDM_LEFT_CH);
    Cy_PDM_PCM_Activate_Channel(PDM0, PDM_RIGHT_CH);

    s_is_running = true;
}

void PdmMic_Stop(void)
{
    if (!s_is_running) return;

    NVIC_DisableIRQ(s_pdm_irq_cfg.intrSrc);

    Cy_PDM_PCM_DeActivate_Channel(PDM0, PDM_LEFT_CH);
    Cy_PDM_PCM_DeActivate_Channel(PDM0, PDM_RIGHT_CH);

    s_is_running = false;
}

bool PdmMic_IsRunning(void)
{
    return s_is_running;
}

uint32_t PdmMic_GetAvailable(void)
{
    uint32_t h = s_head;
    uint32_t t = s_tail;
    if (h >= t) {
        return h - t;
    }
    return PDM_RING_SIZE - (t - h);
}

uint32_t PdmMic_Read(int16_t* out, uint32_t count)
{
    uint32_t avail = PdmMic_GetAvailable();
    if (count > avail) {
        count = avail;
    }

    for (uint32_t i = 0; i < count; ++i) {
        out[i] = s_pdm_ring[s_tail];
        uint32_t next_t = s_tail + 1;
        if (next_t >= PDM_RING_SIZE) {
            next_t = 0;
        }
        s_tail = next_t;
    }
    return count;
}

void PdmMic_Clear(void)
{
    s_head = 0;
    s_tail = 0;
}

float PdmMic_CalculateFrameRms(const int16_t* samples, uint32_t count)
{
    if (count == 0) return 0.0f;
    uint64_t sum_sq = 0;
    for (uint32_t i = 0; i < count; ++i) {
        int32_t val = samples[i];
        sum_sq += (uint64_t)(val * val);
    }
    return sqrtf((float)sum_sq / (float)count);
}
