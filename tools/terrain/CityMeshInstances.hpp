#pragma once
#include "procedural/world/WorldCityModel.hpp"
#include "json.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>

// Legacy consumers can request the expanded mesh for comparisons and CPU renders.
inline bool buildPreviewCityModel(const procedural::world::rewrite::CityCompletionPlan &city,
    const procedural::world::rewrite::SettlementFabric &fabric, procedural::world::rewrite::CityModelSettings settings,
    procedural::world::rewrite::CityModelPlan &model, std::string *error,
    const procedural::world::rewrite::CityReliefPlan *relief=nullptr) {
  settings.instanceDwellingPanels=std::getenv("CITY_FLAT_MESH")==nullptr;
  return procedural::world::rewrite::buildCityModel(city,fabric,settings,model,error,relief);
}

inline void writeCityInstances(const std::filesystem::path &path,
    const procedural::world::rewrite::CityModelPlan &model,nlohmann::json &document) {
  if(model.quadInstances.empty())return;
  const auto &first=model.quadInstances.front();
  const std::array<double,3> origin{first[0],first[1],first[2]};
  const auto name=path.stem().string()+"-instances.bin";
  std::ofstream file(path.parent_path()/name,std::ios::binary);
  file.exceptions(std::ios::failbit|std::ios::badbit);
  // Stream records: no second city-sized temporary export array.
  for(const auto &q:model.quadInstances) {
    std::array<float,22> packed;
    for(size_t k=0;k<22;++k)packed[k]=float(q[k]-(k<12?origin[k%3]:0));
    file.write(reinterpret_cast<const char *>(packed.data()),sizeof(packed));
  }
  document["quadInstances"]={{"file",name},{"count",model.quadInstances.size()},{"stride",88},{"origin",origin}};
}
