#pragma once

#include "procedural/world/WorldDwellingPlan.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace procedural::world::generator {

// A transition between two rectangular runs at one level. `a` and `b` are the
// open end edges of those runs. The convex polygon between them is the joiner:
// stood upright it closes an angled wall/tower gap; laid flat it is the landing
// on which one stair flight turns into the next. Nothing assumes a right angle.
struct AngularJoiner
{
  std::array<rewrite::Position, 2> a{}, b{};
  std::vector<rewrite::Position> polygon;
  std::vector<std::array<rewrite::Position, 3>> triangles;
  double turnRadians = 0;
};

bool buildAngularJoiner(const rewrite::Position &a0,
                        const rewrite::Position &a1,
                        const rewrite::Position &b0,
                        const rewrite::Position &b1,
                        AngularJoiner &out,
                        std::string *error = nullptr);

} // namespace procedural::world::generator
