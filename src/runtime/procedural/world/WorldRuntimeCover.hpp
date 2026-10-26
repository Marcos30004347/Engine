#pragma once
#include "WorldRuntimePublication.hpp"

namespace procedural::world::rewrite {
// Footprint-filtered pages are generated from the semantic source, not averaged
// across hard material boundaries. Level zero remains assets.coverPages.
bool buildRuntimeCoverMips(RuntimeWorldAssets &, const WorldCoverDetail &, std::vector<CoverDefinition>, uint32_t levels, std::string *error = nullptr);
bool validateRuntimeCoverMips(const RuntimeWorldAssets &, std::string *error = nullptr);
// Reader for validated immutable runtime assets.
// Vertex shading reader: expectedMaterial comes from the triangle, so hard
// ownership never depends on a coarse texel. Returns definition-order weights.
bool sampleRuntimeCover(const RuntimeWorldAssets &, double x, double z, uint32_t expectedMaterial, double footprintMetres, CoverSample &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
