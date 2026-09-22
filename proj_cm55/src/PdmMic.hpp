#ifndef PDM_MIC_HPP_
#define PDM_MIC_HPP_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "cy_result.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PDM_SAMPLE_RATE_HZ     16000
#define PDM_FRAME_LEN_10MS     160
#define PDM_FRAME_LEN_25MS     400
#define PDM_INFERENCE_SAMPLES  48000 // 3.0 seconds @ 16 kHz

/**
 * @brief Initializes the on-board PDM microphone hardware (Channels 2 and 3).
 * @return CY_RSLT_SUCCESS on success.
 */
cy_rslt_t PdmMic_Init(void);

/**
 * @brief Starts streaming audio from the PDM microphone into the ring buffer.
 */
void PdmMic_Start(void);

/**
 * @brief Stops PDM microphone capture and disables interrupts.
 */
void PdmMic_Stop(void);

/**
 * @brief Checks if PDM capture is currently active.
 */
bool PdmMic_IsRunning(void);

/**
 * @brief Returns the number of PCM samples currently buffered in the ring buffer.
 */
uint32_t PdmMic_GetAvailable(void);

/**
 * @brief Reads up to @p count 16-bit PCM samples from the ring buffer.
 * @param out Destination buffer.
 * @param count Maximum samples to read.
 * @return Number of samples actually read.
 */
uint32_t PdmMic_Read(int16_t* out, uint32_t count);

/**
 * @brief Discards all unread samples in the ring buffer.
 */
void PdmMic_Clear(void);

/**
 * @brief Calculates RMS energy of a frame of PCM samples.
 */
float PdmMic_CalculateFrameRms(const int16_t* samples, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif // PDM_MIC_HPP_
