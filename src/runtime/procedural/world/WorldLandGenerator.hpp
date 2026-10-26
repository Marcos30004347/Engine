#pragma once
#include "WorldLandProduct.hpp"

namespace procedural::world::rewrite {
// New land-stage entry point: purpose-seeded Poisson/Voronoi partition, canonical
// land product, no legacy height/climate/river mutation. Cores are selected by
// nearest site to authored world positions; duplicated nearest cores are rejected.
bool generateLand(const WorldSpec &, double spacingMetres, const std::vector<Position> &cores, double landFraction, LandProduct &, std::string *error = nullptr, double oceanMarginMetres = 0);
} // namespace procedural::world::rewrite
