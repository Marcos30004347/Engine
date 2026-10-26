#include "editor/virtualgeometry/CityVoxelPartOwnership.hpp"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <unordered_set>

namespace virtualgeometry {
namespace {
using Cell=std::array<int32_t,3>;
struct Hash {
  size_t operator()(const Cell &cell) const {
    uint64_t h=14695981039346656037ull;
    for(int32_t coordinate:cell) {
      const uint32_t value=uint32_t(coordinate);
      for(unsigned byte=0;byte<4;++byte) {
        h^=uint8_t(value>>(byte*8));h*=1099511628211ull;
      }
    }
    return size_t(h);
  }
};
bool fail(std::string *error,const char *message) {
  if(error)*error=message;
  return false;
}
}

bool resolveCityVoxelPartOwnership(std::vector<CityVoxelAuthoredPart> &parts,
    CityVoxelPartOwnershipReport &report,std::string *error,bool permitFullyCovered) {
  report={};
  if(error)error->clear();
  if(parts.empty())return fail(error,"Voxel ownership has no authored parts");
  std::unordered_set<uint64_t> ids;
  uint64_t total=0;
  for(const auto &part:parts) {
    if(!part.stableId || !ids.insert(part.stableId).second || part.cells.empty())
      return fail(error,"Voxel ownership has empty or duplicate part identity");
    total+=part.cells.size();
    if(total>kCityVoxelDwellingCellBudget)
      return fail(error,"Voxel ownership batch exceeds its cell budget");
  }
  struct Winner { uint32_t part; uint16_t priority; uint64_t stableId; };
  std::unordered_map<Cell,Winner,Hash> winner;
  winner.reserve(size_t(total));
  uint64_t equalPriority=0;
  for(uint32_t i=0;i<parts.size();++i) {
    const auto &part=parts[i];
    for(const auto &cell:part.cells) {
      auto inserted=winner.emplace(cell.position,Winner{i,part.priority,part.stableId});
      if(inserted.second)continue;
      auto &previous=inserted.first->second;
      if(previous.part==i)
        return fail(error,"Authored part contains duplicate occupied cell");
      if(previous.priority==part.priority)++equalPriority;
      if(part.priority>previous.priority ||
          (part.priority==previous.priority && part.stableId<previous.stableId))
        previous={i,part.priority,part.stableId};
    }
  }
  std::vector<uint32_t> retained(parts.size(),0);
  for(const auto &[cell,owner]:winner)++retained[owner.part];
  for(uint32_t i=0;i<retained.size();++i)
    if(!retained[i])report.fullyCoveredPartIndices.push_back(i);
  const auto covered=std::find(retained.begin(),retained.end(),0);
  if(covered!=retained.end() && !permitFullyCovered) {
    report.inputCells=total;
    report.uniqueCells=winner.size();
    report.clippedCells=total-winner.size();
    report.equalPriorityConflicts=equalPriority;
    report.fullyCoveredPartIndex=uint32_t(covered-retained.begin());
    if(error)*error="Authored part "+std::to_string(uint32_t(covered-retained.begin()))+
      " (stable ID "+std::to_string(parts[size_t(covered-retained.begin())].stableId)+
      ") is entirely covered by higher-priority parts";
    return false;
  }
  for(uint32_t i=0;i<parts.size();++i) {
    auto &cells=parts[i].cells;
    cells.erase(std::remove_if(cells.begin(),cells.end(),
        [&](const voxel::svraster::CityVoxelCell &cell) {
          return winner.find(cell.position)->second.part!=i;
        }),cells.end());
  }
  report.inputCells=total;
  report.uniqueCells=winner.size();
  report.clippedCells=total-winner.size();
  report.equalPriorityConflicts=equalPriority;
  if(covered!=retained.end())report.fullyCoveredPartIndex=uint32_t(covered-retained.begin());
  return true;
}
} // namespace virtualgeometry
