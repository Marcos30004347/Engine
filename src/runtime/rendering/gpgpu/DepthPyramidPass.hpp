#pragma once

// Shared scene-independent entry point for the Engine SPD depth pyramid.
// Keep the compatibility implementation in virtualgeometry until all callers
// have migrated; voxel rendering must depend only on this shared alias.
#include "virtualgeometry/rendering/VirtualGeometryDepthPyramidPass.hpp"

namespace rendering::gpgpu
{

using DepthPyramidPass = ::virtualgeometry::gpgpu::VirtualGeometryDepthPyramidPass;

} // namespace rendering::gpgpu
