#pragma once

#include <algorithm>
#include <limits>

namespace virtualgeometry
{

struct AABB
{
  float minPoint[3], maxPoint[3];

  AABB()
  {
    minPoint[0] = minPoint[1] = minPoint[2] = std::numeric_limits<float>::max();
    maxPoint[0] = maxPoint[1] = maxPoint[2] = std::numeric_limits<float>::lowest();
  }

  void expandBy(const float p[3])
  {
    for (int i = 0; i < 3; ++i)
    {
      minPoint[i] = std::min(minPoint[i], p[i]);
      maxPoint[i] = std::max(maxPoint[i], p[i]);
    }
  }

  void expandBy(const AABB &o)
  {
    expandBy(o.minPoint);
    expandBy(o.maxPoint);
  }

  void GetCenter(float c[3]) const
  {
    for (int i = 0; i < 3; ++i)
      c[i] = (minPoint[i] + maxPoint[i]) * 0.5f;
  }

  float GetSurfaceArea() const
  {
    const float dx = maxPoint[0] - minPoint[0];
    const float dy = maxPoint[1] - minPoint[1];
    const float dz = maxPoint[2] - minPoint[2];
    return 2.f * (dx * dy + dy * dz + dz * dx);
  }

  static AABB Union(const AABB &a, const AABB &b)
  {
    AABB r;
    for (int i = 0; i < 3; ++i)
    {
      r.minPoint[i] = std::min(a.minPoint[i], b.minPoint[i]);
      r.maxPoint[i] = std::max(a.maxPoint[i], b.maxPoint[i]);
    }
    return r;
  }
};

} // namespace virtualgeometry
