#include "procedural/world/generator/CellBlock.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace procedural::world::generator {

namespace {

using geom2::Point;
using rewrite::Position;

bool fail(std::string *error, const std::string &message)
{
  if (error) *error = message;
  return false;
}

double metres(const int64_t q) { return geom2::toMetres(q); }

Position position(const Point &p, const double y)
{
  return Position{metres(p.x), y, metres(p.y)};
}

std::vector<Position> positions(const geom2::Ring &ring, const double y)
{
  std::vector<Position> out;
  for (const Point &p : ring) out.push_back(position(p, y));
  return out;
}

double levelAt(const CellBlockBrief &brief, const Point &centre)
{
  double level = 0;
  for (const CellBlockLevel &region : brief.levels)
    if (geom2::contains(region.region, centre)) level = region.elevation;
  return level;
}

uint32_t longestEdge(const geom2::Ring &ring)
{
  uint32_t best = 0;
  double length = -1;
  for (uint32_t k = 0; k < uint32_t(ring.size()); ++k)
  {
    const Point &a = ring[k], &b = ring[(k + 1) % ring.size()];
    const double d = std::hypot(metres(b.x - a.x), metres(b.y - a.y));
    if (d > length) { length = d; best = k; }
  }
  return best;
}

} // namespace

bool planCellBlock(const CellBlockBrief &brief, CellBlockPlan &out, Ledger &ledger, std::string *error)
{
  out = CellBlockPlan{};
  out.id = brief.id;
  out.cellMetres = brief.cellMetres;
  if (brief.block.empty() || brief.cellMetres < .5) return fail(error, "a cell block needs a polygon and cells at least half a metre wide");
  if (!geom2::polygonFault(brief.block).empty()) return fail(error, "the block polygon is not sound");
  std::vector<uint8_t> passageOwners(brief.lots.size());
  for(const auto &passage:brief.passages) {
    if(std::count_if(brief.passages.begin(),brief.passages.end(),[&](const auto &other){return other.request.id==passage.request.id;})!=1)
      return fail(error,"covered passage IDs must be unique");
    if(passage.upperLot>=brief.lots.size() || brief.lots[passage.upperLot].storeys<2 || passageOwners[passage.upperLot]++)
      return fail(error,"a covered passage needs one owner with an upper storey");
    if(passage.request.polygon.empty() || !geom2::polygonFault(passage.request.polygon).empty())return fail(error,"invalid block passage polygon");
    for(auto p:passage.request.polygon.outer) {
      const double u=metres(p.x)/brief.cellMetres,v=metres(p.y)/brief.cellMetres;
      if(std::abs(u-std::round(u))>1e-6 || std::abs(v-std::round(v))>1e-6)return fail(error,"block passage must follow whole grid cells");
    }
  }

  const geom2::Box box = geom2::bounds(brief.block);
  const int64_t step = geom2::toUnit(brief.cellMetres);
  const int32_t c0 = int32_t(std::floor(double(box.minX) / step));
  const int32_t c1 = int32_t(std::ceil(double(box.maxX) / step));
  const int32_t r0 = int32_t(std::floor(double(box.minY) / step));
  const int32_t r1 = int32_t(std::ceil(double(box.maxY) / step));
  std::map<std::pair<int32_t, int32_t>, uint32_t> at;

  for (int32_t row = r0; row < r1; ++row)
  for (int32_t column = c0; column < c1; ++column)
  {
    const int64_t x0 = int64_t(column) * step, z0 = int64_t(row) * step;
    const Point centre{x0 + step / 2, z0 + step / 2};
    if (!geom2::contains(brief.block, centre)) continue;
    CellBlockCell cell;
    cell.row = row; cell.column = column; cell.kind = BlockCellKind::Paving;
    cell.elevation = levelAt(brief, centre);
    for (uint32_t l = 0; l < uint32_t(brief.lots.size()); ++l)
      if (geom2::contains(brief.lots[l].polygon, centre)) { cell.kind = BlockCellKind::Lot; cell.lot = l; break; }
    for(const auto &passage:brief.passages)if(geom2::contains(passage.request.polygon,centre)) {
      if(cell.kind!=BlockCellKind::Lot || cell.lot!=passage.upperLot)return fail(error,"passage requires an explicit upper allocation in its owner lot");
      if(std::abs(cell.elevation-levelAt(brief,brief.lots[passage.upperLot].polygon.outer.front()))>1e-6)
        return fail(error,"covered passage needs level public ground");
      cell.kind=BlockCellKind::CoveredPassage;cell.upperLot=cell.lot;cell.lot=UINT32_MAX;cell.passage=passage.request.id;
    }
    cell.polygon = {{metres(x0), cell.elevation, metres(z0)}, {metres(x0 + step), cell.elevation, metres(z0)},
                    {metres(x0 + step), cell.elevation, metres(z0 + step)}, {metres(x0), cell.elevation, metres(z0 + step)}};
    at[{row, column}] = uint32_t(out.cells.size());
    if (cell.kind == BlockCellKind::Paving || cell.kind==BlockCellKind::CoveredPassage) out.pavedCells.push_back(uint32_t(out.cells.size()));
    out.cells.push_back(std::move(cell));
  }

  for(const auto &passage:brief.passages) {
    auto count=std::count_if(out.cells.begin(),out.cells.end(),[&](const auto &cell){return cell.kind==BlockCellKind::CoveredPassage && cell.passage==passage.request.id;});
    if(count==0 || std::abs(count*brief.cellMetres*brief.cellMetres-std::abs(geom2::areaMetres(passage.request.polygon)))>1e-5)
      return fail(error,"passage allocation was clipped or does not occupy whole cells");
  }
  out.pavedGraph.resize(out.cells.size());
  static const int d[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
  for (uint32_t p = 0; p < uint32_t(out.cells.size()); ++p)
  {
    CellBlockCell &cell = out.cells[p];
    for (const auto &q : d)
    {
      const auto found = at.find({cell.row + q[0], cell.column + q[1]});
      if (found == at.end()) continue;
      cell.neighbours.push_back(found->second);
      const auto publicCell=[](const CellBlockCell &c){return c.kind==BlockCellKind::Paving || c.kind==BlockCellKind::CoveredPassage;};
      if (publicCell(cell) && publicCell(out.cells[found->second]))
      {
        out.pavedGraph[p].push_back(found->second);
        if (found->second <= p || std::abs(cell.elevation - out.cells[found->second].elevation) < 1e-6) continue;
        const CellBlockCell &other = out.cells[found->second];
        const double rise = std::abs(other.elevation - cell.elevation);
        if (rise / brief.cellMetres > brief.maximumStairSlope) continue;
        const bool cellLower = cell.elevation < other.elevation;
        const CellBlockCell &lo = cellLower ? cell : other, &hi = cellLower ? other : cell;
        const auto centreOf = [](const CellBlockCell &c) {
          return Position{(c.polygon[0].x + c.polygon[2].x) * .5, c.elevation,
                          (c.polygon[0].z + c.polygon[2].z) * .5};
        };
        const Position a = centreOf(lo), b = centreOf(hi);
        const double dx = b.x - a.x, dz = b.z - a.z, n = std::hypot(dx, dz);
        const double px = -dz / n * .45, pz = dx / n * .45;
        CellBlockStair stair;
        stair.lowerCell = cellLower ? p : found->second;
        stair.upperCell = cellLower ? found->second : p;
        stair.flight.opening = {{a.x + px, a.y, a.z + pz}, {a.x - px, a.y, a.z - pz},
                                {b.x - px, b.y, b.z - pz}, {b.x + px, b.y, b.z + pz}};
        stair.flight.rise = rise;
        stair.flight.width = .9;
        stair.flight.risers = std::max(1u, uint32_t(std::ceil(rise / .18)));
        stair.flight.tread = n / stair.flight.risers;
        out.stairs.push_back(std::move(stair));
      }
    }
  }

  for (uint32_t l = 0; l < uint32_t(brief.lots.size()); ++l)
  {
    const CellBlockLotBrief &lot = brief.lots[l];
    if (lot.polygon.empty()) continue;
    CellBlockBuilding building;
    building.lot = l; building.stairStyle = lot.apartmentStair;
    const Point sample = lot.polygon.outer.front();
    building.elevation = levelAt(brief, sample);
    building.dwelling.id = lot.id; building.dwelling.seed = brief.seed ^ (lot.id * 0x9e3779b97f4a7c15ull);
    building.dwelling.building = lot.building; building.dwelling.lot = lot.polygon;
    building.dwelling.floors = lot.storeys; building.dwelling.frontage = {longestEdge(lot.polygon.outer)};
    building.dwelling.setbackMetres = brief.cellMetres * .35;
    for(const auto &passage:brief.passages)if(passage.upperLot==l) {
      building.dwelling.coveredPassage=passage.request;building.dwelling.setbackMetres=0;
      geom2::CellFrame frame;frame.cellMetres=brief.cellMetres;building.dwelling.sharedGrid=frame;
      building.dwelling.capacity=CellDwellingBrief::Capacity{std::abs(geom2::areaMetres(lot.polygon)),8,15,true,false};
    }

    if (lot.apartmentStair == ApartmentStairStyle::ZigZag && lot.storeys > 1)
    {
      const uint32_t e = building.dwelling.frontage.front();
      access::StackBrief stack;
      stack.wallFrom = position(lot.polygon.outer[e], building.elevation);
      stack.wallTo = position(lot.polygon.outer[(e + 1) % lot.polygon.outer.size()], building.elevation);
      stack.floors = lot.storeys; stack.ground = building.elevation;
      if (!access::planStack(stack, building.exteriorAccess, error)) return false;
    }
    out.buildings.push_back(std::move(building));
  }

  ledger.add(LedgerKind::Repaired, brief.id,
             "cell block: " + std::to_string(out.cells.size()) + " cells, " +
             std::to_string(out.pavedCells.size()) + " paved, " + std::to_string(out.stairs.size()) + " terrace stairs");
  return !out.cells.empty();
}

} // namespace procedural::world::generator
