################################################################################
# \file Makefile
# \brief
# Makefile for configuring the Cortex-M55 core ECAPA-TDNN project
################################################################################

include ../common.mk

################################################################################
# Basic Configuration
################################################################################

APPNAME=proj_cm55
APPTYPE=flash

CORE=CM55
CORE_NAME=CM55_0

VFP_SELECT=hardfp

DEFINES+=FLASH_BOOT
DEFINES+=CY_RETARGET_IO_CONVERT_LF_TO_CRLF
DEFINES+=CY_RTOS_AWARE

COMPONENTS+=FREERTOS RTOS_AWARE

# Logging & Debug UART
COMPONENTS+=APP_LOGGER
COMPONENTS+=RETARGET_IO

# CMSIS-DSP for FFT, mel filterbank, vector math
COMPONENTS+=CMSIS_DSP

# Ethos-U55 NPU & TensorFlow Lite for Microcontrollers
NPU=U55
DEFINES+=ETHOSU55
COMPONENTS+=ML_TFLM
DEFINES+=TF_LITE_STATIC_MEMORY
DEFINES+=CY_ML_MODEL_MEM=.model_data
COMPONENTS+=U55

# C++ flags
CXXFLAGS+=-std=c++17 -fno-rtti -fno-exceptions

# Include search paths
INCLUDES+=./model
INCLUDES+=./head
INCLUDES+=./src
INCLUDES+=../common_modules/COMPONENT_APP_LOGGER
INCLUDES+=../common_modules/COMPONENT_RETARGET_IO
INCLUDES+=../../mtb_shared/display-dsi-waveshare-4-3-lcd/release-v1.0.0

# Waveshare 4.3-inch MIPI-DSI Display Driver & FT5406 Capacitive Touch
DEFINES+=MTB_DISPLAY_W4P3INCH_RPI MTB_CTP_FT5406
INCLUDES+=../../mtb_shared/touch-ctp-ft5406/release-v1.0.0
SOURCES+=../../mtb_shared/display-dsi-waveshare-4-3-lcd/release-v1.0.0/mtb_disp_dsi_waveshare_4p3.c
SOURCES+=../../mtb_shared/touch-ctp-ft5406/release-v1.0.0/mtb_ctp_ft5406.c

# Ignore unused middleware from shared repos
CY_IGNORE+=$(SEARCH_emusb-device)
CY_IGNORE+=$(SEARCH_audio-voice-core)
CY_IGNORE+=$(SEARCH_audio-front-end)
CY_IGNORE+=$(SEARCH_audio-codec-tlv320dac3100)

CY_APP_PATH=
CY_GETLIBS_SHARED_PATH=../..
CY_GETLIBS_SHARED_NAME=mtb_shared

include $(CY_TOOLS_DIR)/make/start.mk
