#ifndef SPEAKER_VERIFICATION_HPP
#define SPEAKER_VERIFICATION_HPP

#if defined(__cplusplus)
#include <cstdint>
#include <cstddef>
#else
#include <stdint.h>
#include <stddef.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Main entry point for the ECAPA-TDNN Speaker Verification pipeline.
 * Runs on Arm Cortex-M55 + Ethos-U55 NPU.
 */
void RunSpeakerVerificationApp(void);

#ifdef __cplusplus
}
#endif

#endif // SPEAKER_VERIFICATION_HPP
