#ifndef COMPRESSED_HEAD_WEIGHTS_HPP
#define COMPRESSED_HEAD_WEIGHTS_HPP

#include <cstdint>
#include <cstddef>

namespace arm::app::speaker {

constexpr size_t g_CompressedHeadWeightsSize = 373557;
constexpr size_t g_DecompressedHeadWeightsSize = 727556;

extern const uint8_t g_CompressedHeadWeights[g_CompressedHeadWeightsSize];

} // namespace arm::app::speaker

#endif // COMPRESSED_HEAD_WEIGHTS_HPP
