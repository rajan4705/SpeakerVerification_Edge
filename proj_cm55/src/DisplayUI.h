/*******************************************************************************
* File Name: DisplayUI.h
* Description: High-performance Touch UI rendering module for Waveshare 4.3"
*              display on Infineon PSOC Edge E84 (800x480 RGB565).
********************************************************************************/

#ifndef DISPLAY_UI_H
#define DISPLAY_UI_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard RGB565 Color Definitions */
#define COLOR_BLACK         0x0000
#define COLOR_WHITE         0xFFFF
#define COLOR_DARK_BG       0x0842   /* Dark navy/slate #0B1120 */
#define COLOR_CARD_BG       0x18E4   /* Charcoal card #1E293B */
#define COLOR_CARD_BORDER   0x31E7   /* Muted border #334155 */
#define COLOR_ACCENT_BLUE   0x1D9F   /* Bright blue #38BDF8 */
#define COLOR_GREEN_MATCH   0x1688   /* Emerald #10B981 */
#define COLOR_GREEN_DARK    0x0B44   /* Dark Forest Green */
#define COLOR_RED_REJECT    0xE9A6   /* Coral Red #EF4444 */
#define COLOR_RED_DARK      0x8800   /* Dark Wine Red */
#define COLOR_AMBER_WARN    0xFD40   /* Amber #F59E0B */
#define COLOR_PURPLE_ENROLL 0x923C   /* Purple #A855F7 */
#define COLOR_PURPLE_DARK   0x510E   /* Dark Violet */
#define COLOR_TEXT_MUTED    0x7BEF   /* Slate #94A3B8 */
#define COLOR_BAR_BG        0x0862   /* Track background */
#define COLOR_BTN_SLATE     0x2969   /* Slate button fill */

typedef struct {
    uint8_t slot;
    bool is_valid;
    char name[32];
    uint32_t chunks_accumulated;
    float speech_sec;
} DisplayProfileEntry;

typedef enum {
    TOUCH_HIT_NONE = 0,
    TOUCH_HIT_BTN_VERIFY,
    TOUCH_HIT_BTN_ENROLL,
    TOUCH_HIT_BTN_GALLERY,
    TOUCH_HIT_BTN_BENCHMARK,
    TOUCH_HIT_BTN_LOAD_DEMO,
    TOUCH_HIT_BTN_CLEAR_ALL,
    TOUCH_HIT_BTN_VAD_TUNE,
    TOUCH_HIT_BTN_GALLERY_HEADER,
    TOUCH_HIT_SLOT_1,
    TOUCH_HIT_SLOT_2,
    TOUCH_HIT_SLOT_3,
    TOUCH_HIT_SLOT_4,
    TOUCH_HIT_SLOT_5,
    TOUCH_HIT_DEL_SLOT_1,
    TOUCH_HIT_DEL_SLOT_2,
    TOUCH_HIT_DEL_SLOT_3,
    TOUCH_HIT_DEL_SLOT_4,
    TOUCH_HIT_DEL_SLOT_5,
    /* Enrollment modal buttons */
    TOUCH_HIT_ENROLL_START_SAVE,
    TOUCH_HIT_ENROLL_CANCEL,
    TOUCH_HIT_ENROLL_NAME_0,
    TOUCH_HIT_ENROLL_NAME_1,
    TOUCH_HIT_ENROLL_NAME_2,
    TOUCH_HIT_ENROLL_NAME_3,
    TOUCH_HIT_ENROLL_NAME_4,
    /* Modal action buttons */
    TOUCH_HIT_MODAL_CONFIRM,
    TOUCH_HIT_MODAL_CANCEL,
    /* VAD Tuning buttons */
    TOUCH_HIT_VAD_SENS_60,
    TOUCH_HIT_VAD_SENS_100,
    TOUCH_HIT_VAD_SENS_150,
    TOUCH_HIT_VAD_SENS_220,
    TOUCH_HIT_VAD_THRESH_MINUS,
    TOUCH_HIT_VAD_THRESH_PLUS,
    TOUCH_HIT_VAD_HANGOVER_MINUS,
    TOUCH_HIT_VAD_HANGOVER_PLUS,
    TOUCH_HIT_VAD_MATCH_MINUS,
    TOUCH_HIT_VAD_MATCH_PLUS,
    TOUCH_HIT_VAD_BACK,
    /* Gallery Detail view buttons */
    TOUCH_HIT_GALLERY_BACK,
    TOUCH_HIT_GALLERY_ENROLL_NEW,
    TOUCH_HIT_GALLERY_CLEAR_ALL,
} DisplayTouchTarget;

