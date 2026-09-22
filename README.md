# Real-Time ECAPA-TDNN Speaker Verification on Infineon PSOC™ Edge E84

[![Target: KIT_PSE84_EVAL](https://img.shields.io/badge/Hardware-KIT__PSE84__EVAL-007079?logo=infineon&logoColor=white)](https://www.infineon.com)
[![Compute: CM55 + Ethos-U55](https://img.shields.io/badge/Compute-Cortex--M55%20%2B%20Ethos--U55%20(256%20MACs)-0091BD?logo=arm&logoColor=white)](https://www.arm.com)
[![Toolchain: ModusToolbox 3.3+](https://img.shields.io/badge/Toolchain-ModusToolbox%20%7C%20LLVM__ARM-blue)](https://www.infineon.com/modustoolbox)
#[![License: Apache 2.0](https://img.shields.io/badge/License-Apache%202.0-green.svg)](LICENSE)

An edge AI deployment of the **ECAPA-TDNN** (Emphasized Channel Attention, Propagation and Aggregation Time Delay Neural Network) acoustic biometric architecture on the **Infineon PSOC™ Edge E84 Evaluation Kit** (`KIT_PSE84_EVAL_EPC2` / `PSE846GPS2DBZC4A`).

The system performs **real-time on-device speaker verification and enrollment** with hardware microphone decimation, adaptive Voice Activity Detection (VAD), non-volatile RRAM speaker profile persistence, and an interactive 4.3" MIPI-DSI touch interface.

---

## Key Highlights & Performance

- **Sub-400 ms Inference**: Evaluates a **3.0-second speech window** (48,000 samples @ 16 kHz) in **375.5 ms** ($\approx 8\times$ faster than real-time).
- **Heterogeneous Graph Partitioning**:
  - **Conv1D / Res2Net Backbone**: Runs on **Arm® Ethos™-U55 NPU** (256 MACs/cycle) via INT8 Post-Training Quantization (PTQ).
  - **Attentive Statistics Pooling (ASP) Head**: Runs on **Arm® Cortex®-M55** via 128-bit **Helium MVE** vector SIMD (`vmladavaq_s8`, `vctp8q`) operating on hybrid packed INT4 weights.
- **Ultra-Compact Footprint**:
  - Ethos-U55 dynamic tensor arena: **1.135 MB** in SoCMem SRAM (`0x26000000`).
  - Compressed ASP head weights: **373.5 KB** in QSPI Flash (decompressed on startup via `tinf`).
- **Autonomous Streaming Pipeline**:
  - Continuous 16 kHz audio capture via on-board PDM-to-PCM hardware decimation (96× downsampling).
  - Dual-threshold SNR/Energy Voice Activity Detection (VAD) with dynamic noise-floor tracking and 350 ms speech hangover.
- **Hardware-Backed Non-Volatile Profile Gallery**:
  - Enrolls up to 5 speaker identities directly into on-chip **RRAM** (`user_nvm` @ `0x0205B000`) protected by CRC32 checksums.
- **Zero-Copy MIPI-DSI Touch GUI**:
  - Direct scanout to Waveshare 4.3" $800 \times 480$ capacitive touch LCD via the GFXSS controller from SoCMem (`0x2633C000`).

---

## Latency & Resource Breakdown

Benchmarked on physical hardware with Cortex-M55 @ 400 MHz and Ethos-U55 @ 400 MHz:

| Stage | Subsystem | Format / Precision | Latency | Share |
| :--- | :--- | :--- | :--- | :--- |
| **Mel Feature Extraction** | Cortex-M55 (CMSIS-DSP RFFT) | FP32 / Helium MVE | 38.6 ms | 10.3% |
| **ECAPA-TDNN Backbone** | Arm Ethos-U55 NPU | INT8 Symmetric | 111.7 ms | 29.7% |
| **ASP Head & Projection** | Cortex-M55 | Hybrid Packed INT4/INT8 (Helium) | 225.2 ms | 60.0% |
| **Cosine Similarity Match** | Cortex-M55 | FP32 Vector Dot Product | < 0.1 ms | < 0.1% |
| **Total Pipeline** | **Heterogeneous System** | **Hybrid Precision** | **375.5 ms** | **100.0%** |

---

## Repository Structure

```
.
├── bsps/                               # Board Support Package for KIT_PSE84_EVAL_EPC2
├── common.mk                           # Toolchain (LLVM_ARM) & build configurations
├── common_app.mk                       # Multi-core build orchestration
├── common_modules/                     # Retarget IO, profiler, and logging components
├── configs/                            # Bootloader signing and image combining scripts
├── docs/                               # Full academic research paper and architecture diagrams
│   └── ECAPA_TDNN_PSOC_Edge_E84_Deployment_Paper.md
├── Makefile                            # Top-level ModusToolbox application Makefile
├── proj_cm33_s/                        # Cortex-M33 Secure Core (Bootloader, memory protection)
├── proj_cm33_ns/                       # Cortex-M33 Non-Secure Core (IPC, system control)
└── proj_cm55/                          # Cortex-M55 DSP/AI Application Core
    ├── Makefile                        # CM55 build configuration & flags
    ├── FreeRTOSConfig.h                # FreeRTOS kernel settings
    ├── head/                           # ASP Head decompression and weight definitions
    │   ├── CompressedHeadWeights.hpp   # Compressed head weights header (size & signature)
    │   ├── CompressedHeadWeights.cc    # [OMITTED - See Pretrained Model Weights]
    │   ├── tinf.c / tinf.h             # Embedded Tiny Inflate zlib decompression engine
    │   └── InputFiles.hpp / .cc        # Embedded test utterance vectors for offline validation
    ├── model/                          # Ethos-U55 TFLM neural network model
    │   ├── MODEL_E84_tflm_model_int8x8.h  # Model buffer header and arena definitions
    │   └── MODEL_E84_tflm_model_int8x8.c  # [OMITTED - See Pretrained Model Weights]
    └── src/
        ├── DisplayUI.c / .h            # 800x480 GFXSS DSI GUI, VU meter, gallery view
        ├── NvmStorage.c / .hpp         # RRAM non-volatile storage driver (0x0205B000)
        ├── PdmMic.c / .hpp             # Hardware PDM-PCM decimator & ring buffer driver
        ├── SpeakerVerification.cpp     # Complete pipeline: Mel FE, ASP pooling, VAD, FreeRTOS
        ├── SpeakerVerification.hpp
        └── TouchController.c / .h      # I2C FT5406 capacitive touch driver
```

---

## Pretrained Model Weights Notice

To comply with licensing and proprietary distribution agreements:
1. **Ethos-U55 Backbone Weights (`proj_cm55/model/MODEL_E84_tflm_model_int8x8.c`)**
2. **Compressed INT4 ASP Head Weights (`proj_cm55/head/CompressedHeadWeights.cc`)**

are **not hosted in this public repository**.

### Requesting Model Weights
The weights can be made available for academic, research, or evaluation purposes.
- Please contact: **Rajan Kumar** at `rajan4705kr@gmail.com`
- Subject: `[PSOC Edge E84] ECAPA-TDNN Model Weights Request`
- Include: Your name, organization/affiliation, and intended evaluation purpose.

Once received, copy the two files into their designated directories:
```bash
cp MODEL_E84_tflm_model_int8x8.c proj_cm55/model/
cp CompressedHeadWeights.cc      proj_cm55/head/
```

---

## Hardware Requirements

| Hardware Item | Purpose |
| :--- | :--- |
| **Infineon KIT_PSE84_EVAL** (`PSE846GPS2DBZC4A`) | Target microcontroller evaluation board |
| **Waveshare 4.3" DSI LCD (800x480)** | Capacitive touch graphical user interface |
| **On-board Digital PDM Mic (Ch 2/3)** | Real-time live voice acquisition |
| **USB-C Cables (x2)** | Target power and KitProg3 / J-Link UART/debugging |

---

## Software & Toolchain Prerequisites

1. **Infineon ModusToolbox™ Software v3.3 or higher**:
   - Install from [Infineon Developer Center](https://www.infineon.com/modustoolbox).
2. **LLVM Compiler for Arm (LLVM_ARM v19.1.5 or newer)**:
   - Required for Armv8.1-M Helium vector intrinsics and Ethos-U55 toolchain support.
   - Set the environment variable:
     ```bash
     export CY_COMPILER_LLVM_ARM_DIR="/path/to/LLVM-ET-Arm-19.1.5"
     ```
3. **Python 3.10+** (for offline conversion/quantization toolchains).

---

## Quick Start & Build Guide

### 1. Clone the Repository
```bash
git clone https://github.com/rajan4705/SpeakerVerification_Edge.git
cd SpeakerVerification_Edge
```

### 2. Fetch Dependent ModusToolbox Libraries
Run `make getlibs` to download the specific SDK libraries referenced by the `.mtb` manifests:
```bash
make getlibs
```

### 3. Place Model Weights
Ensure `proj_cm55/model/MODEL_E84_tflm_model_int8x8.c` and `proj_cm55/head/CompressedHeadWeights.cc` are placed in their respective folders.

### 4. Build the Multi-Core Image
Build all three targets (`proj_cm33_s`, `proj_cm33_ns`, `proj_cm55`) in release mode:
```bash
make build -j$(nproc)
```

### 5. Flash and Program the Board
Connect the board via the KitProg3 USB port and flash:
```bash
make program
```

---

## Interactive Operation

### Serial Terminal Console
Connect to the KitProg3 virtual COM port at **115200 baud, 8N1**:
```bash
picocom -b 115200 /dev/ttyACM0
```

### Supported Operations
- **Interactive Enrollment**:
  - Press the **ENROLL** button on the touch screen or via serial prompt.
  - Speak 3 consecutive utterances. The system verifies consistency ($\cos \ge 0.55$) and saves the normalized centroid embedding to non-volatile **RRAM**.
- **Real-Time Verification**:
  - Press **VERIFY** or speak naturally.
  - The adaptive VAD triggers automatically on speech onset, snapshots 3.0 seconds of audio, runs inference, and displays:
    - **VERIFIED MATCH**: Speaker Name + Cosine Similarity score.
    - **ACCESS DENIED**: Impostor or similarity below threshold ($\theta = 0.55$).
- **Gallery Management**:
  - Persistent profiles survive hardware power cycles and reboots.
  - Select any enrolled profile slot on the display to review stats or clear a slot.

---

## Citation

If you use this codebase or deployment methodology in your research or project, please cite:

```bibtex
@misc{ecapa_tdnn_psoc_edge_e84,
  title={Real-Time Edge Speaker Verification: Heterogeneous Acceleration and Quantization of ECAPA-TDNN on Infineon PSOC Edge E84},
  author={Your Name and Collaborators},
  year={2026},
  note={Infineon PSOC Edge E84 Deployment with Cortex-M55 Helium MVE and Ethos-U55 NPU}
}
```

---
