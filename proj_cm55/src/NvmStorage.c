#include "NvmStorage.hpp"
#include "cy_pdl.h"
#include "cy_rram.h"

#include <string.h>
#include <stdio.h>

static uint32_t ComputeCrc32(const uint8_t* data, size_t length)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    return ~crc;
}

int NvmStorage_Load(SpeakerProfile* gallery)
{
    if (!gallery) return 0;

    // Default: initialize empty gallery
    for (int i = 0; i < MAX_SPEAKER_PROFILES; ++i) {
        gallery[i].enrolled = false;
        memset(gallery[i].name, 0, sizeof(gallery[i].name));
        gallery[i].numChunks = 0;
        gallery[i].totalSpeechSec = 0.0f;
        memset(gallery[i].embedding, 0, sizeof(gallery[i].embedding));
    }

    const GalleryStorageRecord* rec = (const GalleryStorageRecord*)NVM_STORAGE_ADDRESS;

    // Validate header magic and structure version
    if (rec->magic != NVM_STORAGE_MAGIC || rec->version != 1) {
        printf("[NVM] No previous profile gallery found in RRAM (0x%08X). Starting fresh.\r\n",
               (unsigned int)NVM_STORAGE_ADDRESS);
        return 0;
    }

    // Verify CRC32 checksum
    uint32_t expectedCrc = ComputeCrc32((const uint8_t*)rec->profiles, sizeof(rec->profiles));
    if (rec->crc32 != expectedCrc) {
        printf("[NVM] Storage CRC32 mismatch (stored: 0x%08lX, expected: 0x%08lX). Resetting.\r\n",
               (unsigned long)rec->crc32, (unsigned long)expectedCrc);
        return 0;
    }

    int restoredCount = 0;
    for (int i = 0; i < MAX_SPEAKER_PROFILES; ++i) {
        if (rec->profiles[i].enrolled == 1) {
            gallery[i].enrolled = true;
            strncpy(gallery[i].name, rec->profiles[i].name, sizeof(gallery[i].name) - 1);
            gallery[i].name[sizeof(gallery[i].name) - 1] = '\0';
            gallery[i].numChunks = rec->profiles[i].numChunks;
            gallery[i].totalSpeechSec = rec->profiles[i].totalSpeechSec;
            memcpy(gallery[i].embedding, rec->profiles[i].embedding, sizeof(gallery[i].embedding));
            restoredCount++;
        }
    }

    printf("[NVM] Successfully restored %d speaker profile(s) from non-volatile RRAM (0x%08X).\r\n",
           restoredCount, (unsigned int)NVM_STORAGE_ADDRESS);
    return restoredCount;
}

bool NvmStorage_Save(const SpeakerProfile* gallery)
{
    if (!gallery) return false;

    GalleryStorageRecord rec;
    memset(&rec, 0, sizeof(rec));

    rec.magic = NVM_STORAGE_MAGIC;
    rec.version = 1;
    rec.count = 0;

    for (int i = 0; i < MAX_SPEAKER_PROFILES; ++i) {
        if (gallery[i].enrolled) {
            rec.profiles[i].enrolled = 1;
            strncpy(rec.profiles[i].name, gallery[i].name, sizeof(rec.profiles[i].name) - 1);
            rec.profiles[i].name[sizeof(rec.profiles[i].name) - 1] = '\0';
            rec.profiles[i].numChunks = gallery[i].numChunks;
            rec.profiles[i].totalSpeechSec = gallery[i].totalSpeechSec;
            memcpy(rec.profiles[i].embedding, gallery[i].embedding, sizeof(rec.profiles[i].embedding));
            rec.count++;
        } else {
            rec.profiles[i].enrolled = 0;
        }
    }

    rec.crc32 = ComputeCrc32((const uint8_t*)rec.profiles, sizeof(rec.profiles));

    // Write to RRAM
    cy_en_rram_status_t status = Cy_RRAM_WriteByteArray(RRAMC0,
                                                        NVM_STORAGE_ADDRESS,
                                                        (const uint8_t*)&rec,
                                                        sizeof(rec));

    if (status != CY_RRAM_SUCCESS) {
        printf("[NVM] Note: Cy_RRAM_WriteByteArray returned status %d. Profiles retained in SRAM.\r\n",
               (int)status);
        return false;
    }

    // Verify written data in memory
    if (memcmp((const void*)NVM_STORAGE_ADDRESS, &rec, sizeof(rec)) != 0) {
        printf("[NVM] Write verification mismatch in RRAM. Profiles retained in SRAM.\r\n");
        return false;
    }

    printf("[NVM] Gallery state (%u active profile(s)) successfully committed to non-volatile RRAM.\r\n",
           (unsigned int)rec.count);
    return true;
}

bool NvmStorage_DeleteSlot(SpeakerProfile* gallery, int slotIndex)
{
    if (!gallery || slotIndex < 0 || slotIndex >= MAX_SPEAKER_PROFILES) {
        return false;
    }

    gallery[slotIndex].enrolled = false;
    memset(gallery[slotIndex].name, 0, sizeof(gallery[slotIndex].name));
    gallery[slotIndex].numChunks = 0;
    gallery[slotIndex].totalSpeechSec = 0.0f;
    memset(gallery[slotIndex].embedding, 0, sizeof(gallery[slotIndex].embedding));

    return NvmStorage_Save(gallery);
}

bool NvmStorage_ClearAll(SpeakerProfile* gallery)
{
    if (!gallery) return false;

    for (int i = 0; i < MAX_SPEAKER_PROFILES; ++i) {
        gallery[i].enrolled = false;
        memset(gallery[i].name, 0, sizeof(gallery[i].name));
        gallery[i].numChunks = 0;
        gallery[i].totalSpeechSec = 0.0f;
        memset(gallery[i].embedding, 0, sizeof(gallery[i].embedding));
    }

    return NvmStorage_Save(gallery);
}
