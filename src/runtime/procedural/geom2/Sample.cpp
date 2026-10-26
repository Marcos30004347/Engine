#include "procedural/geom2/Sample.hpp"

#include "procedural/PoissonDiskSampler.hpp"
#include "procedural/WeightedSampleElimination.hpp"

#include <cmath>

namespace procedural::geom2 {

namespace {

bool fail(std::string *error, const char *text)
{
  if (error != nullptr)
  {
    *error = text;
  }

  return false;
}

} // namespace

bool samplePolygon(const Polygon &polygon, const SampleSettings &settings, std::vector<Point> &out, std::string *error)
{
  out.clear();

  const std::string fault = polygonFault(polygon);

  if (!fault.empty())
  {
    return fail(error, "sample region is not a valid polygon");
  }

  if (!(settings.spacingMetres > 0.0) || !std::isfinite(settings.spacingMetres))
  {
    return fail(error, "sample spacing must be positive");
  }

  const Box box = bounds(polygon);
  const double width = toMetres(box.maxX - box.minX);
  const double height = toMetres(box.maxY - box.minY);

  if (!(width > 0.0) || !(height > 0.0))
  {
    return fail(error, "sample region has no extent");
  }

  if (PoissonDiskSampler::maximumSampleCount(settings.spacingMetres, width, height) > settings.maximumSamples)
  {
    return fail(error, "sample budget exceeded for this spacing");
  }

  PoissonDiskSampler::Settings inner;
  inner.radius = settings.spacingMetres;
  inner.width = width;
  inner.height = height;
  inner.seed = settings.seed;
  inner.method = settings.method == SampleMethod::Dense ? PoissonDiskSampler::Method::FillVoids : PoissonDiskSampler::Method::RandomThrows;

  PoissonDiskSampler sampler;
  const std::vector<Point2> raw = sampler.generate(inner);

  // The sampler fills a rectangle; the region is what the caller asked for.
  // Rejection rather than clipping, because moving a rejected sample to the
  // boundary would break the spacing guarantee the sampler just made.
  const int64_t margin = settings.marginMetres > 0.0 ? toUnit(settings.marginMetres) : 0;

  for (const auto &p : raw)
  {
    const double x = toMetres(box.minX) + p.x;
    const double y = toMetres(box.minY) + p.y;

    if (!representable(x) || !representable(y))
    {
      continue;
    }

    const Point q{toUnit(x), toUnit(y)};

    if (!contains(polygon, q) || onBoundary(polygon.outer, q))
    {
      continue;
    }

    if (margin > 0)
    {
      // Cheap conservative margin: reject anything within the margin of the
      // bounding box edges as well as of the polygon boundary.
      if (q.x - box.minX < margin || box.maxX - q.x < margin || q.y - box.minY < margin || box.maxY - q.y < margin)
      {
        continue;
      }
    }

    out.push_back(q);
  }

  return true;
}

bool thinToCount(const std::vector<Point> &candidates, const size_t count, std::vector<Point> &out, const double regionAreaMetres, std::string *error)
{
  out.clear();

  if (count == 0)
  {
    return true;
  }

  if (candidates.size() < count)
  {
    return fail(error, "fewer candidates than the requested count");
  }

  std::vector<Point2> input;
  input.reserve(candidates.size());

  for (const auto &p : candidates)
  {
    input.push_back({toMetres(p.x), toMetres(p.y)});
  }

  WeightedSampleElimination elimination;
  WeightedSampleElimination::Settings settings;
  settings.targetCount = count;

  // The area sets the radius the weight falloff is measured against. A wrong
  // one still orders the samples correctly but scales the falloff wrongly, so
  // it is taken from the caller rather than guessed from the bounding box.
  settings.regionArea = regionAreaMetres > 0.0 ? regionAreaMetres : 1.0;

  const std::vector<Point2> kept = elimination.eliminate(input, {}, settings);

  for (const auto &p : kept)
  {
    if (representable(p.x) && representable(p.y))
    {
      out.push_back({toUnit(p.x), toUnit(p.y)});
    }
  }

  return true;
}

} // namespace procedural::geom2