typedef enum {
    UI_VIEW_DASHBOARD = 0,
    UI_VIEW_ENROLL_STUDIO,
    UI_VIEW_DELETE_CONFIRM,
    UI_VIEW_CLEAR_CONFIRM,
    UI_VIEW_BENCHMARK,
    UI_VIEW_GALLERY_DETAIL,
    UI_VIEW_VAD_TUNE,
} DisplayUIView;

/* Initialize GFXSS, MIPI-DSI, I2C, FT5406 touch, and render base dashboard */
bool DisplayUI_Init(void);

/* Get current active UI view */
DisplayUIView DisplayUI_GetCurrentView(void);

/* Hit test a screen touch point (x, y) against active UI touch controls */
DisplayTouchTarget DisplayUI_HitTest(int x, int y);

/* Render main dashboard layout */
void DisplayUI_DrawDashboard(bool is_verifying);

/* Update Verify Button appearance (Green 'START VERIFY' vs Red 'STOP VERIFY') */
void DisplayUI_SetVerifyingState(bool is_verifying);

/* Update top-level operational state and message */
void DisplayUI_SetState(const char* state_title, uint16_t state_color, const char* state_desc);

/* Update live audio VU meter and time counter */
void DisplayUI_UpdateAudioLevel(float rms, bool speech_detected, float duration_sec);

/* Display Match result card */
void DisplayUI_ShowMatch(const char* name, float score, float threshold, float confidence_pct, float latency_ms);

/* Display Unknown / Rejected card */
void DisplayUI_ShowReject(float best_score, float threshold, float latency_ms);

/* Update live listening / speech indicator inside status card */
void DisplayUI_SetListeningStatus(bool is_listening, bool speech_active);

/* Update Gallery Profile cards */
void DisplayUI_UpdateGallery(const DisplayProfileEntry* profiles, uint32_t active_count, uint32_t total_slots);

/* Show Full Gallery Detail Screen */
void DisplayUI_ShowGalleryDetail(const DisplayProfileEntry* profiles, uint32_t active_count, uint32_t total_slots);

/* Show Interactive VAD & Verification Tuning Screen */
void DisplayUI_ShowVadTuning(float rms_thresh, int hangover_ms, float match_thresh, float live_rms, bool live_speech);

/* Live update for VAD screen audio meter */
void DisplayUI_UpdateVadLiveMeter(float rms_thresh, float live_rms, bool live_speech);

/* Show Touch Voice Enrollment Studio */
void DisplayUI_ShowEnrollmentStudio(int slot, const char* name, int name_idx,
                                    bool is_recording, float spoken_sec, int chunks_count);

/* Show Delete Confirmation modal */
void DisplayUI_ShowDeleteConfirm(int slot, const char* name, float speech_sec);

/* Show Clear All Confirmation modal */
void DisplayUI_ShowClearConfirm(void);

/* Show Benchmark Modal / Screen */
void DisplayUI_ShowBenchmarkScreen(bool is_running, int test_num, int total_tests,
                                   const char* line1, const char* line2, const char* line3);

/* Close any modal and restore the main dashboard */
void DisplayUI_RestoreDashboard(bool is_verifying);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_UI_H */
