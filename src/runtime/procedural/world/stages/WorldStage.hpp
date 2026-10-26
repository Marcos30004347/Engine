#pragma once

#include "procedural/world/WorldPipeline.hpp"

#include <memory>
#include <string>
#include <vector>

namespace procedural::world::stages {

// The world map as small generator classes.
//
// This was one 368-line function with a thirteen-way switch over a stage index.
// Every stage already had a name, a progress label and a clean boundary; what
// it did not have was a type, so a stage could not be listed, replaced,
// tested on its own, or reasoned about without reading the twelve around it.
//
// Each class below is one stage and nothing else. No kernel is rewritten here:
// a stage's run() is the body its case had, which is what keeps the frozen
// graph signature identical through the change.

// Everything a stage reads that is not the world itself. Passed rather than
// captured, so a stage has no hidden inputs.
struct Context
{
  const WorldPipelineSettings &settings;
  double cellSpacingMetres = 0;
  uint32_t hierarchyLevels = 1;
  ClimateSettings climate;
  WorldPipelineCallbacks callbacks;

  void phase(const char *name, double progress) const;

  bool cancelled() const;
};

Context makeContext(const WorldPipelineSettings &, const WorldPipelineCallbacks &);

// One stage of the world map.
//
// Deliberately small: a name, a progress label, and one method that advances
// the world. A new stage is a new class, not a new case in a switch and a new
// entry in a parallel array of names.
class WorldStage
{
public:
  virtual ~WorldStage() = default;

  // Stable identity, and what the product key is built from. Never a display
  // string: the progress label is separate because it changes freely.
  virtual const char *name() const = 0;

  virtual const char *phase() const = 0;

  virtual double progress() const = 0;

  // Advances `world` in place. The caller owns a private candidate, so a
  // stage may mutate freely; a failure leaves the candidate discarded.
  virtual bool run(const Context &, GeneratedWorld &, std::string *error) = 0;
};

using StagePtr = std::shared_ptr<WorldStage>;

// The thirteen stages in order. Each is one small class; the comment on each
// says what it owns, not how it works.

// A ladder of Voronoi partitions, coarsest first, each cut inside the one
// above it. The finest is what every later stage measures.
class PartitionStage final : public WorldStage
{
public:
  const char *name() const override
  {
    return "partition";
  }

  const char *phase() const override
  {
    return "Partitioning";
  }

  double progress() const override
  {
    return 0.05;
  }

  bool run(const Context &, GeneratedWorld &, std::string *) override;
};

// Which cells are land. Grown over the coarse partition, or cut from an
// analytic field when the brief asks for an archetype shape.
class LandmassStage final : public WorldStage
{
public:
  const char *name() const override
  {
    return "landmass";
  }

  const char *phase() const override
  {
    return "Raising land";
  }

  double progress() const override
  {
    return 0.45;
  }

  bool run(const Context &, GeneratedWorld &, std::string *) override;
};

// Clips the cells to the world extent and traces the shoreline.
class CoastStage final : public WorldStage
{
public:
  const char *name() const override
  {
    return "coast";
  }

  const char *phase() const override
  {
    return "Clipping and tracing the coast";
  }

  double progress() const override
  {
    return 0.6;
  }

  bool run(const Context &, GeneratedWorld &, std::string *) override;
};

// Elevation and uplift, then a probe drainage run on a copy so that broad
// plains and their floodplains can be conditioned before the real rivers.
class ElevationStage final : public WorldStage
{
public:
  const char *name() const override
  {
    return "elevation-conditioning";
  }

  const char *phase() const override
  {
    return "Elevation and uplift";
  }

  double progress() const override
  {
    return 0.7;
  }

  bool run(const Context &, GeneratedWorld &, std::string *) override;
};

// Stream-power erosion: the valleys and the ranges, against the uplift the
// elevation stage authored. Off unless the brief asks for it.
class FluvialStage final : public WorldStage
{
public:
  const char *name() const override
  {
    return "fluvial";
  }

  const char *phase() const override
  {
    return "Fluvial relief";
  }

  double progress() const override
  {
    return 0.725;
  }

  bool run(const Context &, GeneratedWorld &, std::string *) override;
};

// Plateau blocks, before the definitive drainage pass, and the site index.
class PlateauStage final : public WorldStage
{
public:
  const char *name() const override
  {
    return "plateaus-index";
  }

