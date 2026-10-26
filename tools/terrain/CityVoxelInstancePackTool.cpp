#include "editor/virtualgeometry/CityVoxelCityAssembler.hpp"
#include "editor/virtualgeometry/CityVoxelReleaseAudit.hpp"
#include "editor/virtualgeometry/CityVoxelScenePackager.hpp"
#include "voxel/svraster/CityVoxelSceneFormat.hpp"
#include "json.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

using nlohmann::json;
using namespace virtualgeometry;

namespace {
uint64_t mix(uint64_t h,uint64_t value) {
  for(unsigned byte=0;byte<8;++byte) {
    h^=uint8_t(value>>(8*byte));h*=1099511628211ull;
  }
  return h;
}
std::array<int32_t,3> cell3(const json &value) {
  if(!value.is_array() || value.size()!=3)
    throw std::runtime_error("Expected three integer voxel coordinates");
  return {value[0].get<int32_t>(),value[1].get<int32_t>(),value[2].get<int32_t>()};
}
uint64_t id(const json &value) {
  if(value.is_string())return std::stoull(value.get<std::string>());
  return value.get<uint64_t>();
}
}

int main(int argc,char **argv) {
  if(argc==4 && std::string(argv[1])=="--template") {
    voxel::svraster::CityVoxelSceneFileReader reader;
    std::string error;
    if(!reader.open(argv[2],{},&error)) {
      std::cerr<<error<<'\n';return 1;
    }
    json recipe=json::array();
    for(const auto &part:reader.index().instances) {
      if(part.quarterTurn) {
        std::cerr<<"Template library contains rotated placements\n";return 1;
      }
      recipe.push_back({{"geometryId",part.geometryId},
          {"appearanceId",part.appearanceId},
          {"localOriginCell",part.gridPosition}});
    }
    json output={{"schema",1},{"archetypes",{{"template",recipe}}},
        {"owners",json::array()}};
    std::ofstream file(argv[3]);
    file<<output.dump(2)<<'\n';
    if(!file) {std::cerr<<"Cannot write placement template\n";return 1;}
    std::cout<<"Wrote "<<recipe.size()<<" part references\n";
    return 0;
  }
  if(argc!=4) {
    std::cerr<<"Usage: CityVoxelInstancePackTool library.cvscene placements.json output.cvscene\n"
        <<"       CityVoxelInstancePackTool --template library.cvscene template.json\n";
    return 2;
  }
  try {
    std::ifstream manifestStream(argv[2]);
    if(!manifestStream)throw std::runtime_error("Cannot open city placement manifest");
    json manifest=json::parse(manifestStream);
    if(manifest.value("schema",0)!=1 || !manifest.contains("archetypes") ||
        !manifest["archetypes"].is_object() || !manifest.contains("owners") ||
        !manifest["owners"].is_array())
      throw std::runtime_error("Invalid city placement manifest schema");
    voxel::svraster::CityVoxelSceneFileReader library;
    std::string error;
    if(!library.open(argv[1],{},&error))throw std::runtime_error(error);
    std::vector<voxel::svraster::CityVoxelCompiledGeometry> geometry(library.index().geometry.size());
    std::vector<voxel::svraster::CityVoxelCompiledAppearance> appearances(library.index().appearances.size());
    for(uint32_t i=0;i<geometry.size();++i) {
      const auto &record=library.index().geometry[i];
      auto &asset=geometry[i];
      asset.id=record.id;
      asset.contentHash=record.contentHash;
      asset.addressHash=record.voxelAddressHash;
      asset.pitchMicrometres=record.pitchMicrometres;
      for(unsigned axis=0;axis<3;++axis)
        asset.extent[axis]=record.maximumCell[axis]-record.minimumCell[axis];
    }
    for(uint32_t i=0;i<appearances.size();++i) {
      const auto &record=library.index().appearances[i];
      auto &asset=appearances[i];
      asset.id=record.id;
      asset.contentHash=record.contentHash;
      asset.geometryAddressHash=record.geometryAddressHash;
      asset.geometryId=record.geometryId;
      asset.codec=record.codec;
    }
    CityVoxelCityAssembler builder(geometry,appearances,library.index().nominalPitchMicrometres);
    std::vector<CityVoxelExpectedOwner> expected;
    expected.reserve(manifest["owners"].size());
    uint64_t sourceHash=mix(14695981039346656037ull,library.index().sourceHash);
    for(const auto &owner:manifest["owners"]) {
      const uint64_t ownerId=id(owner.at("id"));
      const std::string archetype=owner.at("archetype").get<std::string>();
      if(!manifest["archetypes"].contains(archetype))
        throw std::runtime_error("Owner references an unknown archetype");
      const auto &recipe=manifest["archetypes"][archetype];
      if(!recipe.is_array() || recipe.empty() || recipe.size()>UINT32_MAX)
        throw std::runtime_error("Archetype must contain part references");
      std::vector<CityVoxelCityAssembler::Part> parts;
      parts.reserve(recipe.size());
      for(uint32_t i=0;i<recipe.size();++i) {
        const auto &part=recipe[i];
        uint64_t stable=mix(mix(14695981039346656037ull,ownerId),i+1u);
        if(!stable)stable=1;
        parts.push_back({stable,id(part.at("geometryId")),
            id(part.at("appearanceId")),cell3(part.at("localOriginCell"))});
      }
      const auto origin=cell3(owner.at("originCell"));
      const uint32_t requestedTurn=owner.value("quarterTurn",0u);
      if(requestedTurn>3u)throw std::runtime_error("Owner quarter turn exceeds 0..3");
      const uint8_t turn=uint8_t(requestedTurn);
      if(!builder.addOwner(ownerId,origin,turn,parts,&error))
        throw std::runtime_error("Owner "+std::to_string(ownerId)+": "+error);
      expected.push_back({ownerId,owner.value("category",std::string("building"))});
      sourceHash=mix(sourceHash,ownerId);
      for(int32_t coordinate:origin)sourceHash=mix(sourceHash,uint32_t(coordinate));
      sourceHash=mix(sourceHash,turn);
      for(const auto &part:parts) {
        sourceHash=mix(sourceHash,part.geometryId);
        sourceHash=mix(sourceHash,part.appearanceId);
        for(int32_t coordinate:part.localOriginCell)
          sourceHash=mix(sourceHash,uint32_t(coordinate));
      }
    }
    CityVoxelStaticScene city;
    std::unordered_map<uint64_t,uint64_t> ownerByPlacement;
    if(!builder.finish(city,ownerByPlacement,&error))throw std::runtime_error(error);
    if(!sourceHash)sourceHash=1;
    if(!repackCityVoxelSceneInstances(argv[1],argv[3],city,ownerByPlacement,
        sourceHash,0x4349565000000001ull,{},&error))
      throw std::runtime_error(error);
    CityVoxelReleaseAuditReport report;
    if(!auditCityVoxelSceneForRelease(argv[3],expected,{},1'000'000u,report,&error))
      throw std::runtime_error("Packed city failed release audit: "+error);
    const auto stats=builder.stats();
    std::cout<<"Packed "<<stats.owners<<" owners and "<<stats.placements
        <<" placements from "<<stats.sharedGeometry<<" shared geometries and "
        <<stats.appearances<<" appearances ("<<stats.placementBytes
        <<" compact placement bytes, "<<report.checkedPages<<" checked pages)\n";
    return 0;
  } catch(const std::exception &exception) {
    std::cerr<<exception.what()<<'\n';
    return 1;
  }
}
