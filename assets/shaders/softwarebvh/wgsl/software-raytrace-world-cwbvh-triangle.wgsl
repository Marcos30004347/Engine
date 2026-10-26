#ifdef SWRT_ENABLE_WGSL_SUBGROUP_EXTENSION
enable subgroups;
#endif

#include "software-raytrace.wgsl"
#include "software-raytrace-primitives-triangle.wgsl"
#include "software-raytrace-direct.wgsl"
