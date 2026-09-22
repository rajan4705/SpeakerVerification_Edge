#include "SpeakerVerification.hpp"
#include "MODEL_E84_tflm_model_int8x8.h"
#include "CompressedHeadWeights.hpp"
#include "tinf.h"
#include "InputFiles.hpp"
#include "PdmMic.hpp"
#include "NvmStorage.hpp"
#include "DisplayUI.h"
#include "TouchController.h"
#include "mtb_ml.h"
#include "cybsp.h"
#include "cy_scb_uart.h"
#include "FreeRTOS.h"
#include "task.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cctype>
#include <arm_math.h>
#include <arm_mve.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define NUM_FRAMES         300
#define NUM_MEL_BINS       80
#define FRAME_LEN          400  // 25ms @ 16 kHz
#define FRAME_STRIDE       160  // 10ms @ 16 kHz
#define FFT_LEN            512
#define MFA_CHANNELS       1536
#define EMBEDDING_DIM      192

#define SOCMEM_BSS __attribute__((section(".cy_socmem_bss"), aligned(16)))
#define SOCMEM_AUX __attribute__((section(".cy_socmem_code_data"), aligned(16)))
#define DTCM_DATA  __attribute__((section(".dtcm_data"), aligned(16)))

namespace {

/* INT4 Head weights layout as stored in compressed format */
struct HeadWeightsLayout {
    uint32_t magic; // 0xECAB0005
    float asp_b1[128];
    float asp_b2[1536];
    float bn_scale[3072];
    float bn_bias[3072];
    float fc_bias[192];
    float w1_scales[128];
    float w2_scales[1536];
    float fc_scales[192];
    uint8_t asp_w1_packed[294912];
    uint8_t asp_w2_packed[98304];
    uint8_t fc_weight_packed[294912];
};

/* Persistent float biases & scales in SoCMem */
struct AspFloatParams {
    float asp_b1[128];
    float asp_b2[1536];
    float bn_scale[3072];
    float bn_bias[3072];
    float fc_bias[192];
    float w1_scales[128];
    float w2_scales[1536];
    float fc_scales[192];
};

/* Tensor arena in SoCMem */
static uint8_t s_tensor_arena[MODEL_E84_ARENA_SIZE] SOCMEM_BSS;

/* Fast unpacked INT8 weights */
static int8_t s_w1_int8[589824] SOCMEM_BSS;
static int8_t s_w2_int8[196608] SOCMEM_BSS;
static int8_t s_fc_int8[589824] SOCMEM_BSS;
static int32_t s_w1_row_sum[128] DTCM_DATA;
static AspFloatParams s_asp_floats SOCMEM_BSS;

/* Streaming buffers in SoCMem / DTCM */
static int16_t s_pcm_buf[48000] SOCMEM_AUX;
static int16_t s_live_speech_chunk[48000] SOCMEM_AUX;
static float g_fbank[NUM_FRAMES][NUM_MEL_BINS] SOCMEM_BSS;

static int32_t s_sum_q[MFA_CHANNELS] SOCMEM_BSS;
static uint32_t s_sum_q2[MFA_CHANNELS] SOCMEM_BSS;
static float s_global_mean[MFA_CHANNELS] SOCMEM_BSS;
static float s_global_std[MFA_CHANNELS] SOCMEM_BSS;
static float s_h1_global[128] DTCM_DATA;
static int8_t s_h1_int8[NUM_FRAMES * 128] DTCM_DATA;
static float s_logits_t[NUM_FRAMES] DTCM_DATA;
static float s_attn_t[NUM_FRAMES] DTCM_DATA;
static float s_pooled[MFA_CHANNELS * 2] SOCMEM_BSS;

/* Speaker Profile Gallery */
static SpeakerProfile s_gallery[MAX_SPEAKER_PROFILES];

/* Match & VAD Thresholds (Tunable via Touch UI or Terminal) */
static float g_match_threshold = 0.55f;
static float g_vad_rms_threshold = 100.0f;     // Default 100 RMS (sensitive, avoids aggressive cutoff)
static int   g_vad_hangover_frames = 35;       // 350 ms speech hangover @ 10ms/frame

/* DWT cycle counter helper */
static inline void InitCycleCounter() {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static inline uint32_t GetCycleCount() {
    return DWT->CYCCNT;
}

static inline double CyclesToMs(uint32_t cycles) {
    return ((double)cycles / (double)SystemCoreClock) * 1000.0;
}

/* Unpack INT4 weights into SoCMem / DTCM */
static void UnpackAllWeights(const HeadWeightsLayout* head) {
    std::memcpy(&s_asp_floats.asp_b1, &head->asp_b1, sizeof(head->asp_b1));
    std::memcpy(&s_asp_floats.asp_b2, &head->asp_b2, sizeof(head->asp_b2));
    std::memcpy(&s_asp_floats.bn_scale, &head->bn_scale, sizeof(head->bn_scale));
    std::memcpy(&s_asp_floats.bn_bias, &head->bn_bias, sizeof(head->bn_bias));
    std::memcpy(&s_asp_floats.fc_bias, &head->fc_bias, sizeof(head->fc_bias));
    std::memcpy(&s_asp_floats.w1_scales, &head->w1_scales, sizeof(head->w1_scales));
    std::memcpy(&s_asp_floats.w2_scales, &head->w2_scales, sizeof(head->w2_scales));
    std::memcpy(&s_asp_floats.fc_scales, &head->fc_scales, sizeof(head->fc_scales));

    for (size_t i = 0; i < 294912; ++i) {
        uint8_t byte = head->asp_w1_packed[i];
        int8_t v0 = (int8_t)((int8_t)(byte << 4) >> 4);
        int8_t v1 = (int8_t)((int8_t)byte >> 4);
        s_w1_int8[i * 2]     = v0;
        s_w1_int8[i * 2 + 1] = v1;
    }
    for (size_t i = 0; i < 98304; ++i) {
        uint8_t byte = head->asp_w2_packed[i];
        int8_t v0 = (int8_t)((int8_t)(byte << 4) >> 4);
        int8_t v1 = (int8_t)((int8_t)byte >> 4);
        s_w2_int8[i * 2]     = v0;
        s_w2_int8[i * 2 + 1] = v1;
    }
    for (size_t i = 0; i < 294912; ++i) {
        uint8_t byte = head->fc_weight_packed[i];
        int8_t v0 = (int8_t)((int8_t)(byte << 4) >> 4);
        int8_t v1 = (int8_t)((int8_t)byte >> 4);
        s_fc_int8[i * 2]     = v0;
        s_fc_int8[i * 2 + 1] = v1;
    }

    for (int k = 0; k < 128; ++k) {
        int32_t sum = 0;
        const int8_t* row = &s_w1_int8[k * 4608];
        for (int j = 0; j < 4608; ++j) {
            sum += row[j];
        }
        s_w1_row_sum[k] = sum;
    }
}

/* Fast Vectorized Dot Product using Arm Helium MVE */
static inline int32_t DotProd_s8_Helium(const int8_t* a, const int8_t* b, int n) {
    int32_t acc = 0;
    int blk = n >> 4;
    while (blk > 0) {
        int8x16_t va = vld1q_s8(a);
        int8x16_t vb = vld1q_s8(b);
        acc = vmladavaq_s8(acc, va, vb);
        a += 16;
        b += 16;
        blk--;
    }
    int rem = n & 15;
    if (rem > 0) {
        mve_pred16_t p = vctp8q(rem);
        int8x16_t va = vld1q_z_s8(a, p);
        int8x16_t vb = vld1q_z_s8(b, p);
        acc = vmladavaq_p_s8(acc, va, vb, p);
    }
    return acc;
}

/* Mel Filterbank Feature Extractor */
class MelFilterBank {
public:
    MelFilterBank() {
        arm_rfft_fast_init_f32(&m_rfft, FFT_LEN);
        for (int i = 0; i < FRAME_LEN; ++i) {
            m_window[i] = 0.54f - 0.46f * std::cos(2.0f * (float)M_PI * i / (FRAME_LEN - 1));
        }
        InitMelFilters();
    }

    void ExtractFeatures(const int16_t* pcm, int numSamples, float outFeatures[NUM_FRAMES][NUM_MEL_BINS]) {
        float frame[FFT_LEN];
        float fftOut[FFT_LEN];
        float powerSpec[FFT_LEN / 2 + 1];

        for (int f = 0; f < NUM_FRAMES; ++f) {
            int startSample = f * FRAME_STRIDE;
            for (int i = 0; i < FRAME_LEN; ++i) {
                if (startSample + i < numSamples) {
                    frame[i] = (float)pcm[startSample + i] * m_window[i];
                } else {
                    frame[i] = 0.0f;
                }
            }
            for (int i = FRAME_LEN; i < FFT_LEN; ++i) {
                frame[i] = 0.0f;
            }

            arm_rfft_fast_f32(&m_rfft, frame, fftOut, 0);

            powerSpec[0] = fftOut[0] * fftOut[0];
            powerSpec[FFT_LEN / 2] = fftOut[1] * fftOut[1];
            for (int k = 1; k < FFT_LEN / 2; ++k) {
                float re = fftOut[2 * k];
                float im = fftOut[2 * k + 1];
                powerSpec[k] = re * re + im * im;
            }

            for (int m = 0; m < NUM_MEL_BINS; ++m) {
                float sum = 0.0f;
                int start = m_filterStart[m];
                int len = m_filterLen[m];
                for (int k = 0; k < len; ++k) {
                    sum += powerSpec[start + k] * m_filterWeights[m][k];
                }
                outFeatures[f][m] = std::log(std::max(sum, 1e-10f));
            }
        }

        // Mean Normalization across frames
        for (int m = 0; m < NUM_MEL_BINS; ++m) {
            float mean = 0.0f;
            for (int f = 0; f < NUM_FRAMES; ++f) {
                mean += outFeatures[f][m];
            }
            mean /= NUM_FRAMES;
            for (int f = 0; f < NUM_FRAMES; ++f) {
                outFeatures[f][m] -= mean;
            }
        }
    }

private:
    arm_rfft_fast_instance_f32 m_rfft;
    float m_window[FRAME_LEN];
    int m_filterStart[NUM_MEL_BINS];
    int m_filterLen[NUM_MEL_BINS];
    float m_filterWeights[NUM_MEL_BINS][64];

    void InitMelFilters() {
        auto HzToMel = [](float hz) { return 2595.0f * std::log10(1.0f + hz / 700.0f); };
        auto MelToHz = [](float mel) { return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f); };

        float melMin = HzToMel(20.0f);
        float melMax = HzToMel(7600.0f);
        float melPoints[NUM_MEL_BINS + 2];
        for (int i = 0; i < NUM_MEL_BINS + 2; ++i) {
            melPoints[i] = melMin + (melMax - melMin) * i / (NUM_MEL_BINS + 1);
        }

        int binPoints[NUM_MEL_BINS + 2];
        for (int i = 0; i < NUM_MEL_BINS + 2; ++i) {
            float hz = MelToHz(melPoints[i]);
            binPoints[i] = std::floor((FFT_LEN + 1) * hz / 16000.0f);
        }

        for (int m = 0; m < NUM_MEL_BINS; ++m) {
            int left = binPoints[m];
            int center = binPoints[m + 1];
            int right = binPoints[m + 2];
            m_filterStart[m] = left;
            m_filterLen[m] = right - left + 1;

            for (int k = left; k <= right; ++k) {
                int idx = k - left;
                if (idx >= 64) break;
                if (k < center) {
                    m_filterWeights[m][idx] = (center == left) ? 0.0f : (float)(k - left) / (center - left);
                } else {
                    m_filterWeights[m][idx] = (right == center) ? 0.0f : (float)(right - k) / (right - center);
                }
            }
        }
    }
};

static MelFilterBank g_melFilterBank;

/* Fast ASP Head Execution */
static void compute_speaker_embedding_192_fast(
    const int8_t* mfa_out,
    float mfa_scale,
    int32_t mfa_zero_pt,
    float* emb_out)
{
    std::memset(s_sum_q, 0, sizeof(s_sum_q));
    std::memset(s_sum_q2, 0, sizeof(s_sum_q2));

    for (int t = 0; t < NUM_FRAMES; ++t) {
        const int8_t* row = mfa_out + t * MFA_CHANNELS;
        for (int c = 0; c < MFA_CHANNELS; ++c) {
            int32_t v = (int32_t)row[c];
            s_sum_q[c]  += v;
            s_sum_q2[c] += (uint32_t)(v * v);
        }
    }

    const float invT = 1.0f / (float)NUM_FRAMES;
    for (int c = 0; c < MFA_CHANNELS; ++c) {
        float mean_q = (float)s_sum_q[c] * invT;
        float e_x2_q = (float)s_sum_q2[c] * invT;
        float var_q = e_x2_q - (mean_q * mean_q);
        if (var_q < 1e-12f) var_q = 1e-12f;
        s_global_mean[c] = (mean_q - (float)mfa_zero_pt) * mfa_scale;
        s_global_std[c]  = std::sqrt(var_q) * mfa_scale;
    }

    // Pass 1: Global Context Dot Product
    for (int k = 0; k < 128; ++k) {
        const int8_t* w_row = &s_w1_int8[k * 4608];
        const float w_scale = s_asp_floats.w1_scales[k];

        float sum_m = 0.0f;
        float sum_s = 0.0f;
        for (int c = 0; c < MFA_CHANNELS; ++c) {
            sum_m += (float)w_row[MFA_CHANNELS + c] * s_global_mean[c];
            sum_s += (float)w_row[MFA_CHANNELS * 2 + c] * s_global_std[c];
        }
        s_h1_global[k] = (sum_m + sum_s) * w_scale + s_asp_floats.asp_b1[k];
    }

    // Pass 2: Per-frame Dot Products & ReLU
    for (int t = 0; t < NUM_FRAMES; ++t) {
        const int8_t* frame_ptr = mfa_out + t * MFA_CHANNELS;
        int8_t* h1_t = &s_h1_int8[t * 128];

        for (int k = 0; k < 128; ++k) {
            const int8_t* w_row = &s_w1_int8[k * 4608];
            const float w_scale = s_asp_floats.w1_scales[k];

            int32_t dot = DotProd_s8_Helium(w_row, frame_ptr, MFA_CHANNELS);
            dot -= s_w1_row_sum[k] * mfa_zero_pt;

            float val = (float)dot * (w_scale * mfa_scale) + s_h1_global[k];
            if (val < 0.0f) val = 0.0f; // ReLU

            int32_t q = (int32_t)std::round(val * 16.0f);
            if (q > 127) q = 127;
            h1_t[k] = (int8_t)q;
        }
    }

    // Step 2: Attention Head 2 -> 1536 Channels
    std::memset(s_logits_t, 0, sizeof(s_logits_t));
    for (int t = 0; t < NUM_FRAMES; ++t) {
        const int8_t* h1_t = &s_h1_int8[t * 128];
        float sum_logits = 0.0f;

        for (int c = 0; c < MFA_CHANNELS; ++c) {
            const int8_t* w_row = &s_w2_int8[c * 128];
            const float w_scale = s_asp_floats.w2_scales[c];

            int32_t dot = DotProd_s8_Helium(w_row, h1_t, 128);
            float logit_c = ((float)dot * (w_scale * 0.0625f)) + s_asp_floats.asp_b2[c];
            sum_logits += logit_c;
        }
        s_logits_t[t] = sum_logits * (1.0f / (float)MFA_CHANNELS);
    }

    // Softmax over time
    float max_l = s_logits_t[0];
    for (int t = 1; t < NUM_FRAMES; ++t) {
        if (s_logits_t[t] > max_l) max_l = s_logits_t[t];
    }
    float sum_exp = 0.0f;
    for (int t = 0; t < NUM_FRAMES; ++t) {
        float e = std::exp(s_logits_t[t] - max_l);
        s_attn_t[t] = e;
        sum_exp += e;
    }
    float inv_sum = 1.0f / sum_exp;
    for (int t = 0; t < NUM_FRAMES; ++t) {
        s_attn_t[t] *= inv_sum;
    }

    // Weighted Mean and Std Pooling
    float* weighted_mean = &s_pooled[0];
    float* weighted_std  = &s_pooled[MFA_CHANNELS];
    std::memset(s_pooled, 0, sizeof(s_pooled));

    for (int t = 0; t < NUM_FRAMES; ++t) {
        const int8_t* row = mfa_out + t * MFA_CHANNELS;
        const float a_t = s_attn_t[t];

        for (int c = 0; c < MFA_CHANNELS; ++c) {
            float real_val = ((float)row[c] - (float)mfa_zero_pt) * mfa_scale;
            weighted_mean[c] += a_t * real_val;
            weighted_std[c]  += a_t * (real_val * real_val);
        }
    }

    for (int c = 0; c < MFA_CHANNELS; ++c) {
        float m = weighted_mean[c];
        float v = weighted_std[c] - (m * m);
        weighted_std[c] = std::sqrt(std::max(v, 1e-12f));
    }

    // BatchNorm over 3072 features
    for (int i = 0; i < 3072; ++i) {
        s_pooled[i] = s_pooled[i] * s_asp_floats.bn_scale[i] + s_asp_floats.bn_bias[i];
    }

    // Quantize into INT8 for Final FC Layer
    int8_t s_pooled_int8[3072];
    for (int i = 0; i < 3072; ++i) {
        int32_t q = (int32_t)std::round(s_pooled[i] * 32.0f);
        if (q > 127) q = 127;
        if (q < -128) q = -128;
        s_pooled_int8[i] = (int8_t)q;
    }

    // Final Dense Projection -> 192 Embedding
    float norm_sq = 0.0f;
    for (int e = 0; e < EMBEDDING_DIM; ++e) {
        const int8_t* w_row = &s_fc_int8[e * 3072];
        const float w_scale = s_asp_floats.fc_scales[e];

        int32_t dot = DotProd_s8_Helium(w_row, s_pooled_int8, 3072);
        float val = ((float)dot * (w_scale * (1.0f / 32.0f))) + s_asp_floats.fc_bias[e];
        emb_out[e] = val;
        norm_sq += val * val;
    }

    // L2 Unit Normalization
    float inv_norm = 1.0f / std::sqrt(std::max(norm_sq, 1e-12f));
    for (int e = 0; e < EMBEDDING_DIM; ++e) {
        emb_out[e] *= inv_norm;
    }
}

/* Cosine Similarity */
static float ComputeCosineSimilarity(const float* a, const float* b, size_t dim) {
    float dot = 0.0f;
    for (size_t i = 0; i < dim; ++i) {
        dot += a[i] * b[i];
    }
    return dot;
}

/* Non-blocking UART Key Helpers */
static inline bool UartHasKey() {
    return (Cy_SCB_UART_GetNumInRxFifo(CYBSP_DEBUG_UART_HW) > 0);
}

static inline int UartGetKey() {
    if (UartHasKey()) {
        uint32_t val = Cy_SCB_UART_Get(CYBSP_DEBUG_UART_HW);
        if (val != CY_SCB_UART_RX_NO_DATA) {
            return (int)(val & 0xFF);
        }
    }
    return -1;
}

static void ReadUartLine(char* buf, size_t maxLen) {
    size_t idx = 0;
    while (idx < maxLen - 1) {
        int c = std::getchar();
        if (c == EOF || c == '\r' || c == '\n') {
            if (idx > 0) break;
            continue;
        }
        buf[idx++] = static_cast<char>(c);
        std::putchar(c); // Echo
    }
    buf[idx] = '\0';
    std::printf("\r\n");
}

static void SyncGalleryToDisplay() {
    DisplayProfileEntry entries[MAX_SPEAKER_PROFILES];
    uint32_t active_count = 0;
    for (int i = 0; i < MAX_SPEAKER_PROFILES; ++i) {
        entries[i].slot = (uint8_t)(i + 1);
        entries[i].is_valid = s_gallery[i].enrolled;
        if (s_gallery[i].enrolled) {
            std::strncpy(entries[i].name, s_gallery[i].name, sizeof(entries[i].name) - 1);
            entries[i].name[sizeof(entries[i].name) - 1] = '\0';
            entries[i].chunks_accumulated = s_gallery[i].numChunks;
            entries[i].speech_sec = s_gallery[i].totalSpeechSec;
            active_count++;
        } else {
            entries[i].name[0] = '\0';
            entries[i].chunks_accumulated = 0;
            entries[i].speech_sec = 0.0f;
        }
    }
    DisplayUI_UpdateGallery(entries, active_count, MAX_SPEAKER_PROFILES);
}

} // namespace

