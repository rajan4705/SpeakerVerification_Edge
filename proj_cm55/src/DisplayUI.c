/*******************************************************************************
* File Name: DisplayUI.c
* Description: Touch-optimized UI implementation for Waveshare 4.3" MIPI-DSI
*              display (800x480) on Infineon PSOC Edge E84.
********************************************************************************/

#include "DisplayUI.h"
#include "TouchController.h"
#include "cy_pdl.h"
#include "cycfg.h"
#include "cycfg_peripherals.h"
#include "mtb_disp_dsi_waveshare_4p3.h"
#include <stdio.h>
#include <string.h>

#define DISP_STRIDE       832
#define DISP_HEIGHT       480
#define VISIBLE_WIDTH     800
#define VISIBLE_HEIGHT    480

/* Framebuffer placed in gfx_mem region (0x2633C000) */
static uint16_t s_disp_fb[DISP_STRIDE * DISP_HEIGHT] __attribute__((section(".cy_gpu_buf"), aligned(128)));

static cy_stc_gfx_context_t s_gfx_context;
static cy_stc_scb_i2c_context_t s_i2c_context;

static DisplayUIView s_cur_view = UI_VIEW_DASHBOARD;
static bool s_is_verifying = false;
static DisplayProfileEntry s_gallery_cache[5];
static uint32_t s_gallery_active_count = 0;
static uint32_t s_gallery_total_slots = 5;

/* Modal state tracking */
static int s_delete_slot = -1;
static int s_enroll_slot = 0;
static int s_enroll_name_idx = 0;

static const char* s_preset_names[] = {
    "Rajan", "Saksham", "Alex", "User 1", "Guest"
};

/* Interrupt Handlers */
static void disp_dc_isr(void)
{
    Cy_GFXSS_Clear_DC_Interrupt(GFXSS, &s_gfx_context);
}

static void disp_gpu_isr(void)
{
    Cy_GFXSS_Clear_GPU_Interrupt(GFXSS, &s_gfx_context);
}

static void disp_i2c_isr(void)
{
    Cy_SCB_I2C_Interrupt(CYBSP_I2C_CONTROLLER_HW, &s_i2c_context);
}

static inline void DisplayUI_Flush(void)
{
    SCB_CleanDCache();
    Cy_GFXSS_Set_FrameBuffer((GFXSS_Type*)GFXSS, (uint32_t*)s_disp_fb, &s_gfx_context);
}