  const char *phase() const override
  {
    return "Plateau blocks";
  }

  double progress() const override
  {
    return 0.73;
  }

  bool run(const Context &, GeneratedWorld &, std::string *) override;
};

// Climate before the rivers, because precipitation is what drives them.
class BaseClimateStage final : public WorldStage
{
public:
  const char *name() const override
  {
    return "climate-base";
  }

  const char *phase() const override
  {
    return "Climate";
  }

  double progress() const override
  {
    return 0.74;
  }

  bool run(const Context &, GeneratedWorld &, std::string *) override;
};

// Carves the closed basins that lakes need.
//
// fillLakes finds depressions; it does not make them. The elevation field is
// built from ramps and fractal noise, both of which are smooth, so a measured
// world had a maximum depression depth of 2.95 m over 1,547 land cells with a
// 99th percentile of zero - no basin anywhere came near the 20 m gate, and no
// setting on that gate could change it because there was nothing to find. Every
// world came out with three lake cells, which were tarns left by the coast
// trace rather than lakes.
//
// So the basins are placed deliberately, the way a tectonic or glacial basin
// would be, and the existing lake pass fills them. That keeps one rule for what
// holds water and one place that decides it.
class BasinStage final : public WorldStage
{
public:
  const char *name() const override
  {
    return "basins";
  }

  const char *phase() const override
  {
    return "Lake basins";
  }

  double progress() const override
  {
    return 0.745;
  }

  bool run(const Context &, GeneratedWorld &, std::string *) override;
};

// The definitive drainage pass, lake filling, and a re-traced coast.
class RiverStage final : public WorldStage
{
public:
  const char *name() const override
  {
    return "rivers-lakes";
  }

  const char *phase() const override
  {
    return "Rivers";
  }

  double progress() const override
  {
    return 0.8;
  }

  bool run(const Context &, GeneratedWorld &, std::string *) override;
};

// Ridges and a monotone relief remap, after drainage so it cannot break it.
class StylizeStage final : public WorldStage
{
public:
  const char *name() const override
  {
    return "stylization";
  }

  const char *phase() const override
  {
    return "Ridges and relief stylization";
  }

  double progress() const override
  {
    return 0.82;
  }

  bool run(const Context &, GeneratedWorld &, std::string *) override;
};

// The continuous elevation field and the contours read off it. Published only
// after the last scalar mutation, so every consumer sees one final surface.
class SurfaceStage final : public WorldStage
{
public:
  const char *name() const override
  {
    return "surface-contours";
  }

  const char *phase() const override
  {
    return "Continuous elevation, uplift and contours";
  }

  double progress() const override
  {
    return 0.83;
  }

  bool run(const Context &, GeneratedWorld &, std::string *) override;
};

// A second climate pass, now that rivers and lakes exist: riparian strips and
// lake shores are local moisture the first pass could not know about.
class FinalClimateStage final : public WorldStage
{
public:
  const char *name() const override
  {
    return "climate-final";
  }

  const char *phase() const override
  {
    return "Climate and land types";
  }

  double progress() const override
  {
    return 0.84;
  }

  bool run(const Context &, GeneratedWorld &, std::string *) override;
};

// Named regions over the finished map.
class RegionStage final : public WorldStage
{
public:
  const char *name() const override
  {
    return "regions";
  }

  const char *phase() const override
  {
    return "Regions and names";
  }

  double progress() const override
  {
    return 0.86;
  }

  bool run(const Context &, GeneratedWorld &, std::string *) override;
};

// Summarises the leaves back up the ladder and measures the result against
// what the brief asked for.
class SummaryStage final : public WorldStage
{
public:
  const char *name() const override
  {
    return "summary";
  }

  const char *phase() const override
  {
    return "Summary";
  }

  double progress() const override
  {
    return 0.95;
  }

  bool run(const Context &, GeneratedWorld &, std::string *) override;
};

// The pipeline: the stages in the order they must run.
//
// One list, so the names, the order and the implementations cannot drift apart
// the way a switch and a parallel array of strings can.
const std::vector<StagePtr> &worldMapStages();

} // namespace procedural::world::stages
