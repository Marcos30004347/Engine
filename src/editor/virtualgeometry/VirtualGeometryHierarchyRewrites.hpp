#pragma once

#include "virtualgeometry/VirtualGeometryData.hpp"

namespace virtualgeometry
{
class VirtualGeometryHierarchyRewrites
{
    public:
  static void buildPagesAndRewrites(VirtualGeometryBuildData &data, const VirtualGeometryBuildSettings &settings);
};

}; // namespace virtualgeometry
