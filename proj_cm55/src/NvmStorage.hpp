#ifndef NVM_STORAGE_HPP_
#define NVM_STORAGE_HPP_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MAX_SPEAKER_PROFILES  5
#define EMBEDDING_DIM_SIZE    192
#define NVM_STORAGE_ADDRESS   0x0205B000 // user_nvm region for non-secure cores
#define NVM_STORAGE_MAGIC     0xECAB0005

typedef struct {
    bool enrolled;
    char name[32];
    uint32_t numChunks;
    float totalSpeechSec;
    float embedding[EMBEDDING_DIM_SIZE];
} SpeakerProfile;

#pragma pack(push, 4)
typedef struct {
    uint32_t enrolled;
    char name[32];
    uint32_t numChunks;
    float totalSpeechSec;
    float embedding[EMBEDDING_DIM_SIZE];
} StoredProfile;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t crc32;
    StoredProfile profiles[MAX_SPEAKER_PROFILES];
} GalleryStorageRecord;
#pragma pack(pop)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes storage and loads existing profiles from non-volatile memory.
 * @param gallery Pointer to memory array of SpeakerProfile[MAX_SPEAKER_PROFILES].
 * @return Number of valid enrolled profiles restored from NVM.
 */
int NvmStorage_Load(SpeakerProfile* gallery);

/**
 * @brief Saves current gallery to non-volatile memory with CRC32 verification.
 * @param gallery Pointer to memory array of SpeakerProfile[MAX_SPEAKER_PROFILES].
 * @return true if successfully written and verified in NVM.
 */
bool NvmStorage_Save(const SpeakerProfile* gallery);

/**
 * @brief Deletes a profile from a given slot [0..4] and persists to NVM.
 */
bool NvmStorage_DeleteSlot(SpeakerProfile* gallery, int slotIndex);

/**
 * @brief Erases all profiles from the gallery and persists to NVM.
 */
bool NvmStorage_ClearAll(SpeakerProfile* gallery);

#ifdef __cplusplus
}
#endif

#endif // NVM_STORAGE_HPP_