/*******************************************************************************
* Standard 8x16 ASCII Font (Chars 32 ' ' through 126 '~')
********************************************************************************/
static const uint8_t s_font8x16[95][16] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x00,0x00,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00}, /* '!' */
    {0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* '"' */
    {0x00,0x00,0x6C,0x6C,0xFE,0x6C,0x6C,0x6C,0xFE,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00}, /* '#' */
    {0x18,0x18,0x7C,0xC6,0xC2,0xC0,0x7C,0x06,0x06,0x86,0xC6,0x7C,0x18,0x18,0x00,0x00}, /* '$' */
    {0x00,0x00,0x00,0xC2,0xC6,0x0C,0x18,0x30,0x60,0xC0,0xC6,0x86,0x00,0x00,0x00,0x00}, /* '%' */
    {0x00,0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00}, /* '&' */
    {0x00,0x30,0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ''' */
    {0x00,0x0C,0x18,0x30,0x30,0x60,0x60,0x60,0x60,0x30,0x30,0x18,0x0C,0x00,0x00,0x00}, /* '(' */
    {0x00,0x30,0x18,0x0C,0x0C,0x06,0x06,0x06,0x06,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00}, /* ')' */
    {0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* '*' */
    {0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* '+' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00}, /* ',' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* '-' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00}, /* '.' */
    {0x00,0x00,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00,0x00,0x00,0x00}, /* '/' */
    {0x00,0x00,0x3C,0x66,0xC6,0xCE,0xDE,0xF6,0xE6,0xC6,0x66,0x3C,0x00,0x00,0x00,0x00}, /* '0' */
    {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00}, /* '1' */
    {0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0x00,0x00,0x00,0x00}, /* '2' */
    {0x00,0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00}, /* '3' */
    {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00}, /* '4' */
    {0x00,0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00}, /* '5' */
    {0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, /* '6' */
    {0x00,0x00,0xFE,0xC6,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00}, /* '7' */
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, /* '8' */
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x0C,0x78,0x00,0x00,0x00,0x00}, /* '9' */
    {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00}, /* ':' */
    {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00}, /* ';' */
    {0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00,0x00}, /* '<' */
    {0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* '=' */
    {0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00,0x00}, /* '>' */
    {0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00}, /* '?' */
    {0x00,0x00,0x7C,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0x60,0x3C,0x00,0x00,0x00,0x00}, /* '@' */
    {0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00}, /* 'A' */
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00}, /* 'B' */
    {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x00,0x00,0x00,0x00}, /* 'C' */
    {0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00}, /* 'D' */
    {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00}, /* 'E' */
    {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00}, /* 'F' */
    {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0x00,0x00,0x00,0x00}, /* 'G' */
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00}, /* 'H' */
    {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00}, /* 'I' */
    {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00}, /* 'J' */
    {0x00,0x00,0xE6,0x66,0x66,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0x00,0x00,0x00,0x00}, /* 'K' */
    {0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00}, /* 'L' */
    {0x00,0x00,0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00}, /* 'M' */
    {0x00,0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00}, /* 'N' */
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, /* 'O' */
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00}, /* 'P' */
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0x00,0x00}, /* 'Q' */
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00}, /* 'R' */
    {0x00,0x00,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0x06,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, /* 'S' */
    {0x00,0x00,0x7E,0x5A,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00}, /* 'T' */
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, /* 'U' */
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00,0x00,0x00,0x00}, /* 'V' */
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xD6,0xD6,0xFE,0xEE,0x6C,0x28,0x00,0x00,0x00,0x00}, /* 'W' */
    {0x00,0x00,0xC6,0xC6,0x6C,0x38,0x10,0x38,0x6C,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00}, /* 'X' */
    {0x00,0x00,0x66,0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00}, /* 'Y' */
    {0x00,0x00,0xFE,0xC6,0x8C,0x18,0x30,0x60,0xC2,0xC6,0xFE,0x00,0x00,0x00,0x00,0x00}, /* 'Z' */
    {0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00,0x00}, /* '[' */
    {0x00,0x00,0x80,0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00,0x00,0x00,0x00,0x00,0x00}, /* '\' */
    {0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00,0x00}, /* ']' */
    {0x00,0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* '^' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0x00}, /* '_' */
    {0x00,0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* '`' */
    {0x00,0x00,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00,0x00}, /* 'a' */
    {0x00,0x00,0xE0,0x60,0x7C,0x66,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00,0x00}, /* 'b' */
    {0x00,0x00,0x00,0x00,0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00,0x00,0x00,0x00,0x00}, /* 'c' */
    {0x00,0x00,0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00,0x00}, /* 'd' */
    {0x00,0x00,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC0,0x66,0x3C,0x00,0x00,0x00,0x00,0x00}, /* 'e' */
    {0x00,0x00,0x38,0x6C,0x64,0x60,0xF0,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00,0x00}, /* 'f' */
    {0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0x00,0x00,0x00,0x00}, /* 'g' */
    {0x00,0x00,0xE0,0x60,0x6C,0x76,0x66,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00,0x00}, /* 'h' */
    {0x00,0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00}, /* 'i' */
    {0x00,0x00,0x0C,0x0C,0x00,0x1C,0x0C,0x0C,0x0C,0x0C,0xCC,0x78,0x00,0x00,0x00,0x00}, /* 'j' */
    {0x00,0x00,0xE0,0x60,0x66,0x6C,0x78,0x78,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00,0x00}, /* 'k' */
    {0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00}, /* 'l' */
    {0x00,0x00,0x00,0x00,0xFE,0xD6,0xD6,0xD6,0xD6,0xD6,0xD6,0x00,0x00,0x00,0x00,0x00}, /* 'm' */
    {0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00}, /* 'n' */
    {0x00,0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, /* 'o' */
    {0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x7C,0x60,0xF0,0x00,0x00,0x00,0x00,0x00}, /* 'p' */
    {0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0x7C,0x0C,0x1E,0x00,0x00,0x00,0x00,0x00}, /* 'q' */
    {0x00,0x00,0x00,0x00,0xDC,0x76,0x66,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00,0x00}, /* 'r' */
    {0x00,0x00,0x00,0x00,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, /* 's' */
    {0x00,0x00,0x10,0x30,0xFC,0x30,0x30,0x30,0x30,0x36,0x1C,0x00,0x00,0x00,0x00,0x00}, /* 't' */
    {0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00,0x00}, /* 'u' */
    {0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00,0x00,0x00,0x00,0x00}, /* 'v' */
    {0x00,0x00,0x00,0x00,0xC6,0xD6,0xD6,0xFE,0xEE,0x6C,0x28,0x00,0x00,0x00,0x00,0x00}, /* 'w' */
    {0x00,0x00,0x00,0x00,0xC6,0x6C,0x38,0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00}, /* 'x' */
    {0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0xF8,0x00,0x00,0x00,0x00}, /* 'y' */
    {0x00,0x00,0x00,0x00,0xFE,0x8C,0x18,0x30,0x60,0xC2,0xFE,0x00,0x00,0x00,0x00,0x00}, /* 'z' */
    {0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x0E,0x00,0x00,0x00,0x00,0x00,0x00}, /* '{' */
    {0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00}, /* '|' */
    {0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00,0x00,0x00}, /* '}' */
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}  /* '~' */
};

/*******************************************************************************
* 2D Drawing Primitives
********************************************************************************/
static inline void DrawPixel(int x, int y, uint16_t color)
{
    if (x >= 0 && x < VISIBLE_WIDTH && y >= 0 && y < VISIBLE_HEIGHT) {
        s_disp_fb[y * DISP_STRIDE + x] = color;
    }
}

static void FillRect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > VISIBLE_WIDTH) w = VISIBLE_WIDTH - x;
    if (y + h > VISIBLE_HEIGHT) h = VISIBLE_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    for (int r = y; r < y + h; r++) {
        uint16_t *row = &s_disp_fb[r * DISP_STRIDE + x];
        for (int c = 0; c < w; c++) {
            row[c] = color;
        }
    }
}

static void DrawRect(int x, int y, int w, int h, uint16_t color)
{
    FillRect(x, y, w, 1, color);
    FillRect(x, y + h - 1, w, 1, color);
    FillRect(x, y, 1, h, color);
    FillRect(x + w - 1, y, 1, h, color);
}

static void DrawRoundedCard(int x, int y, int w, int h, uint16_t fill_color, uint16_t border_color)
{
    FillRect(x + 2, y, w - 4, h, fill_color);
    FillRect(x, y + 2, w, h - 4, fill_color);

    FillRect(x + 2, y, w - 4, 1, border_color);
    FillRect(x + 2, y + h - 1, w - 4, 1, border_color);
    FillRect(x, y + 2, 1, h - 4, border_color);
    FillRect(x + w - 1, y + 2, 1, h - 4, border_color);

    DrawPixel(x + 1, y + 1, border_color);
    DrawPixel(x + w - 2, y + 1, border_color);
    DrawPixel(x + 1, y + h - 2, border_color);
    DrawPixel(x + w - 2, y + h - 2, border_color);
}

/* Tactile touch button with drop shadow / inner contrast */
static void DrawTouchButton(int x, int y, int w, int h, uint16_t fill_color, uint16_t border_color,
                            const char* text, uint16_t text_color, int scale)
{
    DrawRoundedCard(x, y, w, h, fill_color, border_color);

    if (text) {
        int text_len = (int)strlen(text);
        int text_px_w = text_len * 8 * scale;
        int text_px_h = 16 * scale;
        int tx = x + (w - text_px_w) / 2;
        int ty = y + (h - text_px_h) / 2;
        if (tx < x + 4) tx = x + 4;

        /* Draw button string centered */
        int cur_x = tx;
        while (*text) {
            char c = *text++;
            if (c < 32 || c > 126) c = ' ';
            const uint8_t *bitmap = s_font8x16[c - 32];
            for (int row = 0; row < 16; row++) {
                uint8_t bits = bitmap[row];
                for (int col = 0; col < 8; col++) {
                    if (bits & (0x80 >> col)) {
                        if (scale == 1) {
                            DrawPixel(cur_x + col, ty + row, text_color);
                        } else {
                            FillRect(cur_x + col * scale, ty + row * scale, scale, scale, text_color);
                        }
                    }
                }
            }
            cur_x += 8 * scale;
        }
    }
}

static void DrawChar(int x, int y, char c, uint16_t fg, uint16_t bg, int scale)
{
    if (c < 32 || c > 126) c = ' ';
    const uint8_t *bitmap = s_font8x16[c - 32];

    for (int row = 0; row < 16; row++) {
        uint8_t bits = bitmap[row];
        for (int col = 0; col < 8; col++) {
            bool on = (bits & (0x80 >> col)) != 0;
            uint16_t color = on ? fg : bg;
            if (bg != 0 || on) {
                if (scale == 1) {
                    DrawPixel(x + col, y + row, color);
                } else {
                    FillRect(x + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
    }
}

static void DrawString(int x, int y, const char *str, uint16_t fg, uint16_t bg, int scale)
{
    if (!str) return;
    int cur_x = x;
    while (*str) {
        DrawChar(cur_x, y, *str, fg, bg, scale);
        cur_x += 8 * scale;
        str++;
    }
}

/*******************************************************************************
* High-Level Layout Renderers
********************************************************************************/
static void DrawHeader(void)
{
    FillRect(0, 0, VISIBLE_WIDTH, 46, COLOR_DARK_BG);
    FillRect(0, 45, VISIBLE_WIDTH, 2, COLOR_CARD_BORDER);

    /* Professional Title */
    DrawString(16, 6, "SPEAKER VERIFICATION DEMO", COLOR_WHITE, 0, 2);
    DrawString(16, 26, "Real-Time Neural Biometrics  |  Edge AI On-Device Inference", COLOR_TEXT_MUTED, 0, 1);

    /* Clean Status Badge on Top Right */
    DrawRoundedCard(644, 8, 140, 30, 0x1143, COLOR_GREEN_MATCH);
    DrawString(666, 15, "SYSTEM READY", COLOR_GREEN_MATCH, 0, 1);
}

static void DrawControlButtons(bool is_verifying)
{
    /* Clear Action button region (x=16..784, y=326..470) */
    FillRect(16, 326, 768, 144, COLOR_DARK_BG);

    /* Row 1: Primary Actions (y = 326..394, h = 68) */
    if (!is_verifying) {
        DrawTouchButton(16, 326, 246, 68, COLOR_GREEN_MATCH, COLOR_WHITE,
                        "> START VERIFY", COLOR_WHITE, 2);
    } else {
        DrawTouchButton(16, 326, 246, 68, COLOR_RED_REJECT, COLOR_WHITE,
                        "[] STOP VERIFY", COLOR_WHITE, 2);
    }

    DrawTouchButton(276, 326, 246, 68, COLOR_PURPLE_ENROLL, COLOR_WHITE,
                    "+ ENROLL VOICE", COLOR_WHITE, 2);

    DrawTouchButton(536, 326, 248, 68, COLOR_BTN_SLATE, COLOR_ACCENT_BLUE,
                    "VOICE GALLERY", COLOR_ACCENT_BLUE, 2);

    /* Row 2: Secondary / Utility Actions (y = 404..466, h = 62) */
    DrawTouchButton(16, 404, 246, 62, COLOR_BTN_SLATE, COLOR_AMBER_WARN,
                    "VAD TUNE", COLOR_AMBER_WARN, 1);

    DrawTouchButton(276, 404, 246, 62, COLOR_BTN_SLATE, COLOR_CARD_BORDER,
                    "BENCHMARK", COLOR_WHITE, 1);

    DrawTouchButton(536, 404, 248, 62, COLOR_CARD_BG, COLOR_RED_REJECT,
                    "CLEAR ALL", COLOR_RED_REJECT, 1);
}

void DisplayUI_DrawDashboard(bool is_verifying)
{
    s_cur_view = UI_VIEW_DASHBOARD;
    s_is_verifying = is_verifying;

    FillRect(0, 0, VISIBLE_WIDTH, VISIBLE_HEIGHT, COLOR_DARK_BG);

    DrawHeader();

    /* Full-Width Main Status Card (x=16..784, w=768, y=52..242, h=190) */
    DrawRoundedCard(16, 52, 768, 190, COLOR_CARD_BG, COLOR_CARD_BORDER);

    /* Initial state badge */
    if (!is_verifying) {
        DrawRoundedCard(32, 64, 200, 36, COLOR_BTN_SLATE, COLOR_ACCENT_BLUE);
        DrawString(44, 74, "STANDBY", COLOR_ACCENT_BLUE, 0, 1);
        DrawString(250, 74, "Ready for Voice Verification | Tap START VERIFY", COLOR_TEXT_MUTED, 0, 1);

        DrawString(32, 112, "SPEAKER: ---", COLOR_TEXT_MUTED, 0, 2);
    } else {
        DrawRoundedCard(32, 64, 200, 36, COLOR_BTN_SLATE, COLOR_AMBER_WARN);
        DrawString(44, 74, "LISTENING...", COLOR_AMBER_WARN, 0, 1);
        DrawString(250, 74, "Awaiting Voice Input... Speak into the microphone", COLOR_AMBER_WARN, 0, 1);

        DrawString(32, 112, "SPEAKER: (Awaiting Voice...)", COLOR_TEXT_MUTED, 0, 2);
    }

    DrawString(32, 152, "Inference Latency: 341.6 ms (Ethos-U55 NPU: 111.8ms | Fast ASP Head: 188.6ms)", COLOR_TEXT_MUTED, 0, 1);
    DrawString(32, 172, "Continuous Engine: 500 ms hop (2.0 Hz sliding window live refresh)", COLOR_TEXT_MUTED, 0, 1);

    char gal_buf[80];
    snprintf(gal_buf, sizeof(gal_buf), "Gallery Storage:   %lu / %lu Profiles in RRAM (Tap 'VOICE GALLERY' to view)",
             (unsigned long)s_gallery_active_count, (unsigned long)s_gallery_total_slots);
    DrawString(32, 192, gal_buf, COLOR_ACCENT_BLUE, 0, 1);

    /* Bottom-Right Status Badge inside card */
    if (is_verifying) {
        DrawRoundedCard(592, 64, 176, 36, COLOR_CARD_BG, COLOR_AMBER_WARN);
        DrawString(608, 74, "[LISTENING...]", COLOR_AMBER_WARN, 0, 1);
    } else {
        DrawRoundedCard(592, 64, 176, 36, COLOR_CARD_BG, COLOR_CARD_BORDER);
        DrawString(624, 74, "[IDLE]", COLOR_TEXT_MUTED, 0, 1);
    }

    /* Full-Width VU Meter Card (x=16..784, w=768, y=248..316, h=68) */
    DrawRoundedCard(16, 248, 768, 68, COLOR_CARD_BG, COLOR_CARD_BORDER);
    DrawString(32, 254, "MIC AUDIO ACTIVITY", COLOR_TEXT_MUTED, 0, 1);
    FillRect(32, 274, 736, 26, COLOR_BAR_BG);
    DrawRect(31, 273, 738, 28, COLOR_CARD_BORDER);

    /* Action Buttons (x=16..784, w=768, y=326..468) */
    DrawControlButtons(is_verifying);

    DisplayUI_Flush();
}

void DisplayUI_SetVerifyingState(bool is_verifying)
{
    s_is_verifying = is_verifying;
    if (s_cur_view == UI_VIEW_DASHBOARD) {
        DrawControlButtons(is_verifying);

        if (!is_verifying) {
            FillRect(20, 54, 760, 184, COLOR_CARD_BG);
            DrawRoundedCard(32, 64, 200, 36, COLOR_BTN_SLATE, COLOR_ACCENT_BLUE);
            DrawString(44, 74, "STANDBY", COLOR_ACCENT_BLUE, 0, 1);
            DrawString(250, 74, "Ready for Voice Verification | Tap START VERIFY", COLOR_TEXT_MUTED, 0, 1);

            DrawString(32, 112, "SPEAKER: ---", COLOR_TEXT_MUTED, 0, 2);

            DrawString(32, 152, "Inference Latency: 341.6 ms (Ethos-U55 NPU: 111.8ms | Fast ASP Head: 188.6ms)", COLOR_TEXT_MUTED, 0, 1);
            DrawString(32, 172, "Continuous Engine: 500 ms hop (2.0 Hz sliding window live refresh)", COLOR_TEXT_MUTED, 0, 1);

            char gal_buf[80];
            snprintf(gal_buf, sizeof(gal_buf), "Gallery Storage:   %lu / %lu Profiles in RRAM (Tap 'VOICE GALLERY' to view)",
                     (unsigned long)s_gallery_active_count, (unsigned long)s_gallery_total_slots);
            DrawString(32, 192, gal_buf, COLOR_ACCENT_BLUE, 0, 1);

            DrawRoundedCard(592, 64, 176, 36, COLOR_CARD_BG, COLOR_CARD_BORDER);
            DrawString(624, 74, "[IDLE]", COLOR_TEXT_MUTED, 0, 1);
        } else {
            DrawRoundedCard(592, 64, 176, 36, COLOR_CARD_BG, COLOR_AMBER_WARN);
            DrawString(608, 74, "[LISTENING...]", COLOR_AMBER_WARN, 0, 1);
        }
        DisplayUI_Flush();
    }
}

DisplayUIView DisplayUI_GetCurrentView(void)
{
    return s_cur_view;
}

void DisplayUI_RestoreDashboard(bool is_verifying)
{
    DisplayUI_DrawDashboard(is_verifying);
}

/*******************************************************************************
* Operational State and Match Verdicts
********************************************************************************/
void DisplayUI_SetState(const char* state_title, uint16_t state_color, const char* state_desc)
{
    if (s_cur_view != UI_VIEW_DASHBOARD) return;

    FillRect(20, 54, 760, 184, COLOR_CARD_BG);

    DrawRoundedCard(32, 64, 220, 38, state_color, COLOR_WHITE);
    DrawString(44, 74, state_title, COLOR_WHITE, 0, 1);

    if (state_desc) {
        DrawString(32, 116, state_desc, COLOR_WHITE, 0, 2);
    }
    DisplayUI_Flush();
}

void DisplayUI_ShowMatch(const char* name, float score, float threshold, float confidence_pct, float latency_ms)
{
    if (s_cur_view != UI_VIEW_DASHBOARD) return;

    FillRect(20, 54, 760, 184, COLOR_CARD_BG);

    /* Match Verdict Badge */
    DrawRoundedCard(32, 64, 200, 36, COLOR_GREEN_MATCH, COLOR_WHITE);
    DrawString(44, 74, "MATCH VERIFIED!", COLOR_WHITE, 0, 1);

    /* Similarity and Confidence Score */
    char score_buf[64];
    snprintf(score_buf, sizeof(score_buf), "Sim: %.2f (%.0f%%) | Match Threshold: %.2f", score, confidence_pct, threshold);
    DrawString(250, 74, score_buf, COLOR_GREEN_MATCH, 0, 1);

    /* Bottom-Right Status Badge */
    DrawRoundedCard(592, 64, 176, 36, 0x1143, COLOR_GREEN_MATCH);
    DrawString(608, 74, "[VOICE ACTIVE]", COLOR_GREEN_MATCH, 0, 1);

    /* Speaker Name in large scale 2 font (HELD ON SCREEN) */
    char name_buf[64];
    snprintf(name_buf, sizeof(name_buf), "SPEAKER: %s", name);
    DrawString(32, 112, name_buf, COLOR_WHITE, 0, 2);

    /* Latency and Pipeline breakdown */
    char stat_buf[96];
    snprintf(stat_buf, sizeof(stat_buf), "Inference Latency: %.1f ms  (Ethos-U55 NPU: 112ms | Fast ASP Head: 188ms)", latency_ms);
    DrawString(32, 152, stat_buf, COLOR_TEXT_MUTED, 0, 1);

    snprintf(stat_buf, sizeof(stat_buf), "Continuous Engine: 500 ms hop (2.0 Hz sliding window live refresh)");
    DrawString(32, 172, stat_buf, COLOR_ACCENT_BLUE, 0, 1);

    char gal_buf[80];
    snprintf(gal_buf, sizeof(gal_buf), "Gallery Storage:   %lu / %lu Profiles in RRAM (Tap 'VOICE GALLERY' to view)",
             (unsigned long)s_gallery_active_count, (unsigned long)s_gallery_total_slots);
    DrawString(32, 192, gal_buf, COLOR_TEXT_MUTED, 0, 1);

    DisplayUI_Flush();
}

void DisplayUI_ShowReject(float best_score, float threshold, float latency_ms)
{
    if (s_cur_view != UI_VIEW_DASHBOARD) return;

    FillRect(20, 54, 760, 184, COLOR_CARD_BG);

    DrawRoundedCard(32, 64, 200, 36, COLOR_RED_REJECT, COLOR_WHITE);
    DrawString(44, 74, "UNKNOWN SPEAKER", COLOR_WHITE, 0, 1);

    char score_buf[64];
    snprintf(score_buf, sizeof(score_buf), "Sim: %.2f < %.2f | ACCESS DENIED", best_score, threshold);
    DrawString(250, 74, score_buf, COLOR_RED_REJECT, 0, 1);

    /* Bottom-Right status badge */
    DrawRoundedCard(592, 64, 176, 36, COLOR_CARD_BG, COLOR_AMBER_WARN);
    DrawString(608, 74, "[LISTENING...]", COLOR_AMBER_WARN, 0, 1);

    /* Access Denied / Unknown in large scale 2 font */
    DrawString(32, 112, "ACCESS DENIED (Unknown Voice)", COLOR_RED_REJECT, 0, 2);

    char stat_buf[96];
    snprintf(stat_buf, sizeof(stat_buf), "Inference Latency: %.1f ms  (Ethos-U55 NPU: 112ms | Fast ASP Head: 188ms)", latency_ms);
    DrawString(32, 152, stat_buf, COLOR_TEXT_MUTED, 0, 1);

    DrawString(32, 172, "Speaker voice did not match any enrolled profiles in gallery.", COLOR_TEXT_MUTED, 0, 1);

    char gal_buf[80];
    snprintf(gal_buf, sizeof(gal_buf), "Gallery Storage:   %lu / %lu Profiles in RRAM (Tap 'VOICE GALLERY' to view)",
             (unsigned long)s_gallery_active_count, (unsigned long)s_gallery_total_slots);
    DrawString(32, 192, gal_buf, COLOR_TEXT_MUTED, 0, 1);

    DisplayUI_Flush();
}

void DisplayUI_SetListeningStatus(bool is_listening, bool speech_active)
{
    if (s_cur_view != UI_VIEW_DASHBOARD) return;

    /* Only update the bottom-right badge - preserves speaker name & verdict */
    if (is_listening) {
        if (speech_active) {
            DrawRoundedCard(592, 64, 176, 36, 0x1143, COLOR_GREEN_MATCH);
            DrawString(608, 74, "[VOICE ACTIVE]", COLOR_GREEN_MATCH, 0, 1);
        } else {
            DrawRoundedCard(592, 64, 176, 36, COLOR_CARD_BG, COLOR_AMBER_WARN);
            DrawString(608, 74, "[LISTENING...]", COLOR_AMBER_WARN, 0, 1);
        }
    } else {
        DrawRoundedCard(592, 64, 176, 36, COLOR_CARD_BG, COLOR_CARD_BORDER);
        DrawString(624, 74, "[IDLE]", COLOR_TEXT_MUTED, 0, 1);
    }
    DisplayUI_Flush();
}

void DisplayUI_UpdateAudioLevel(float rms, bool speech_detected, float duration_sec)
{
    if (s_cur_view != UI_VIEW_DASHBOARD && s_cur_view != UI_VIEW_ENROLL_STUDIO) return;

    int bar_x = 32;
    int bar_y = (s_cur_view == UI_VIEW_DASHBOARD) ? 274 : 270;
    int bar_w = (s_cur_view == UI_VIEW_DASHBOARD) ? 736 : 474;
    int bar_h = (s_cur_view == UI_VIEW_DASHBOARD) ? 26 : 20;

    /* Live Voice Activity and Seconds Counter (above VU bar on dashboard) */
    if (s_cur_view == UI_VIEW_DASHBOARD) {
        FillRect(200, 252, 560, 18, COLOR_CARD_BG);
        if (s_is_verifying) {
            char stat[64];
            snprintf(stat, sizeof(stat), "%s  Speech: %4.1fs/3.0s | RMS:%3u",
                     speech_detected ? "[VOICE DETECTED]" : "[SILENCE]", duration_sec, (unsigned int)rms);
            DrawString(200, 254, stat, speech_detected ? COLOR_GREEN_MATCH : COLOR_TEXT_MUTED, 0, 1);

            /* Also update status badge */
            if (speech_detected) {
                DrawRoundedCard(592, 64, 176, 36, 0x1143, COLOR_GREEN_MATCH);
                DrawString(608, 74, "[VOICE ACTIVE]", COLOR_GREEN_MATCH, 0, 1);
            } else {
                DrawRoundedCard(592, 64, 176, 36, COLOR_CARD_BG, COLOR_AMBER_WARN);
                DrawString(608, 74, "[LISTENING...]", COLOR_AMBER_WARN, 0, 1);
            }
        }
    }

    float norm = rms / 400.0f;
    if (norm > 1.0f) norm = 1.0f;
    if (norm < 0.0f) norm = 0.0f;

    int fill_w = (int)(norm * bar_w);

    FillRect(bar_x, bar_y, bar_w, bar_h, COLOR_BAR_BG);

    int seg_green  = (bar_w * 6) / 10;
    int seg_yellow = (bar_w * 8) / 10;

    int cur_fill = (fill_w > seg_green) ? seg_green : fill_w;
    if (cur_fill > 0) {
        FillRect(bar_x, bar_y, cur_fill, bar_h, COLOR_GREEN_MATCH);
    }
    if (fill_w > seg_green) {
        int y_fill = (fill_w > seg_yellow) ? (seg_yellow - seg_green) : (fill_w - seg_green);
        FillRect(bar_x + seg_green, bar_y, y_fill, bar_h, COLOR_AMBER_WARN);
    }
    if (fill_w > seg_yellow) {
        int r_fill = fill_w - seg_yellow;
        FillRect(bar_x + seg_yellow, bar_y, r_fill, bar_h, COLOR_RED_REJECT);
    }

    DisplayUI_Flush();
}

/*******************************************************************************
* Gallery Rendering
********************************************************************************/
void DisplayUI_UpdateGallery(const DisplayProfileEntry* profiles, uint32_t active_count, uint32_t total_slots)
{
    if (profiles) {
        for (uint32_t i = 0; i < 5 && i < total_slots; ++i) {
            s_gallery_cache[i] = profiles[i];
        }
    }
    s_gallery_active_count = active_count;
    s_gallery_total_slots = total_slots;

    if (s_cur_view == UI_VIEW_DASHBOARD) {
        /* Update the gallery count line in the status card */
        FillRect(32, 192, 540, 20, COLOR_CARD_BG);
        char gal_buf[80];
        snprintf(gal_buf, sizeof(gal_buf), "Gallery Storage:   %lu / %lu Profiles in RRAM (Tap 'VOICE GALLERY' to view)",
                 (unsigned long)s_gallery_active_count, (unsigned long)s_gallery_total_slots);
        DrawString(32, 192, gal_buf, COLOR_ACCENT_BLUE, 0, 1);
        DisplayUI_Flush();
    }
}

/*******************************************************************************
* Voice Enrollment Studio
********************************************************************************/
void DisplayUI_ShowEnrollmentStudio(int slot, const char* name, int name_idx,
                                    bool is_recording, float spoken_sec, int chunks_count)
{
    s_cur_view = UI_VIEW_ENROLL_STUDIO;
    s_enroll_slot = slot;
    s_enroll_name_idx = name_idx;

    /* Clear left panel */
    FillRect(16, 50, 498, 420, COLOR_DARK_BG);

    /* Enrollment Studio Card */
    DrawRoundedCard(16, 50, 498, 420, COLOR_CARD_BG, COLOR_CARD_BORDER);

    /* Studio Header */
    char title_buf[64];
    snprintf(title_buf, sizeof(title_buf), "VOICE ENROLLMENT - SLOT [%d]", slot + 1);
    DrawRoundedCard(28, 60, 474, 38, COLOR_PURPLE_ENROLL, COLOR_WHITE);
    DrawString(40, 72, title_buf, COLOR_WHITE, 0, 1);

    /* Quick Name Picker Label */
    DrawString(28, 108, "SELECT SPEAKER NAME:", COLOR_TEXT_MUTED, 0, 1);

    /* 5 Name Preset Pills */
    int pill_x = 28;
    for (int i = 0; i < 5; ++i) {
        uint16_t bg = (i == name_idx) ? COLOR_ACCENT_BLUE : COLOR_BTN_SLATE;
        uint16_t border = (i == name_idx) ? COLOR_WHITE : COLOR_CARD_BORDER;
        uint16_t text_c = (i == name_idx) ? COLOR_BLACK : COLOR_WHITE;

        DrawTouchButton(pill_x, 126, 90, 36, bg, border, s_preset_names[i], text_c, 1);
        pill_x += 96;
    }

    /* Target Name & Status Box */
    DrawRoundedCard(28, 172, 474, 120, 0x08E2, COLOR_CARD_BORDER);

    char name_disp[64];
    snprintf(name_disp, sizeof(name_disp), "Speaker Name:  %s", name);
    DrawString(40, 184, name_disp, COLOR_WHITE, 0, 2);

    char speech_disp[64];
    snprintf(speech_disp, sizeof(speech_disp), "Spoken Speech: %.1f seconds", spoken_sec);
    DrawString(40, 222, speech_disp, COLOR_PURPLE_ENROLL, 0, 1);

    char chunk_disp[64];
    snprintf(chunk_disp, sizeof(chunk_disp), "Speech Chunks: %d accumulated (Centroid: %s)",
             chunks_count, (chunks_count >= 1 ? "READY" : "Awaiting >= 1.5s"));
    DrawString(40, 244, chunk_disp, (chunks_count >= 1 ? COLOR_GREEN_MATCH : COLOR_AMBER_WARN), 0, 1);

    /* Action Buttons */
    if (!is_recording) {
        DrawTouchButton(28, 310, 230, 74, COLOR_GREEN_MATCH, COLOR_WHITE,
                        "> START RECORD", COLOR_WHITE, 1);
        DrawTouchButton(272, 310, 230, 74, COLOR_BTN_SLATE, COLOR_CARD_BORDER,
                        "CANCEL", COLOR_TEXT_MUTED, 1);
        DrawString(40, 400, "Tap START RECORD, then speak naturally into the mic.", COLOR_TEXT_MUTED, 0, 1);
    } else {
        DrawTouchButton(28, 310, 230, 74, COLOR_GREEN_MATCH, COLOR_WHITE,
                        "SAVE PROFILE", COLOR_WHITE, 1);
        DrawTouchButton(272, 310, 230, 74, COLOR_RED_REJECT, COLOR_WHITE,
                        "CANCEL", COLOR_WHITE, 1);
        DrawString(40, 400, "Recording... Tap SAVE when finished speaking.", COLOR_AMBER_WARN, 0, 1);
    }

    DisplayUI_Flush();
}

/*******************************************************************************
* Delete & Clear Confirmation Modals
********************************************************************************/
void DisplayUI_ShowDeleteConfirm(int slot, const char* name, float speech_sec)
{
    s_cur_view = UI_VIEW_DELETE_CONFIRM;
    s_delete_slot = slot;

    /* Center Modal Card */
    DrawRoundedCard(160, 100, 480, 270, 0x0C04, COLOR_RED_REJECT);

    DrawRoundedCard(180, 116, 440, 40, COLOR_RED_REJECT, COLOR_WHITE);
    DrawString(280, 128, "CONFIRM DELETION", COLOR_WHITE, 0, 1);

    char prompt[64];
    snprintf(prompt, sizeof(prompt), "Delete Speaker from Slot [%d]?", slot + 1);
    DrawString(180, 172, prompt, COLOR_WHITE, 0, 2);

    char name_str[64];
    snprintf(name_str, sizeof(name_str), "Speaker: %s  (%.1fs speech)", name, speech_sec);
    DrawString(180, 210, name_str, COLOR_TEXT_MUTED, 0, 1);
    DrawString(180, 230, "Profile will be permanently cleared from RRAM.", COLOR_AMBER_WARN, 0, 1);

    /* Touch buttons */
    DrawTouchButton(180, 270, 210, 68, COLOR_RED_REJECT, COLOR_WHITE,
                    "CONFIRM DELETE", COLOR_WHITE, 1);
    DrawTouchButton(410, 270, 210, 68, COLOR_BTN_SLATE, COLOR_CARD_BORDER,
                    "KEEP PROFILE", COLOR_WHITE, 1);

    DisplayUI_Flush();
}

void DisplayUI_ShowClearConfirm(void)
{
    s_cur_view = UI_VIEW_CLEAR_CONFIRM;

    DrawRoundedCard(160, 100, 480, 270, 0x0C04, COLOR_RED_REJECT);

    DrawRoundedCard(180, 116, 440, 40, COLOR_RED_REJECT, COLOR_WHITE);
    DrawString(260, 128, "CLEAR ENTIRE GALLERY?", COLOR_WHITE, 0, 1);

    DrawString(180, 172, "DELETE ALL PROFILES?", COLOR_WHITE, 0, 2);
    DrawString(180, 210, "All 5 enrolled speaker slots will be erased.", COLOR_TEXT_MUTED, 0, 1);
    DrawString(180, 230, "Non-volatile RRAM memory will be cleared.", COLOR_AMBER_WARN, 0, 1);

    DrawTouchButton(180, 270, 210, 68, COLOR_RED_REJECT, COLOR_WHITE,
                    "CLEAR ALL", COLOR_WHITE, 1);
    DrawTouchButton(410, 270, 210, 68, COLOR_BTN_SLATE, COLOR_CARD_BORDER,
                    "CANCEL", COLOR_WHITE, 1);

    DisplayUI_Flush();
}

void DisplayUI_ShowBenchmarkScreen(bool is_running, int test_num, int total_tests,
                                   const char* line1, const char* line2, const char* line3)
{
    s_cur_view = UI_VIEW_BENCHMARK;

    FillRect(16, 50, 498, 420, COLOR_DARK_BG);
    DrawRoundedCard(16, 50, 498, 420, COLOR_CARD_BG, COLOR_CARD_BORDER);

    DrawRoundedCard(28, 60, 474, 38, COLOR_BTN_SLATE, COLOR_ACCENT_BLUE);
    DrawString(40, 72, "OFFLINE VERIFICATION BENCHMARK", COLOR_WHITE, 0, 1);

    char prog_buf[64];
    snprintf(prog_buf, sizeof(prog_buf), "Progress: %s (Test %d/%d)",
             is_running ? "RUNNING ETHOS-U55..." : "COMPLETED", test_num, total_tests);
    DrawString(28, 116, prog_buf, is_running ? COLOR_AMBER_WARN : COLOR_GREEN_MATCH, 0, 1);

    if (line1) DrawString(28, 150, line1, COLOR_WHITE, 0, 1);
    if (line2) DrawString(28, 180, line2, COLOR_WHITE, 0, 1);
    if (line3) DrawString(28, 210, line3, COLOR_ACCENT_BLUE, 0, 1);

    DrawString(28, 250, "Hardware: Ethos-U55 NPU (256 MACs) + Cortex-M55 Helium", COLOR_TEXT_MUTED, 0, 1);
    DrawString(28, 274, "Dataset: LibriSpeech Test Vectors (16 kHz Mono)", COLOR_TEXT_MUTED, 0, 1);

    if (!is_running) {
        DrawTouchButton(28, 330, 474, 70, COLOR_GREEN_MATCH, COLOR_WHITE,
                        "<- BACK TO DASHBOARD", COLOR_WHITE, 1);
    }

    DisplayUI_Flush();
}

/*******************************************************************************
* Full Gallery Detail Screen
********************************************************************************/
void DisplayUI_ShowGalleryDetail(const DisplayProfileEntry* profiles, uint32_t active_count, uint32_t total_slots)
{
    s_cur_view = UI_VIEW_GALLERY_DETAIL;

    if (profiles) {
        for (uint32_t i = 0; i < 5 && i < total_slots; ++i) {
            s_gallery_cache[i] = profiles[i];
        }
    }
    s_gallery_active_count = active_count;
    s_gallery_total_slots = total_slots;

    FillRect(0, 0, VISIBLE_WIDTH, VISIBLE_HEIGHT, COLOR_DARK_BG);
    DrawHeader();

    /* Main Container Card */
    DrawRoundedCard(16, 50, 768, 420, COLOR_CARD_BG, COLOR_CARD_BORDER);

    /* Header Bar inside card */
    DrawRoundedCard(28, 60, 744, 38, COLOR_BTN_SLATE, COLOR_ACCENT_BLUE);
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "ENROLLED SPEAKER GALLERY - %lu/%lu PROFILES ACTIVE",
             (unsigned long)active_count, (unsigned long)total_slots);
    DrawString(40, 72, hdr, COLOR_WHITE, 0, 1);

    /* 5 Profile rows (y = 106, step = 56) */
    int row_y = 106;
    for (int i = 0; i < 5; ++i) {
        FillRect(28, row_y, 744, 52, COLOR_CARD_BG);
        if (s_gallery_cache[i].is_valid) {
            DrawRoundedCard(28, row_y, 744, 52, 0x1143, COLOR_GREEN_MATCH);

            /* Slot badge */
            char s_str[16];
            snprintf(s_str, sizeof(s_str), "SLOT %d", i + 1);
            DrawRoundedCard(36, row_y + 8, 76, 36, COLOR_GREEN_MATCH, COLOR_WHITE);
            DrawString(44, row_y + 18, s_str, COLOR_WHITE, 0, 1);

            /* Name */
            DrawString(124, row_y + 16, s_gallery_cache[i].name, COLOR_WHITE, 0, 2);

            /* Stats */
            char det[80];
            snprintf(det, sizeof(det), "Speech: %.1fs | Chunks: %lu | RRAM 0x0205B000",
                     s_gallery_cache[i].speech_sec, (unsigned long)s_gallery_cache[i].chunks_accumulated);
            DrawString(320, row_y + 20, det, COLOR_TEXT_MUTED, 0, 1);

            /* Delete button on the right */
            DrawTouchButton(680, row_y + 6, 82, 40, COLOR_RED_REJECT, COLOR_WHITE,
                            "DELETE", COLOR_WHITE, 1);
        } else {
            DrawRoundedCard(28, row_y, 744, 52, 0x08E2, COLOR_CARD_BORDER);

            char s_str[16];
            snprintf(s_str, sizeof(s_str), "SLOT %d", i + 1);
            DrawRoundedCard(36, row_y + 8, 76, 36, COLOR_BTN_SLATE, COLOR_CARD_BORDER);
            DrawString(44, row_y + 18, s_str, COLOR_TEXT_MUTED, 0, 1);

            DrawString(124, row_y + 18, "(Empty Profile Slot)", COLOR_TEXT_MUTED, 0, 1);
            DrawString(320, row_y + 18, "Available for voice enrollment", COLOR_TEXT_MUTED, 0, 1);

            DrawTouchButton(660, row_y + 6, 102, 40, COLOR_PURPLE_ENROLL, COLOR_WHITE,
                            "+ ENROLL", COLOR_WHITE, 1);
        }
        row_y += 56;
    }

    /* Bottom Action Buttons (y = 398, h = 58) */
    DrawTouchButton(28, 398, 230, 58, COLOR_GREEN_MATCH, COLOR_WHITE,
                    "<- DASHBOARD", COLOR_WHITE, 1);

    DrawTouchButton(270, 398, 230, 58, COLOR_PURPLE_ENROLL, COLOR_WHITE,
                    "+ ENROLL VOICE", COLOR_WHITE, 1);

    DrawTouchButton(512, 398, 260, 58, COLOR_CARD_BG, COLOR_RED_REJECT,
                    "CLEAR ALL PROFILES", COLOR_RED_REJECT, 1);

    DisplayUI_Flush();
}

/*******************************************************************************
* VAD & Verification Tuning Screen
********************************************************************************/
void DisplayUI_ShowVadTuning(float rms_thresh, int hangover_ms, float match_thresh, float live_rms, bool live_speech)
{
    s_cur_view = UI_VIEW_VAD_TUNE;

    FillRect(0, 0, VISIBLE_WIDTH, VISIBLE_HEIGHT, COLOR_DARK_BG);
    DrawHeader();

    DrawRoundedCard(16, 50, 768, 420, COLOR_CARD_BG, COLOR_CARD_BORDER);

    DrawRoundedCard(28, 60, 744, 38, COLOR_BTN_SLATE, COLOR_AMBER_WARN);
    DrawString(40, 72, "VOICE ACTIVITY DETECTION (VAD) & SENSITIVITY CALIBRATION", COLOR_WHITE, 0, 1);

    /* Live Mic Level & Test Box (y=104..176, h=72) */
    DrawRoundedCard(28, 104, 744, 72, 0x08E2, COLOR_CARD_BORDER);
    DrawString(38, 112, "LIVE MIC LEVEL (Speak to test):", COLOR_TEXT_MUTED, 0, 1);

    FillRect(38, 134, 480, 24, COLOR_BAR_BG);
    DrawRect(37, 133, 482, 26, COLOR_CARD_BORDER);

    DisplayUI_UpdateVadLiveMeter(rms_thresh, live_rms, live_speech);

    /* Section 2: VAD Threshold Controls (y=182..282, h=100) */
    DrawRoundedCard(28, 182, 744, 100, 0x08E2, COLOR_CARD_BORDER);
    char th_label[80];
    snprintf(th_label, sizeof(th_label), "VAD Energy Threshold: %u RMS  (Lower = More Sensitive)", (unsigned int)rms_thresh);
    DrawString(38, 190, th_label, COLOR_WHITE, 0, 1);

    /* Presets */
    DrawTouchButton(38, 214, 105, 54, ((int)rms_thresh == 60) ? COLOR_ACCENT_BLUE : COLOR_BTN_SLATE,
                    ((int)rms_thresh == 60) ? COLOR_WHITE : COLOR_CARD_BORDER,
                    "High (60)", ((int)rms_thresh == 60) ? COLOR_BLACK : COLOR_WHITE, 1);

    DrawTouchButton(151, 214, 105, 54, ((int)rms_thresh == 100) ? COLOR_ACCENT_BLUE : COLOR_BTN_SLATE,
                    ((int)rms_thresh == 100) ? COLOR_WHITE : COLOR_CARD_BORDER,
                    "Norm (100)", ((int)rms_thresh == 100) ? COLOR_BLACK : COLOR_WHITE, 1);

    DrawTouchButton(264, 214, 105, 54, ((int)rms_thresh == 150) ? COLOR_ACCENT_BLUE : COLOR_BTN_SLATE,
                    ((int)rms_thresh == 150) ? COLOR_WHITE : COLOR_CARD_BORDER,
                    "Mid (150)", ((int)rms_thresh == 150) ? COLOR_BLACK : COLOR_WHITE, 1);

    DrawTouchButton(377, 214, 105, 54, ((int)rms_thresh == 220) ? COLOR_ACCENT_BLUE : COLOR_BTN_SLATE,
                    ((int)rms_thresh == 220) ? COLOR_WHITE : COLOR_CARD_BORDER,
                    "Strict(220)", ((int)rms_thresh == 220) ? COLOR_BLACK : COLOR_WHITE, 1);

    /* Fine Tune Steppers */
    DrawTouchButton(510, 214, 115, 54, COLOR_BTN_SLATE, COLOR_CARD_BORDER,
                    "[- 10]", COLOR_WHITE, 1);

    DrawTouchButton(635, 214, 115, 54, COLOR_BTN_SLATE, COLOR_CARD_BORDER,
                    "[+ 10]", COLOR_WHITE, 1);

    /* Section 3: Hangover & Match Threshold (y=288..364, h=76) */
    DrawRoundedCard(28, 288, 366, 76, 0x08E2, COLOR_CARD_BORDER);
    char hg_str[48];
    snprintf(hg_str, sizeof(hg_str), "Speech Hangover: %d ms", hangover_ms);
    DrawString(38, 296, hg_str, COLOR_WHITE, 0, 1);
    DrawTouchButton(38, 320, 160, 38, COLOR_BTN_SLATE, COLOR_CARD_BORDER, "[- 50ms]", COLOR_WHITE, 1);
    DrawTouchButton(210, 320, 160, 38, COLOR_BTN_SLATE, COLOR_CARD_BORDER, "[+ 50ms]", COLOR_WHITE, 1);

    DrawRoundedCard(406, 288, 366, 76, 0x08E2, COLOR_CARD_BORDER);
    char mt_str[48];
    snprintf(mt_str, sizeof(mt_str), "Match Threshold: %.2f", match_thresh);
    DrawString(416, 296, mt_str, COLOR_WHITE, 0, 1);
    DrawTouchButton(416, 320, 160, 38, COLOR_BTN_SLATE, COLOR_CARD_BORDER, "[- 0.05]", COLOR_WHITE, 1);
    DrawTouchButton(588, 320, 160, 38, COLOR_BTN_SLATE, COLOR_CARD_BORDER, "[+ 0.05]", COLOR_WHITE, 1);

    /* Section 4: Return to Dashboard (y=372..442, h=70) */
    DrawTouchButton(28, 372, 744, 70, COLOR_GREEN_MATCH, COLOR_WHITE,
                    "<- SAVE & RETURN TO DASHBOARD", COLOR_WHITE, 2);

    DisplayUI_Flush();
}

void DisplayUI_UpdateVadLiveMeter(float rms_thresh, float live_rms, bool live_speech)
{
    if (s_cur_view != UI_VIEW_VAD_TUNE) return;

    int bar_x = 38;
    int bar_y = 134;
    int bar_w = 480;
    int bar_h = 24;

    float norm = live_rms / 400.0f;
    if (norm > 1.0f) norm = 1.0f;
    if (norm < 0.0f) norm = 0.0f;

    int fill_w = (int)(norm * bar_w);

    FillRect(bar_x, bar_y, bar_w, bar_h, COLOR_BAR_BG);

    uint16_t bar_col = live_speech ? COLOR_GREEN_MATCH : COLOR_ACCENT_BLUE;
    if (fill_w > 0) {
        FillRect(bar_x, bar_y, fill_w, bar_h, bar_col);
    }

    /* Threshold marker */
    int th_x = bar_x + (int)((rms_thresh / 400.0f) * bar_w);
    if (th_x >= bar_x && th_x < (bar_x + bar_w)) {
        FillRect(th_x - 1, bar_y - 2, 3, bar_h + 4, COLOR_RED_REJECT);
    }

    /* Live Status Badge */
    if (live_speech) {
        DrawRoundedCard(530, 126, 230, 40, 0x1143, COLOR_GREEN_MATCH);
        DrawString(546, 138, "[SPEECH DETECTED]", COLOR_GREEN_MATCH, 0, 1);
    } else {
        DrawRoundedCard(530, 126, 230, 40, COLOR_CARD_BG, COLOR_CARD_BORDER);
        DrawString(546, 138, "[BACKGROUND/QUIET]", COLOR_TEXT_MUTED, 0, 1);
    }

    /* Live RMS text */
    FillRect(320, 110, 190, 18, 0x08E2);
    char rms_txt[32];
    snprintf(rms_txt, sizeof(rms_txt), "RMS: %3u (Th: %u)", (unsigned int)live_rms, (unsigned int)rms_thresh);
    DrawString(320, 112, rms_txt, live_speech ? COLOR_GREEN_MATCH : COLOR_WHITE, 0, 1);

    DisplayUI_Flush();
}

/*******************************************************************************
* Touch Hit Testing
********************************************************************************/
DisplayTouchTarget DisplayUI_HitTest(int x, int y)
{
    if (x < 0 || x >= VISIBLE_WIDTH || y < 0 || y >= VISIBLE_HEIGHT) {
        return TOUCH_HIT_NONE;
    }

    /* 1. Modal Views Hit-Testing */
    if (s_cur_view == UI_VIEW_DELETE_CONFIRM || s_cur_view == UI_VIEW_CLEAR_CONFIRM) {
        if (Touch_HitTest(x, y, 180, 270, 210, 68)) {
            return TOUCH_HIT_MODAL_CONFIRM;
        }
        if (Touch_HitTest(x, y, 410, 270, 210, 68)) {
            return TOUCH_HIT_MODAL_CANCEL;
        }
        return TOUCH_HIT_NONE;
    }

    if (s_cur_view == UI_VIEW_BENCHMARK) {
        if (Touch_HitTest(x, y, 28, 330, 474, 70)) {
            return TOUCH_HIT_MODAL_CANCEL; // Back to dashboard
        }
        return TOUCH_HIT_NONE;
    }

    if (s_cur_view == UI_VIEW_GALLERY_DETAIL) {
        /* Rows delete/enroll buttons (y = 106, step = 56) */
        int row_y = 106;
        for (int i = 0; i < 5; ++i) {
            if (s_gallery_cache[i].is_valid) {
                if (Touch_HitTest(x, y, 680, row_y + 6, 82, 40)) {
                    return (DisplayTouchTarget)(TOUCH_HIT_DEL_SLOT_1 + i);
                }
            } else {
                if (Touch_HitTest(x, y, 660, row_y + 6, 102, 40)) {
                    return (DisplayTouchTarget)(TOUCH_HIT_SLOT_1 + i);
                }
            }
            row_y += 56;
        }

        /* Bottom buttons */
        if (Touch_HitTest(x, y, 28, 398, 230, 58)) return TOUCH_HIT_GALLERY_BACK;
        if (Touch_HitTest(x, y, 270, 398, 230, 58)) return TOUCH_HIT_GALLERY_ENROLL_NEW;
        if (Touch_HitTest(x, y, 512, 398, 260, 58)) return TOUCH_HIT_GALLERY_CLEAR_ALL;

        return TOUCH_HIT_NONE;
    }

    if (s_cur_view == UI_VIEW_VAD_TUNE) {
        /* Presets */
        if (Touch_HitTest(x, y, 38, 214, 105, 54)) return TOUCH_HIT_VAD_SENS_60;
        if (Touch_HitTest(x, y, 151, 214, 105, 54)) return TOUCH_HIT_VAD_SENS_100;
        if (Touch_HitTest(x, y, 264, 214, 105, 54)) return TOUCH_HIT_VAD_SENS_150;
        if (Touch_HitTest(x, y, 377, 214, 105, 54)) return TOUCH_HIT_VAD_SENS_220;

        /* Fine tune buttons */
        if (Touch_HitTest(x, y, 510, 214, 115, 54)) return TOUCH_HIT_VAD_THRESH_MINUS;
        if (Touch_HitTest(x, y, 635, 214, 115, 54)) return TOUCH_HIT_VAD_THRESH_PLUS;

        /* Hangover */
        if (Touch_HitTest(x, y, 38, 320, 160, 38)) return TOUCH_HIT_VAD_HANGOVER_MINUS;
        if (Touch_HitTest(x, y, 210, 320, 160, 38)) return TOUCH_HIT_VAD_HANGOVER_PLUS;

        /* Match Thresh */
        if (Touch_HitTest(x, y, 416, 320, 160, 38)) return TOUCH_HIT_VAD_MATCH_MINUS;
        if (Touch_HitTest(x, y, 588, 320, 160, 38)) return TOUCH_HIT_VAD_MATCH_PLUS;

        /* Back button */
        if (Touch_HitTest(x, y, 28, 372, 744, 70)) return TOUCH_HIT_VAD_BACK;

        return TOUCH_HIT_NONE;
    }

    if (s_cur_view == UI_VIEW_ENROLL_STUDIO) {
        /* Name Preset Pills (y = 126..162) */
        int pill_x = 28;
        for (int i = 0; i < 5; ++i) {
            if (Touch_HitTest(x, y, pill_x, 126, 90, 36)) {
                return (DisplayTouchTarget)(TOUCH_HIT_ENROLL_NAME_0 + i);
            }
            pill_x += 96;
        }

        /* Start/Save button */
        if (Touch_HitTest(x, y, 28, 310, 230, 74)) {
            return TOUCH_HIT_ENROLL_START_SAVE;
        }
        /* Cancel button */
        if (Touch_HitTest(x, y, 272, 310, 230, 74)) {
            return TOUCH_HIT_ENROLL_CANCEL;
        }
        return TOUCH_HIT_NONE;
    }

    /* 2. Main Dashboard View Hit-Testing */
    if (s_cur_view == UI_VIEW_DASHBOARD) {
        /* Primary Action Buttons (Row 1: y = 326..394, h = 68) */
        if (Touch_HitTest(x, y, 16, 326, 246, 68)) {
            return TOUCH_HIT_BTN_VERIFY;
        }
        if (Touch_HitTest(x, y, 276, 326, 246, 68)) {
            return TOUCH_HIT_BTN_ENROLL;
        }
        if (Touch_HitTest(x, y, 536, 326, 248, 68)) {
            return TOUCH_HIT_BTN_GALLERY;
        }

        /* Utility Buttons (Row 2: y = 404..466, h = 62) */
        if (Touch_HitTest(x, y, 16, 404, 246, 62)) {
            return TOUCH_HIT_BTN_VAD_TUNE;
        }
        if (Touch_HitTest(x, y, 276, 404, 246, 62)) {
            return TOUCH_HIT_BTN_BENCHMARK;
        }
        if (Touch_HitTest(x, y, 536, 404, 248, 62)) {
            return TOUCH_HIT_BTN_CLEAR_ALL;
        }

        return TOUCH_HIT_NONE;
    }

    return TOUCH_HIT_NONE;
}

/*******************************************************************************
* Display and Touch Initialization
********************************************************************************/
bool DisplayUI_Init(void)
{
    cy_en_gfx_status_t gfx_status = Cy_GFXSS_Init(GFXSS, &GFXSS_config, &s_gfx_context);
    if (CY_GFX_SUCCESS != gfx_status) {
        return false;
    }

    /* Configure DC Interrupt */
    cy_stc_sysint_t dc_irq_cfg = {
        .intrSrc      = GFXSS_DC_IRQ,
        .intrPriority = 3
    };
    Cy_SysInt_Init(&dc_irq_cfg, disp_dc_isr);
    NVIC_EnableIRQ(GFXSS_DC_IRQ);

    /* Configure GPU Interrupt */
    cy_stc_sysint_t gpu_irq_cfg = {
        .intrSrc      = GFXSS_GPU_IRQ,
        .intrPriority = 3
    };
    Cy_SysInt_Init(&gpu_irq_cfg, disp_gpu_isr);
    Cy_GFXSS_Enable_GPU_Interrupt(GFXSS);
    NVIC_EnableIRQ(GFXSS_GPU_IRQ);

    /* Initialize I2C Controller for Waveshare panel bridge & Touch */
    cy_en_scb_i2c_status_t i2c_result = Cy_SCB_I2C_Init(CYBSP_I2C_CONTROLLER_HW,
                                                        &CYBSP_I2C_CONTROLLER_config,
                                                        &s_i2c_context);
    if (CY_SCB_I2C_SUCCESS != i2c_result) {
        return false;
    }

    cy_stc_sysint_t i2c_irq_cfg = {
        .intrSrc      = CYBSP_I2C_CONTROLLER_IRQ,
        .intrPriority = 2
    };
    Cy_SysInt_Init(&i2c_irq_cfg, disp_i2c_isr);
    NVIC_EnableIRQ(CYBSP_I2C_CONTROLLER_IRQ);

    Cy_SCB_I2C_Enable(CYBSP_I2C_CONTROLLER_HW);
    Cy_SysLib_Delay(500);

    /* Initialize Waveshare 4.3 panel bridge */
    i2c_result = mtb_disp_waveshare_4p3_init(CYBSP_I2C_CONTROLLER_HW, &s_i2c_context);
    if (CY_SCB_I2C_SUCCESS != i2c_result) {
        return false;
    }

    /* Set brightness to 100% */
    mtb_disp_waveshare_4p3_set_brightness(CYBSP_I2C_CONTROLLER_HW, &s_i2c_context, 100);

    /* Initialize FT5406 Capacitive Touch */
    TouchController_Init(&s_i2c_context);

    /* Render initial dashboard layout */
    DisplayUI_DrawDashboard(false);
    DisplayUI_Flush();

    return true;
}
