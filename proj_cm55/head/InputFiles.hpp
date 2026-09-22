#ifndef INPUT_FILES_HPP
#define INPUT_FILES_HPP

#include <cstdint>
#include <cstddef>

namespace arm::app::speaker {

struct AudioSample {
    const char* name;
    const char* speakerId;
    const int8_t* compressed_data;
    size_t size;
};

constexpr size_t g_NumTestAudioFiles = 4;
extern const AudioSample g_TestAudioSamples[g_NumTestAudioFiles];

void DecompressAudio(const int8_t* comp, int16_t* pcm_out, size_t numSamples);

} // namespace arm::app::speaker

#endif // INPUT_FILES_HPP
