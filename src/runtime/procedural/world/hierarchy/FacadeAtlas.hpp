#pragma once
#include "procedural/world/hierarchy/CellStructure.hpp"
#include <array>

namespace procedural::world::hierarchy {
using FacadeTriangle = std::array<rewrite::Position,3>;
std::vector<rewrite::Position> apertureContour(ApertureShape, double archSpring = -1);
const std::vector<FacadeTriangle> &facadeAsset(FacadeAsset, ApertureShape);
void planFacadeInstances(Structure &, const rewrite::DwellingFacadePolicy &);
void lowerWindowHead(Structure &, uint32_t face, double absoluteHeight);
void meshFacadeInstances(const Structure &, Solid &);
} // namespace procedural::world::hierarchy
