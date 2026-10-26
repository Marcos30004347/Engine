#include "procedural/geometry/TerrainLandforms.hpp"
#include "procedural/geometry/TerrainMeshBuild.hpp"
#include "procedural/geometry/TerrainStreamPower.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
using namespace procedural;
using namespace procedural::geometry;
int main(int argc,char **argv)
{
  int failed=0;
  const auto check=[&](bool ok,const char *s){std::cout<<(ok?"PASS ":"FAIL ")<<s<<'\n';failed+=!ok;};
  RockFormation f;f.topMetres=600;f.faceWidthMetres=220;f.talusWidthMetres=550;
  f.controls={{2400,2400},{4200,2100},{5400,2800},{5200,4300},{3600,5100},{2300,4300}};
  SplineLandforms forms;forms.build({f},8000,75);
  check(forms.sample({3500,3500},100).height>597,"plateau top reaches authored elevation");
  check(forms.sample({100,100},100).height==100,"compact support leaves distant plains unchanged");
  bool finite=true,continuous=true;double maxSlope=0;int faces=0,talus=0;
  for(int y=500;y<7500;y+=53)for(int x=500;x<7500;x+=53)
  {
    const Point2 p{double(x),double(y)};const auto s=forms.sample(p,100),near=forms.sample({p.x+.001,p.y},100);
    finite&=std::isfinite(s.height)&&s.height>=100&&s.height<603;
    continuous&=std::abs(near.height-s.height)<.01;
    maxSlope=std::max(maxSlope,std::abs(near.height-s.height)/.001);
    faces+=s.role==SurfaceRole::Face;talus+=s.role==SurfaceRole::Talus;
  }
  check(finite&&continuous&&maxSlope>1,"finite continuous surface has a steep cliff without vertical steps");
  check(faces>0&&talus>0&&forms.sample({3500,3500},100).hardness>4,"face, talus and resistant cap are distinct");
  std::reverse(f.controls.begin(),f.controls.end());SplineLandforms reversed;reversed.build({f},8000,75);
  check(std::abs(reversed.sample({3500,3500},100).height-forms.sample({3500,3500},100).height)<1e-9,
      "either spline winding produces the same plateau");
  bool rejects=false;try{auto bad=f;bad.faceWidthMetres=0;SplineLandforms s;s.build({bad},8000,75);}catch(const std::invalid_argument&){rejects=true;}
  check(rejects,"invalid profile rejected");
  auto ramp=f;ramp.rampWidthMetres=2400;ramp.rampAzimuth=0;
  SplineLandforms ramps;ramps.build({ramp},8000,75);
  bool rampContinuous=true;
  for(int y=2000;y<6500;y+=47)for(int x=1500;x<7900;x+=47)
  { const auto h=ramps.sample({double(x),double(y)},100).height;
    rampContinuous &= std::abs(ramps.sample({x+.0001,double(y)},100).height-h)<.002; }
  check(rampContinuous,"directional access slope is continuous through concave footprint regions");
  auto granite=f;granite.lithology=PlanetLithology::Granite;granite.cutCap=true;
  SplineLandforms graniteCap;graniteCap.build({granite},8000,75);
  check(std::abs(graniteCap.sample({3933,3533},950).height-600)<.51,
      "granite summit can be trimmed into a flat bench before erosion");
  f.upliftMetresPerYear=.004;
  const auto nested=nestRockFormations({f},3,75);
  check(nested.size()==3,"three cliff levels fit inside a broad root cap");
  forms.build(nested,8000,75);
  auto upliftOverride=nested;upliftOverride.back().upliftMetresPerYear=.02;
  SplineLandforms coherent;coherent.build(upliftOverride,8000,75);
  check(std::abs(coherent.sample({3933,3533},100).upliftMetresPerYear-.004)<1e-12,
      "nested tiers inherit coherent parent uplift instead of rebuilding a ridge dome");
  check(std::count_if(forms.chains().begin(),forms.chains().end(),[](const Chain &c){return c.closed;})==15,"each nested tier contributes brink, face and toe constraints");
  const auto summit=forms.sample({3933,3533},100);
  check(std::abs(summit.height-nested.back().topMetres)<.51,"nested summit resolves above parent cap");
  const auto flatA=forms.sample({3933,3533},100).height;
  const auto flatB=forms.sample({3943,3533},250).height;
  check(std::abs(flatA-flatB)<.02,"cap flattens underlying slope independently of lithology");
  bool rejectedParent=false;
  try{auto bad=nested;bad[1].controls[0]={7500,7500};SplineLandforms invalid;invalid.build(bad,8000,75);}
  catch(const std::invalid_argument&){rejectedParent=true;}
  check(rejectedParent,"nested cliff escaping its parent cap is rejected");
  {
    constexpr uint32_t side=32;const size_t count=side*side;
    std::vector<double> heights(count),hardness(count),soft(count,1),rain(count,1),uplift(count,0);
    std::vector<uint8_t> outlets(count);
    for(uint32_t y=0;y<side;++y)for(uint32_t x=0;x<side;++x)
    {
      const size_t i=y*side+x;const auto value=forms.sample({(x+.5)*250,(y+.5)*250},100);
      heights[i]=value.height;hardness[i]=value.hardness;
      outlets[i]=x==0||y==0||x+1==side||y+1==side;
    }
    auto resistant=heights,weak=heights;StreamPowerSettings erosion;
    erosion.iterations=1;erosion.maximumTimeStep=.1;erosion.diffusion=0;
    const auto rockReport=erodeStreamPower(side,250,uplift,rain,hardness,outlets,resistant,erosion);
    const auto softReport=erodeStreamPower(side,250,uplift,rain,soft,outlets,weak,erosion);
    check(rockReport.erodedCubicMetres>0 && rockReport.erodedCubicMetres<softReport.erodedCubicMetres,
        "nested rock resistance reduces incision but still participates in erosion");
  }
  auto chains=forms.chains();Chain rim;rim.closed=true;rim.kind=ChainKind::WorldBorder;
  rim.points={{0,0},{8000,0},{8000,8000},{0,8000}};chains.push_back(rim);
  TerrainMeshSettings settings;settings.tileSizeMetres=4000;settings.workerCount=2;
  settings.preserveChainVertices=true;settings.poissonSpacingMetres=200;settings.poissonLayers=1;
  settings.verticalToleranceMetres=4;settings.maximumRefinementRounds=5;settings.minimumEdgeMetres=15;
  TerrainMesh mesh;std::string error;
  const auto height=[&](Point2 p){return forms.sample(p,100).height;};
  const auto material=[&](Point2 p){return uint8_t(forms.sample(p,100).role);};
  const bool built=buildTerrainMesh(chains,height,material,8000,settings,mesh,&error);
  check(built,"spline rings crossing four tiles triangulate");if(!built)std::cerr<<error<<'\n';
  if(built)
  {
    check(mesh.statistics.constraintsIncluded==mesh.statistics.constraintCount,"all brink and toe constraints retained");
    for(const auto &tile:mesh.tiles)for(const auto &v:tile.vertices)finite&=std::isfinite(v.y);
    check(finite,"meshed formation has finite vertices");
    bool seams=true;
    for(const auto &a:mesh.tiles)for(const auto &b:mesh.tiles)
    {
      const int side=a.tileX+1==b.tileX&&a.tileZ==b.tileZ?3:
          a.tileZ+1==b.tileZ&&a.tileX==b.tileX?1:-1;
      if(side<0)continue;const auto &left=a.borders[side],&right=b.borders[side==3?2:0];
      seams &= left.size()==right.size();
      for(size_t i=0;i<std::min(left.size(),right.size());++i)
        seams &= std::hypot(left[i].x-right[i].x,left[i].y-right[i].y)<1e-8;
    }
    check(seams,"shared tile borders agree through cliff rings");
    std::cout<<"triangles "<<mesh.triangleCount()<<'\n';
  }
  if(argc>1)
  {
    std::ofstream out(argv[1],std::ios::binary);const uint32_t side=512;out.write(reinterpret_cast<const char*>(&side),4);
    for(uint32_t y=0;y<side;++y)for(uint32_t x=0;x<side;++x)
    {const float h=float(height({(x+.5)*8000/side,(y+.5)*8000/side}));out.write(reinterpret_cast<const char*>(&h),4);}
  }
  return failed?1:0;
}