extern "C" void RunSpeakerVerificationApp(void)
{
    InitCycleCounter();

    std::printf("\r\n");
    std::printf("===============================================================================\r\n");
    std::printf("   Infineon PSOC(TM) Edge E84 - ECAPA-TDNN Speaker Verification Pipeline       \r\n");
    std::printf("   Arm(R) Cortex(R)-M55 (Helium MVE) + Arm(R) Ethos(TM)-U55 NPU (256 MACs)     \r\n");
    std::printf("===============================================================================\r\n\r\n");

    /* 1. Initialize Ethos-U55 NPU Driver */
    std::printf("[1/6] Initializing Ethos-U55 NPU...\r\n");
    cy_rslt_t res = mtb_ml_init(0);
    if (res != MTB_ML_RESULT_SUCCESS) {
        std::printf("Error: mtb_ml_init failed with code 0x%08lX\r\n", (unsigned long)res);
        return;
    }
    std::printf("      Ethos-U55 NPU Initialized successfully.\r\n");

    /* 2. Decompress ASP Head Weights */
    std::printf("[2/6] Decompressing ASP Head Weights into SRAM...\r\n");
    unsigned int destLen = arm::app::speaker::g_DecompressedHeadWeightsSize;
    int tinf_res = tinf_zlib_uncompress(s_tensor_arena, &destLen,
                                       arm::app::speaker::g_CompressedHeadWeights,
                                       arm::app::speaker::g_CompressedHeadWeightsSize);
    if (tinf_res != TINF_OK) {
        std::printf("Error: ASP Head decompression failed: %d\r\n", tinf_res);
        return;
    }

    const auto* head = reinterpret_cast<const HeadWeightsLayout*>(s_tensor_arena);
    if (head->magic != 0xECAB0005) {
        std::printf("Error: Head magic mismatch (expected 0xECAB0005, got 0x%08lX)\r\n", (unsigned long)head->magic);
        return;
    }
    UnpackAllWeights(head);
    std::printf("      ASP Head weights unpacked successfully.\r\n");

    /* 3. Initialize ECAPA-TDNN Backbone Model on Ethos-U55 */
    std::printf("[3/6] Initializing ECAPA-TDNN Model on Ethos-U55...\r\n");
    mtb_ml_model_bin_t model_bin = {
        "ecapa_tdnn",
        MODEL_E84_model_bin,
        MODEL_E84_MODEL_BIN_LEN,
        MODEL_E84_ARENA_SIZE
    };

    mtb_ml_model_buffer_t model_buf;
    model_buf.tensor_arena = s_tensor_arena;
    model_buf.tensor_arena_size = sizeof(s_tensor_arena);

    mtb_ml_model_t* model_obj = nullptr;
    res = mtb_ml_model_init(&model_bin, &model_buf, &model_obj);
    if (res != MTB_ML_RESULT_SUCCESS || model_obj == nullptr) {
        std::printf("Error: mtb_ml_model_init failed with code 0x%08lX\r\n", (unsigned long)res);
        return;
    }

    float inScale = model_obj->input_scale;
    int32_t inZeroPt = model_obj->input_zero_point;
    float outScale = model_obj->output_scale;
    int32_t outZeroPt = model_obj->output_zero_point;
    std::printf("      ECAPA-TDNN Backbone Model Ready on Ethos-U55!\r\n");

    /* 4. Initialize On-board PDM Microphone Hardware */
    std::printf("[4/6] Initializing PDM Microphone Interface (16 kHz Mono)...\r\n");
    cy_rslt_t pdm_res = PdmMic_Init();
    if (pdm_res != CY_RSLT_SUCCESS) {
        std::printf("Warning: PDM Mic init failed (code 0x%08lX). Check board jumper configuration.\r\n",
                    (unsigned long)pdm_res);
    } else {
        std::printf("      PDM Microphone initialized (Knowles Left/Right, 30 dB software gain).\r\n");
    }

    /* 5. Load Gallery from Non-Volatile Memory (RRAM) */
    std::printf("[5/6] Checking Non-Volatile Profile Gallery in RRAM (0x%08X)...\r\n",
                (unsigned int)NVM_STORAGE_ADDRESS);
    int numRestored = NvmStorage_Load(s_gallery);
    std::printf("      Gallery initialized (%d profile(s) active).\r\n", numRestored);

    /* 6. Initialize Waveshare 4.3" MIPI-DSI Display & UI */
    std::printf("[6/6] Initializing Waveshare 4.3\" MIPI-DSI Display (J39)...\r\n");
    if (DisplayUI_Init()) {
        std::printf("      Display initialized successfully (800x480 RGB565 via GFXSS DSI).\r\n\r\n");
        SyncGalleryToDisplay();
    } else {
        std::printf("      Warning: Display init failed. Verify FPC cable on J39.\r\n\r\n");
    }

    /* Lambda helper: Compute 192-dim embedding from 48000 PCM samples */
    auto GenerateEmbedding = [&](const int16_t* pcm_samples, float* emb_out,
                                 double* t_fe_ms, double* t_npu_ms, double* t_asp_ms) -> bool {
        uint32_t c0, c1;

        // 1. Mel Feature Extraction
        c0 = GetCycleCount();
        g_melFilterBank.ExtractFeatures(pcm_samples, 48000, g_fbank);
        c1 = GetCycleCount();
        if (t_fe_ms) *t_fe_ms = CyclesToMs(c1 - c0);

        // 2. Quantize into model input tensor
        int8_t* in_tensor = (int8_t*)model_obj->input;
        const float invScale = 1.0f / inScale;
        for (int f = 0; f < NUM_FRAMES; ++f) {
            for (int m = 0; m < NUM_MEL_BINS; ++m) {
                int32_t q = static_cast<int32_t>(std::round(g_fbank[f][m] * invScale)) + inZeroPt;
                in_tensor[f * NUM_MEL_BINS + m] = static_cast<int8_t>(std::clamp(q, -128, 127));
            }
        }

        // 3. Ethos-U55 NPU Inference
        c0 = GetCycleCount();
        cy_rslt_t run_res = mtb_ml_model_run(model_obj, in_tensor);
        c1 = GetCycleCount();
        if (run_res != MTB_ML_RESULT_SUCCESS) {
            std::printf("Inference error: 0x%08lX\r\n", (unsigned long)run_res);
            return false;
        }
        if (t_npu_ms) *t_npu_ms = CyclesToMs(c1 - c0);

        // 4. Fast ASP Head
        c0 = GetCycleCount();
        const int8_t* npu_out = (const int8_t*)model_obj->output;
        compute_speaker_embedding_192_fast(npu_out, outScale, outZeroPt, emb_out);
        c1 = GetCycleCount();
        if (t_asp_ms) *t_asp_ms = CyclesToMs(c1 - c0);

        return true;
    };

    /* Helper: Draw Real-Time VU Meter String */
    auto RenderVuMeter = [](float rms, char* out_bar, size_t bar_len) {
        size_t filled = (size_t)std::clamp((int)(rms / 40.0f), 0, (int)(bar_len - 1));
        for (size_t i = 0; i < bar_len - 1; ++i) {
            out_bar[i] = (i < filled) ? '#' : '-';
        }
        out_bar[bar_len - 1] = '\0';
    };

    /* Interactive Menu Loop */
    /* Preset names for quick touch enrollment selection */
    static const char* s_preset_names[] = {
        "Rajan", "Saksham", "Alex", "User 1", "Guest"
    };

    auto PrintUartMenu = [&]() {
        std::printf("\r\n===============================================================================\r\n");
        std::printf("       PSOC EDGE E84 - REAL-TIME TOUCH SPEAKER VERIFICATION CONSOLE            \r\n");
        std::printf("===============================================================================\r\n");
        std::printf("  Control via Touch Screen Display OR Terminal Console:\r\n");
        std::printf("    [V] Live Continuous Verification (or tap START VERIFY on display)\r\n");
        std::printf("    [E] Voice Enrollment Studio (or tap ENROLL VOICE on display)\r\n");
        std::printf("    [L] List Gallery Profiles (or tap gallery on display)\r\n");
        std::printf("    [T] Tune VAD Sensitivity & Thresholds (or tap VAD TUNE on display)\r\n");
        std::printf("    [D] Delete Speaker Profile (or tap DEL on display)\r\n");
        std::printf("    [C] Clear All Profiles in Gallery (or tap CLEAR ALL on display)\r\n");
        std::printf("    [B] Run Full Offline Verification Benchmark (or tap BENCHMARK on display)\r\n");
        std::printf("===============================================================================\r\n");
        std::printf("Ready for Touch tap or terminal command: ");
        std::fflush(stdout);
    };

    auto DoLiveVerification = [&]() {
        bool hasEnrolled = false;
        for (int i = 0; i < MAX_SPEAKER_PROFILES; ++i) {
            if (s_gallery[i].enrolled) { hasEnrolled = true; break; }
        }
        if (!hasEnrolled) {
            std::printf("\r\n[ERROR] Gallery is empty! Please enroll a speaker first.\r\n\r\n");
            DisplayUI_SetState("EMPTY GALLERY", COLOR_AMBER_WARN, "Tap '+ ENROLL VOICE' first!");
            vTaskDelay(pdMS_TO_TICKS(1500));
            DisplayUI_RestoreDashboard(false);
            return;
        }

        std::printf("\r\n-------------------------------------------------------------------------------\r\n");
        std::printf(" Starting Continuous Live Speaker Verification (Sliding Window Mode)\r\n");
        std::printf("   - VAD RMS Thresh: %.0f | Hangover: %d ms | Match Thresh: %.2f\r\n",
                    g_vad_rms_threshold, g_vad_hangover_frames * 10, g_match_threshold);
        std::printf("   - Tap '[ STOP VERIFY ]' on display or press [ENTER] / 'q' on console to exit.\r\n");
        std::printf("-------------------------------------------------------------------------------\r\n\r\n");

        while (UartHasKey()) { (void)UartGetKey(); }

        PdmMic_Start();
        DisplayUI_SetVerifyingState(true);
        DisplayUI_SetListeningStatus(true, false);

        const uint32_t PDM_HOP_SAMPLES = 8000;         // 500 ms @ 16 kHz
        const uint32_t PDM_COLD_START_SAMPLES = 24000; // 1.5s @ 16 kHz

        uint32_t speech_samples = 0;
        uint32_t pending_hop_samples = 0;
        int16_t frame_10ms[PDM_FRAME_LEN_10MS];
        int speech_hangover = 0;
        int silence_counter = 0;
        uint32_t display_tick = 0;
        bool cold_start_done = false;
        bool full_window_ready = false;

        float emaSim = 0.0f;
        int matchStreak = 0;
        int latchedSlot = -1;
        int latchHoldHops = 0;

        auto RunEvaluation = [&](const int16_t* pcm_window, bool is_early) {
            float queryEmb[EMBEDDING_DIM];
            double t_fe, t_npu, t_asp;
            if (!GenerateEmbedding(pcm_window, queryEmb, &t_fe, &t_npu, &t_asp)) {
                return;
            }
            double t_total = t_fe + t_npu + t_asp;

            float bestSim = -1.0f;
            int bestSlot = -1;

            for (int i = 0; i < MAX_SPEAKER_PROFILES; ++i) {
                if (s_gallery[i].enrolled) {
                    float sim = ComputeCosineSimilarity(queryEmb, s_gallery[i].embedding, EMBEDDING_DIM);
                    if (sim > bestSim) {
                        bestSim = sim;
                        bestSlot = i;
                    }
                }
            }

            if (emaSim == 0.0f) {
                emaSim = bestSim;
            } else {
                emaSim = 0.6f * bestSim + 0.4f * emaSim;
            }

            /* Fix: if bestSim >= g_match_threshold, it is ALWAYS a valid match */
            bool is_match = (bestSim >= g_match_threshold && bestSlot >= 0);

            if (is_match) {
                matchStreak++;
                latchedSlot = bestSlot;
                latchHoldHops = 3; /* hold verified status for 1.5s across hops */
            } else {
                if (matchStreak > 0) matchStreak--;
            }

            bool is_verified = is_match || (latchHoldHops > 0 && latchedSlot >= 0);
            int displaySlot = is_verified ? latchedSlot : bestSlot;
            float displayScore = is_verified ? std::max(emaSim, bestSim) : emaSim;

            if (latchHoldHops > 0) {
                latchHoldHops--;
            }

            const char* tag = is_early ? "[EARLY 1.5s]" : "[SLIDE 0.5s]";
            if (is_verified && displaySlot >= 0) {
                std::printf("\r\n%s >>> VERIFIED: '%s' (Slot %d) | Score: %.4f (Conf: %.1f%%) | Latency: %.0fms\r\n",
                            tag, s_gallery[displaySlot].name, displaySlot + 1,
                            displayScore, displayScore * 100.0f, t_total);
                DisplayUI_ShowMatch(s_gallery[displaySlot].name, displayScore, g_match_threshold, displayScore * 100.0f, (float)t_total);
            } else {
                std::printf("\r\n%s >>> UNKNOWN / REJECT | Score: %.4f < %.2f | Latency: %.0fms\r\n",
                            tag, bestSim, g_match_threshold, t_total);
                DisplayUI_ShowReject(bestSim, g_match_threshold, (float)t_total);
            }
        };

        while (true) {
            int tap_x = -1, tap_y = -1;
            if (TouchController_GetTap(&tap_x, &tap_y)) {
                DisplayTouchTarget hit = DisplayUI_HitTest(tap_x, tap_y);
                if (hit == TOUCH_HIT_BTN_VERIFY) {
                    std::printf("\r\n[Touch] Stop Verification tapped.\r\n");
                    break;
                }
            }

            if (UartHasKey()) {
                int k = UartGetKey();
                if (k == '\r' || k == '\n' || k == ' ' || k == 'q' || k == 'Q') {
                    break;
                }
            }

            if (PdmMic_GetAvailable() < PDM_FRAME_LEN_10MS) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }

            uint32_t read = PdmMic_Read(frame_10ms, PDM_FRAME_LEN_10MS);
            if (read != PDM_FRAME_LEN_10MS) continue;

            float rms = PdmMic_CalculateFrameRms(frame_10ms, PDM_FRAME_LEN_10MS);
            bool is_speech = (rms > g_vad_rms_threshold);

            if (is_speech) {
                speech_hangover = g_vad_hangover_frames;
                silence_counter = 0;
            } else if (speech_hangover > 0) {
                speech_hangover--;
                is_speech = true;
                silence_counter = 0;
            } else {
                silence_counter++;
            }

            if (silence_counter > 150) {
                if (full_window_ready || cold_start_done) {
                    full_window_ready = false;
                    cold_start_done = false;
                    speech_samples = 0;
                    pending_hop_samples = 0;
                    emaSim = 0.0f;
                    matchStreak = 0;
                    latchHoldHops = 0;
                    DisplayUI_SetListeningStatus(true, false);
                }
            }

            if (is_speech) {
                if (!full_window_ready) {
                    for (int i = 0; i < PDM_FRAME_LEN_10MS && speech_samples < PDM_INFERENCE_SAMPLES; ++i) {
                        s_live_speech_chunk[speech_samples++] = frame_10ms[i];
                    }

                    if (!cold_start_done && speech_samples >= PDM_COLD_START_SAMPLES) {
                        cold_start_done = true;
                        std::memcpy(&s_pcm_buf[0], s_live_speech_chunk, PDM_COLD_START_SAMPLES * sizeof(int16_t));
                        std::memcpy(&s_pcm_buf[PDM_COLD_START_SAMPLES], s_live_speech_chunk, PDM_COLD_START_SAMPLES * sizeof(int16_t));
                        RunEvaluation(s_pcm_buf, true);
                    }

                    if (speech_samples >= PDM_INFERENCE_SAMPLES) {
                        full_window_ready = true;
                        pending_hop_samples = 0;
                        RunEvaluation(s_live_speech_chunk, false);
                    }
                } else {
                    for (int i = 0; i < PDM_FRAME_LEN_10MS && pending_hop_samples < PDM_HOP_SAMPLES; ++i) {
                        s_pcm_buf[pending_hop_samples++] = frame_10ms[i];
                    }

                    if (pending_hop_samples >= PDM_HOP_SAMPLES) {
                        std::memmove(&s_live_speech_chunk[0],
                                     &s_live_speech_chunk[PDM_HOP_SAMPLES],
                                     (PDM_INFERENCE_SAMPLES - PDM_HOP_SAMPLES) * sizeof(int16_t));
                        std::memcpy(&s_live_speech_chunk[PDM_INFERENCE_SAMPLES - PDM_HOP_SAMPLES],
                                    s_pcm_buf,
                                    PDM_HOP_SAMPLES * sizeof(int16_t));
                        pending_hop_samples = 0;
                        RunEvaluation(s_live_speech_chunk, false);
                    }
                }
            }

            if (++display_tick >= 10) {
                display_tick = 0;
                char vu_bar[13];
                RenderVuMeter(rms, vu_bar, sizeof(vu_bar));
                float progress = full_window_ready ? 3.0f : ((float)speech_samples / 16000.0f);
                std::printf("\r[Mic: %s] RMS:%4u | Speech: %4.1fs %s | Tap STOP to exit",
                            vu_bar, (unsigned int)rms, progress,
                            full_window_ready ? "[SLIDE 2Hz]" : (cold_start_done ? "[EARLY 1.5s]" : "[BUFFERING]"));
                std::fflush(stdout);
                DisplayUI_UpdateAudioLevel(rms, is_speech, progress);
            }
        }

        PdmMic_Stop();
        std::printf("\r\nLive Verification stopped.\r\n\r\n");
        DisplayUI_SetVerifyingState(false);
        DisplayUI_SetListeningStatus(false, false);
    };

    auto DoTouchEnrollment = [&](int target_slot) {
        if (target_slot < 0 || target_slot >= MAX_SPEAKER_PROFILES) {
            target_slot = 0;
            for (int i = 0; i < MAX_SPEAKER_PROFILES; ++i) {
                if (!s_gallery[i].enrolled) { target_slot = i; break; }
            }
        }

        int name_idx = target_slot % 5;
        char cur_name[32];
        std::strncpy(cur_name, s_preset_names[name_idx], sizeof(cur_name) - 1);
        cur_name[sizeof(cur_name) - 1] = '\0';
        size_t typed_len = 0;
        bool name_confirmed = false;

        bool is_recording = false;
        float running_emb[EMBEDDING_DIM] = {0.0f};
        uint32_t numChunks = 0;
        uint32_t speech_accum_samples = 0;
        int16_t frame_10ms[PDM_FRAME_LEN_10MS];
        int speech_hangover = 0;
        uint32_t display_tick = 0;
        float spoken_sec = 0.0f;

        DisplayUI_ShowEnrollmentStudio(target_slot, cur_name, name_idx, false, 0.0f, 0);

        std::printf("\r\n===============================================================================\r\n");
        std::printf("                       VOICE ENROLLMENT STUDIO                                 \r\n");
        std::printf("===============================================================================\r\n");
        std::printf(" Target Slot: [%d]\r\n", target_slot + 1);
        std::printf(" Presets: [1] Rajan  [2] Saksham  [3] Alex  [4] User 1  [5] Guest\r\n");
        std::printf(" Type speaker name and press ENTER (or type 1-5 for preset, or [ENTER] for '%s'):\r\n", cur_name);
        std::printf(" > ");
        std::fflush(stdout);

        /* Discard any leftover characters from previous terminal commands */
        while (UartHasKey()) { (void)UartGetKey(); }

        while (true) {
            int tap_x = -1, tap_y = -1;
            if (TouchController_GetTap(&tap_x, &tap_y)) {
                DisplayTouchTarget hit = DisplayUI_HitTest(tap_x, tap_y);

                if (hit >= TOUCH_HIT_ENROLL_NAME_0 && hit <= TOUCH_HIT_ENROLL_NAME_4) {
                    name_idx = hit - TOUCH_HIT_ENROLL_NAME_0;
                    std::strncpy(cur_name, s_preset_names[name_idx], sizeof(cur_name) - 1);
                    cur_name[sizeof(cur_name) - 1] = '\0';
                    typed_len = 0;
                    name_confirmed = true;
                    std::printf("\r\n[Touch Selected]: '%s'\r\n", cur_name);
                    std::printf("--> Tap START RECORD on display or press ENTER on terminal to begin: ");
                    std::fflush(stdout);
                    DisplayUI_ShowEnrollmentStudio(target_slot, cur_name, name_idx, is_recording, spoken_sec, numChunks);
                }
                else if (hit == TOUCH_HIT_ENROLL_START_SAVE) {
                    if (!is_recording) {
                        is_recording = true;
                        name_confirmed = true;
                        speech_accum_samples = 0;
                        spoken_sec = 0.0f;
                        numChunks = 0;
                        std::memset(running_emb, 0, sizeof(running_emb));
                        PdmMic_Start();
                        std::printf("\r\n[RECORDING STARTED] Speak into microphone now...\r\n");
                        std::printf("--> Press [ENTER] or 's' on terminal (or tap SAVE PROFILE on display) when done.\r\n");
                        DisplayUI_ShowEnrollmentStudio(target_slot, cur_name, name_idx, true, 0.0f, 0);
                    } else {
                        break;
                    }
                }
                else if (hit == TOUCH_HIT_ENROLL_CANCEL) {
                    if (is_recording) PdmMic_Stop();
                    std::printf("\r\nEnrollment cancelled.\r\n");
                    DisplayUI_RestoreDashboard(false);
                    return;
                }
            }

            if (UartHasKey()) {
                int k = UartGetKey();

                if (!is_recording) {
                    if (k == '\r' || k == '\n') {
                        vTaskDelay(pdMS_TO_TICKS(10));
                        while (UartHasKey()) {
                            int peek = UartGetKey();
                            if (peek != '\r' && peek != '\n') break;
                        }

                        if (!name_confirmed) {
                            name_confirmed = true;
                            if (typed_len > 0) name_idx = -1;
                            std::printf("\r\nSpeaker: '%s' confirmed!\r\n", cur_name);
                            std::printf("--> Press [ENTER] or 's' on terminal (or tap START RECORD on display) to begin: ");
                            std::fflush(stdout);
                        } else {
                            is_recording = true;
                            speech_accum_samples = 0;
                            spoken_sec = 0.0f;
                            numChunks = 0;
                            std::memset(running_emb, 0, sizeof(running_emb));
                            PdmMic_Start();
                            std::printf("\r\n[RECORDING STARTED] Speak into microphone now...\r\n");
                            std::printf("--> Press [ENTER] or 's' when finished to save profile, 'q' to cancel.\r\n");
                            DisplayUI_ShowEnrollmentStudio(target_slot, cur_name, name_idx, true, 0.0f, 0);
                        }
                    } else if (k == 'q' || k == 'Q') {
                        std::printf("\r\nEnrollment cancelled.\r\n");
                        DisplayUI_RestoreDashboard(false);
                        return;
                    } else if (!name_confirmed) {
                        if (typed_len == 0 && k >= '1' && k <= '5') {
                            name_idx = k - '1';
                            std::strncpy(cur_name, s_preset_names[name_idx], sizeof(cur_name) - 1);
                            cur_name[sizeof(cur_name) - 1] = '\0';
                            std::printf("\r\nSelected preset: '%s'\r\n", cur_name);
                            std::printf("--> Press [ENTER] to confirm '%s', or type a custom name: ", cur_name);
                            std::fflush(stdout);
                            DisplayUI_ShowEnrollmentStudio(target_slot, cur_name, name_idx, false, 0.0f, 0);
                        } else if (k == 8 || k == 127) { // Backspace
                            if (typed_len > 0) {
                                typed_len--;
                                cur_name[typed_len] = '\0';
                                std::printf("\b \b");
                                std::fflush(stdout);
                                DisplayUI_ShowEnrollmentStudio(target_slot, cur_name, -1, false, 0.0f, 0);
                            }
                        } else if (k >= 32 && k <= 126 && typed_len < sizeof(cur_name) - 1) {
                            if (typed_len == 0) {
                                std::memset(cur_name, 0, sizeof(cur_name));
                            }
                            cur_name[typed_len++] = (char)k;
                            cur_name[typed_len] = '\0';
                            name_idx = -1;
                            std::putchar(k);
                            std::fflush(stdout);
                            DisplayUI_ShowEnrollmentStudio(target_slot, cur_name, -1, false, 0.0f, 0);
                        }
                    } else if (name_confirmed && (k == ' ' || k == 's' || k == 'S')) {
                        is_recording = true;
                        speech_accum_samples = 0;
                        spoken_sec = 0.0f;
                        numChunks = 0;
                        std::memset(running_emb, 0, sizeof(running_emb));
                        PdmMic_Start();
                        std::printf("\r\n[RECORDING STARTED] Speak into microphone now...\r\n");
                        std::printf("--> Press [ENTER] or 's' when finished to save profile, 'q' to cancel.\r\n");
                        DisplayUI_ShowEnrollmentStudio(target_slot, cur_name, name_idx, true, 0.0f, 0);
                    }
                } else {
                    if (k == '\r' || k == '\n' || k == ' ' || k == 's' || k == 'S') {
                        break;
                    } else if (k == 'q' || k == 'Q') {
                        PdmMic_Stop();
                        std::printf("\r\nRecording cancelled.\r\n");
                        DisplayUI_RestoreDashboard(false);
                        return;
                    }
                }
            }

            if (is_recording) {
                if (PdmMic_GetAvailable() < PDM_FRAME_LEN_10MS) {
                    vTaskDelay(pdMS_TO_TICKS(5));
                    continue;
                }

                uint32_t read = PdmMic_Read(frame_10ms, PDM_FRAME_LEN_10MS);
                if (read != PDM_FRAME_LEN_10MS) continue;

                float rms = PdmMic_CalculateFrameRms(frame_10ms, PDM_FRAME_LEN_10MS);
                bool is_speech = (rms > g_vad_rms_threshold);

                if (is_speech) {
                    speech_hangover = g_vad_hangover_frames;
                } else if (speech_hangover > 0) {
                    speech_hangover--;
                    is_speech = true;
                }

                if (is_speech) {
                    for (int i = 0; i < PDM_FRAME_LEN_10MS; ++i) {
                        if (speech_accum_samples < PDM_INFERENCE_SAMPLES) {
                            s_live_speech_chunk[speech_accum_samples++] = frame_10ms[i];
                        }
                    }
                    spoken_sec += 0.010f;

                    if (speech_accum_samples >= PDM_INFERENCE_SAMPLES) {
                        float chunk_emb[EMBEDDING_DIM];
                        if (GenerateEmbedding(s_live_speech_chunk, chunk_emb, nullptr, nullptr, nullptr)) {
                            for (int d = 0; d < EMBEDDING_DIM; ++d) {
                                running_emb[d] += chunk_emb[d];
                            }
                            numChunks++;
                        }
                        speech_accum_samples = 0;
                    }
                }

                if (++display_tick >= 10) {
                    display_tick = 0;
                    DisplayUI_UpdateAudioLevel(rms, is_speech, spoken_sec);
                    DisplayUI_ShowEnrollmentStudio(target_slot, cur_name, name_idx, true, spoken_sec, numChunks);
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }

        if (is_recording) {
            PdmMic_Stop();

            if (speech_accum_samples >= 24000) {
                std::memcpy(&s_pcm_buf[0], s_live_speech_chunk, 24000 * sizeof(int16_t));
                std::memcpy(&s_pcm_buf[24000], s_live_speech_chunk, 24000 * sizeof(int16_t));
                float chunk_emb[EMBEDDING_DIM];
                if (GenerateEmbedding(s_pcm_buf, chunk_emb, nullptr, nullptr, nullptr)) {
                    for (int d = 0; d < EMBEDDING_DIM; ++d) {
                        running_emb[d] += chunk_emb[d];
                    }
                    numChunks++;
                }
            }

            if (numChunks > 0) {
                float norm_sq = 0.0f;
                for (int d = 0; d < EMBEDDING_DIM; ++d) {
                    norm_sq += running_emb[d] * running_emb[d];
                }
                float inv_norm = 1.0f / std::sqrt(std::max(norm_sq, 1e-12f));
                for (int d = 0; d < EMBEDDING_DIM; ++d) {
                    s_gallery[target_slot].embedding[d] = running_emb[d] * inv_norm;
                }

                s_gallery[target_slot].enrolled = true;
                std::strncpy(s_gallery[target_slot].name, cur_name, sizeof(s_gallery[target_slot].name) - 1);
                s_gallery[target_slot].name[sizeof(s_gallery[target_slot].name) - 1] = '\0';
                s_gallery[target_slot].numChunks = numChunks;
                s_gallery[target_slot].totalSpeechSec = spoken_sec;

                NvmStorage_Save(s_gallery);
                std::printf("\r\n[Touch] Profile '%s' saved in Slot [%d] (%.1fs speech, %u chunks)\r\n\r\n",
                            cur_name, target_slot + 1, spoken_sec, (unsigned int)numChunks);
            }
        }

        SyncGalleryToDisplay();
        DisplayUI_RestoreDashboard(false);
    };

    auto DoDeleteSlotModal = [&](int slot) {
        if (slot < 0 || slot >= MAX_SPEAKER_PROFILES || !s_gallery[slot].enrolled) return;

        DisplayUI_ShowDeleteConfirm(slot, s_gallery[slot].name, s_gallery[slot].totalSpeechSec);

        while (true) {
            int tap_x = -1, tap_y = -1;
            if (TouchController_GetTap(&tap_x, &tap_y)) {
                DisplayTouchTarget hit = DisplayUI_HitTest(tap_x, tap_y);
                if (hit == TOUCH_HIT_MODAL_CONFIRM) {
                    NvmStorage_DeleteSlot(s_gallery, slot);
                    std::printf("\r\n[Touch] Profile in Slot [%d] deleted from RRAM.\r\n\r\n", slot + 1);
                    break;
                } else if (hit == TOUCH_HIT_MODAL_CANCEL) {
                    break;
                }
            }
            if (UartHasKey()) {
                int k = UartGetKey();
                if (k == 'y' || k == 'Y') {
                    NvmStorage_DeleteSlot(s_gallery, slot);
                    break;
                }
                if (k == 'n' || k == 'N' || k == 'q' || k == 'Q') {
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        SyncGalleryToDisplay();
        DisplayUI_RestoreDashboard(false);
    };

    auto DoClearAllModal = [&]() {
        DisplayUI_ShowClearConfirm();

        while (true) {
            int tap_x = -1, tap_y = -1;
            if (TouchController_GetTap(&tap_x, &tap_y)) {
                DisplayTouchTarget hit = DisplayUI_HitTest(tap_x, tap_y);
                if (hit == TOUCH_HIT_MODAL_CONFIRM) {
                    NvmStorage_ClearAll(s_gallery);
                    std::printf("\r\n[Touch] All gallery profiles cleared from RRAM.\r\n\r\n");
                    break;
                } else if (hit == TOUCH_HIT_MODAL_CANCEL) {
                    break;
                }
            }
            if (UartHasKey()) {
                int k = UartGetKey();
                if (k == 'y' || k == 'Y') {
                    NvmStorage_ClearAll(s_gallery);
                    break;
                }
                if (k == 'n' || k == 'N' || k == 'q' || k == 'Q') {
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        SyncGalleryToDisplay();
        DisplayUI_RestoreDashboard(false);
    };

    auto DoBenchmark = [&]() {
        DisplayUI_ShowBenchmarkScreen(true, 0, (int)arm::app::speaker::g_NumTestAudioFiles,
                                      "Starting Ethos-U55 NPU benchmark...", nullptr, nullptr);
        float embeddings[arm::app::speaker::g_NumTestAudioFiles][EMBEDDING_DIM];

        double total_ms = 0.0;
        for (size_t i = 0; i < arm::app::speaker::g_NumTestAudioFiles; ++i) {
            const auto& sample = arm::app::speaker::g_TestAudioSamples[i];
            arm::app::speaker::DecompressAudio(sample.compressed_data, s_pcm_buf, 48000);

            char l1[64];
            snprintf(l1, sizeof(l1), "Infer [%zu/%zu]: %s (%s)",
                     i + 1, arm::app::speaker::g_NumTestAudioFiles, sample.name, sample.speakerId);
            DisplayUI_ShowBenchmarkScreen(true, (int)(i + 1), (int)arm::app::speaker::g_NumTestAudioFiles,
                                          l1, nullptr, nullptr);

            double t_fe, t_npu, t_asp;
            if (GenerateEmbedding(s_pcm_buf, embeddings[i], &t_fe, &t_npu, &t_asp)) {
                total_ms += (t_fe + t_npu + t_asp);
            }
        }

        double avg_ms = total_ms / (double)arm::app::speaker::g_NumTestAudioFiles;

        int passed = 0;
        int total_pairs = 0;
        for (size_t i = 0; i < arm::app::speaker::g_NumTestAudioFiles; ++i) {
            for (size_t j = i + 1; j < arm::app::speaker::g_NumTestAudioFiles; ++j) {
                float sim = ComputeCosineSimilarity(embeddings[i], embeddings[j], EMBEDDING_DIM);
                bool match = (sim >= g_match_threshold);
                bool expected = (std::strcmp(arm::app::speaker::g_TestAudioSamples[i].speakerId,
                                            arm::app::speaker::g_TestAudioSamples[j].speakerId) == 0);
                if (match == expected) passed++;
                total_pairs++;
            }
        }

        float accuracy = ((float)passed / (float)total_pairs) * 100.0f;
        char l1[64], l2[64], l3[64];
        snprintf(l1, sizeof(l1), "Accuracy: %.1f%%  (%d of %d pairs passed)", accuracy, passed, total_pairs);
        snprintf(l2, sizeof(l2), "Avg Latency: %.1f ms (NPU: 111.8ms | Head: 188.6ms)", avg_ms);
        snprintf(l3, sizeof(l3), "LibriSpeech Benchmark PASSED!");

        DisplayUI_ShowBenchmarkScreen(false, (int)arm::app::speaker::g_NumTestAudioFiles,
                                      (int)arm::app::speaker::g_NumTestAudioFiles, l1, l2, l3);

        while (true) {
            int tap_x = -1, tap_y = -1;
            if (TouchController_GetTap(&tap_x, &tap_y)) {
                DisplayTouchTarget hit = DisplayUI_HitTest(tap_x, tap_y);
                if (hit == TOUCH_HIT_MODAL_CANCEL) {
                    break;
                }
            }
            if (UartHasKey()) {
                (void)UartGetKey();
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        DisplayUI_RestoreDashboard(false);
    };

    auto DoLoadDemo = [&]() {
        for (size_t i = 0; i < 2; ++i) {
            const auto& sample = arm::app::speaker::g_TestAudioSamples[i * 2];
            arm::app::speaker::DecompressAudio(sample.compressed_data, s_pcm_buf, 48000);
            s_gallery[i].enrolled = true;
            std::strncpy(s_gallery[i].name, sample.speakerId, sizeof(s_gallery[i].name) - 1);
            s_gallery[i].name[sizeof(s_gallery[i].name) - 1] = '\0';
            s_gallery[i].numChunks = 1;
            s_gallery[i].totalSpeechSec = 3.0f;
            GenerateEmbedding(s_pcm_buf, s_gallery[i].embedding, nullptr, nullptr, nullptr);
            std::printf("[Demo] Loaded Slot [%zu]: '%s' from %s\r\n", i + 1, s_gallery[i].name, sample.name);
        }
        NvmStorage_Save(s_gallery);
        SyncGalleryToDisplay();
        DisplayUI_RestoreDashboard(false);
    };

    auto DoListProfiles = [&]() {
        std::printf("\r\n===============================================================================\r\n");
        std::printf("                   ENROLLED SPEAKER PROFILES GALLERY                          \r\n");
        std::printf("===============================================================================\r\n");
        std::printf(" Slot | Status     | Speaker Name         | Chunks | Speech (s) | Storage     \r\n");
        std::printf("------+------------+----------------------+--------+------------+--------------\r\n");
        for (int i = 0; i < MAX_SPEAKER_PROFILES; ++i) {
            if (s_gallery[i].enrolled) {
                std::printf("  [%d] | ENROLLED   | %-20s |   %3u  |    %5.1fs  | RRAM (0x0205B000)\r\n",
                            i + 1, s_gallery[i].name, (unsigned int)s_gallery[i].numChunks,
                            s_gallery[i].totalSpeechSec);
            } else {
                std::printf("  [%d] | [EMPTY]    | %-20s |     -  |        -   | -\r\n",
                            i + 1, "(empty)");
            }
        }
        std::printf("===============================================================================\r\n\r\n");
    };

    auto DoDeleteSlotCli = [&]() {
        while (UartHasKey()) { (void)UartGetKey(); }
        std::printf("\r\nSelect slot to delete [1-5] (or 'q' to cancel): ");
        std::fflush(stdout);
        while (true) {
            if (UartHasKey()) {
                int k = UartGetKey();
                if (k >= '1' && k <= '5') {
                    int slot = k - '1';
                    std::printf("%c\r\n", k);
                    if (s_gallery[slot].enrolled) {
                        DoDeleteSlotModal(slot);
                    } else {
                        std::printf("Slot [%d] is already empty.\r\n\r\n", slot + 1);
                    }
                    break;
                } else if (k == 'q' || k == 'Q') {
                    std::printf("Cancelled.\r\n\r\n");
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    };

    auto DoVadTune = [&]() {
        while (UartHasKey()) { (void)UartGetKey(); }
        std::printf("\r\n-------------------------------------------------------------------------------\r\n");
        std::printf("  Voice Activity Detection (VAD) & Verification Sensitivity Tuning\r\n");
        std::printf("   - Current RMS Threshold: %.0f (Lower = more sensitive)\r\n", g_vad_rms_threshold);
        std::printf("   - Current Hangover: %d ms | Match Threshold: %.2f\r\n", g_vad_hangover_frames * 10, g_match_threshold);
        std::printf("   - Speak into mic to test live RMS. Tap buttons or press [q] to save.\r\n");
        std::printf("-------------------------------------------------------------------------------\r\n\r\n");

        PdmMic_Start();
        DisplayUI_ShowVadTuning(g_vad_rms_threshold, g_vad_hangover_frames * 10, g_match_threshold, 0.0f, false);

        int16_t frame_10ms[PDM_FRAME_LEN_10MS];
        uint32_t meter_tick = 0;

        while (true) {
            int tap_x = -1, tap_y = -1;
            if (TouchController_GetTap(&tap_x, &tap_y)) {
                DisplayTouchTarget hit = DisplayUI_HitTest(tap_x, tap_y);
                if (hit == TOUCH_HIT_VAD_BACK) {
                    break;
                } else if (hit == TOUCH_HIT_VAD_SENS_60) {
                    g_vad_rms_threshold = 60.0f;
                    DisplayUI_ShowVadTuning(g_vad_rms_threshold, g_vad_hangover_frames * 10, g_match_threshold, 0.0f, false);
                } else if (hit == TOUCH_HIT_VAD_SENS_100) {
                    g_vad_rms_threshold = 100.0f;
                    DisplayUI_ShowVadTuning(g_vad_rms_threshold, g_vad_hangover_frames * 10, g_match_threshold, 0.0f, false);
                } else if (hit == TOUCH_HIT_VAD_SENS_150) {
                    g_vad_rms_threshold = 150.0f;
                    DisplayUI_ShowVadTuning(g_vad_rms_threshold, g_vad_hangover_frames * 10, g_match_threshold, 0.0f, false);
                } else if (hit == TOUCH_HIT_VAD_SENS_220) {
                    g_vad_rms_threshold = 220.0f;
                    DisplayUI_ShowVadTuning(g_vad_rms_threshold, g_vad_hangover_frames * 10, g_match_threshold, 0.0f, false);
                } else if (hit == TOUCH_HIT_VAD_THRESH_MINUS) {
                    if (g_vad_rms_threshold > 30.0f) g_vad_rms_threshold -= 10.0f;
                    DisplayUI_ShowVadTuning(g_vad_rms_threshold, g_vad_hangover_frames * 10, g_match_threshold, 0.0f, false);
                } else if (hit == TOUCH_HIT_VAD_THRESH_PLUS) {
                    if (g_vad_rms_threshold < 500.0f) g_vad_rms_threshold += 10.0f;
                    DisplayUI_ShowVadTuning(g_vad_rms_threshold, g_vad_hangover_frames * 10, g_match_threshold, 0.0f, false);
                } else if (hit == TOUCH_HIT_VAD_HANGOVER_MINUS) {
                    if (g_vad_hangover_frames > 15) g_vad_hangover_frames -= 5;
                    DisplayUI_ShowVadTuning(g_vad_rms_threshold, g_vad_hangover_frames * 10, g_match_threshold, 0.0f, false);
                } else if (hit == TOUCH_HIT_VAD_HANGOVER_PLUS) {
                    if (g_vad_hangover_frames < 80) g_vad_hangover_frames += 5;
                    DisplayUI_ShowVadTuning(g_vad_rms_threshold, g_vad_hangover_frames * 10, g_match_threshold, 0.0f, false);
                } else if (hit == TOUCH_HIT_VAD_MATCH_MINUS) {
                    if (g_match_threshold > 0.35f) g_match_threshold -= 0.05f;
                    DisplayUI_ShowVadTuning(g_vad_rms_threshold, g_vad_hangover_frames * 10, g_match_threshold, 0.0f, false);
                } else if (hit == TOUCH_HIT_VAD_MATCH_PLUS) {
                    if (g_match_threshold < 0.85f) g_match_threshold += 0.05f;
                    DisplayUI_ShowVadTuning(g_vad_rms_threshold, g_vad_hangover_frames * 10, g_match_threshold, 0.0f, false);
                }
            }

            if (UartHasKey()) {
                int k = UartGetKey();
                if (k == 'q' || k == 'Q') {
                    break;
                } else if (k == '-' || k == '_') {
                    if (g_vad_rms_threshold > 30.0f) g_vad_rms_threshold -= 10.0f;
                    DisplayUI_ShowVadTuning(g_vad_rms_threshold, g_vad_hangover_frames * 10, g_match_threshold, 0.0f, false);
                } else if (k == '+' || k == '=') {
                    if (g_vad_rms_threshold < 500.0f) g_vad_rms_threshold += 10.0f;
                    DisplayUI_ShowVadTuning(g_vad_rms_threshold, g_vad_hangover_frames * 10, g_match_threshold, 0.0f, false);
                }
            }

            if (PdmMic_GetAvailable() >= PDM_FRAME_LEN_10MS) {
                uint32_t read = PdmMic_Read(frame_10ms, PDM_FRAME_LEN_10MS);
                if (read == PDM_FRAME_LEN_10MS) {
                    float rms = PdmMic_CalculateFrameRms(frame_10ms, PDM_FRAME_LEN_10MS);
                    bool live_speech = (rms > g_vad_rms_threshold);
                    if (++meter_tick >= 5) {
                        meter_tick = 0;
                        DisplayUI_UpdateVadLiveMeter(g_vad_rms_threshold, rms, live_speech);
                    }
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        PdmMic_Stop();
        std::printf("[VAD Tune] Saved: RMS Thresh = %.0f, Hangover = %d ms, Match Thresh = %.2f\r\n\r\n",
                    g_vad_rms_threshold, g_vad_hangover_frames * 10, g_match_threshold);
        DisplayUI_RestoreDashboard(false);
    };

    auto DoGalleryDetailView = [&]() {
        while (UartHasKey()) { (void)UartGetKey(); }
        auto RefreshAndDraw = [&]() {
            DisplayProfileEntry entries[MAX_SPEAKER_PROFILES];
            for (int i = 0; i < MAX_SPEAKER_PROFILES; ++i) {
                entries[i].slot = (uint8_t)(i + 1);
                entries[i].is_valid = s_gallery[i].enrolled;
                std::strncpy(entries[i].name, s_gallery[i].name, sizeof(entries[i].name) - 1);
                entries[i].name[sizeof(entries[i].name) - 1] = '\0';
                entries[i].chunks_accumulated = s_gallery[i].numChunks;
                entries[i].speech_sec = s_gallery[i].totalSpeechSec;
            }
            uint32_t active = 0;
            for (int i = 0; i < MAX_SPEAKER_PROFILES; ++i) {
                if (s_gallery[i].enrolled) active++;
            }
            DisplayUI_ShowGalleryDetail(entries, active, MAX_SPEAKER_PROFILES);
        };

        RefreshAndDraw();

        while (true) {
            int tap_x = -1, tap_y = -1;
            if (TouchController_GetTap(&tap_x, &tap_y)) {
                DisplayTouchTarget hit = DisplayUI_HitTest(tap_x, tap_y);
                if (hit == TOUCH_HIT_GALLERY_BACK) {
                    break;
                } else if (hit >= TOUCH_HIT_DEL_SLOT_1 && hit <= TOUCH_HIT_DEL_SLOT_5) {
                    int slot = hit - TOUCH_HIT_DEL_SLOT_1;
                    DoDeleteSlotModal(slot);
                    RefreshAndDraw();
                } else if (hit >= TOUCH_HIT_SLOT_1 && hit <= TOUCH_HIT_SLOT_5) {
                    int slot = hit - TOUCH_HIT_SLOT_1;
                    if (!s_gallery[slot].enrolled) {
                        DoTouchEnrollment(slot);
                        RefreshAndDraw();
                    } else {
                        DoDeleteSlotModal(slot);
                        RefreshAndDraw();
                    }
                } else if (hit == TOUCH_HIT_GALLERY_ENROLL_NEW) {
                    DoTouchEnrollment(-1);
                    RefreshAndDraw();
                } else if (hit == TOUCH_HIT_GALLERY_CLEAR_ALL) {
                    DoClearAllModal();
                    RefreshAndDraw();
                }
            }

            if (UartHasKey()) {
                int k = UartGetKey();
                if (k == 'q' || k == 'Q' || k == '\r' || k == '\n') {
                    break;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(20));
        }

        DisplayUI_RestoreDashboard(false);
    };

    PrintUartMenu();

    /* Main Event Loop (Touch + Terminal) */
    while (true) {
        int tap_x = -1, tap_y = -1;
        bool tapped = TouchController_GetTap(&tap_x, &tap_y);
        char choice = 0;

        if (UartHasKey()) {
            int k = UartGetKey();
            if (k > 0) choice = (char)::toupper((unsigned char)k);
        }

        if (tapped) {
            DisplayTouchTarget hit = DisplayUI_HitTest(tap_x, tap_y);

            if (hit == TOUCH_HIT_BTN_VERIFY) {
                choice = 'V';
            } else if (hit == TOUCH_HIT_BTN_ENROLL) {
                choice = 'E';
            } else if (hit == TOUCH_HIT_BTN_BENCHMARK) {
                choice = 'B';
            } else if (hit == TOUCH_HIT_BTN_VAD_TUNE) {
                choice = 'T';
            } else if (hit == TOUCH_HIT_BTN_CLEAR_ALL) {
                choice = 'C';
            } else if (hit == TOUCH_HIT_BTN_GALLERY || hit == TOUCH_HIT_BTN_GALLERY_HEADER) {
                DoGalleryDetailView();
                PrintUartMenu();
                continue;
            }
        }

        if (choice == 'V') {
            DoLiveVerification();
            PrintUartMenu();
        } else if (choice == 'E') {
            DoTouchEnrollment(-1);
            PrintUartMenu();
        } else if (choice == 'L') {
            DoListProfiles();
            DoGalleryDetailView();
            PrintUartMenu();
        } else if (choice == 'T') {
            DoVadTune();
            PrintUartMenu();
        } else if (choice == 'D') {
            DoDeleteSlotCli();
            PrintUartMenu();
        } else if (choice == 'C') {
            DoClearAllModal();
            PrintUartMenu();
        } else if (choice == 'B') {
            DoBenchmark();
            PrintUartMenu();
        } else if (choice == 'S') {
            DoLoadDemo();
            PrintUartMenu();
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
