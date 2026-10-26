#include "procedural/world/WorldCityLandforms.hpp"
#include <iostream>
using namespace procedural::world::rewrite;
int main() {
  int failures=0,checks=0;
  auto check=[&](bool ok,const char *name){++checks;if(!ok){++failures;std::cerr<<"FAIL "<<name<<'\n';}};
  const auto ground=[](double x,double z){return .012*x+.008*z;};
  for(unsigned seed:{1,17,41,103}) {
    std::vector<CityLandformCell> cells;
    auto point=[&](int x,int z){return Position{x*40.+(x>0&&x<5?3*std::sin(x*17+z*7+seed):0),0,z*40.+(z>0&&z<5?3*std::cos(x*11+z*19+seed):0)};};
    for(int z=0;z<5;++z)for(int x=0;x<5;++x){CityLandformCell c;c.id=cells.size()+1;c.boundary={point(x,z),point(x+1,z),point(x+1,z+1),point(x,z+1)};cells.push_back(c);}
    CityLandformSettings s;s.reliefMetres=24;CityLandformPlan p;std::string why;
    check(planCityLandforms(cells,ground,{100,0,100},s,p,&why),"connected cell landform accepted");
    double radius=0;Position peak;
    for(const auto &c:p.cells){radius=std::max(radius,landform_detail::distance(c.centre,{100,0,100}));if(c.id==p.summit)peak=c.centre;}
    check(landform_detail::distance(peak,{100,0,100})<=radius*s.summitCoreFraction,"summit remains within the inhabited core");
    auto h=cityLandformSurface(ground,p);std::set<unsigned> shelves;
    for(const auto &c:p.cells) {
      shelves.insert(c.shelf);
      check(std::abs(h(c.centre.x,c.centre.z)-ground(c.centre.x,c.centre.z)-c.uplift)<1e-7,"cell core has its assigned uplift");
      for(size_t j=0;j<c.vertices.size();++j) {
        auto a=p.vertices[c.vertices[j]],b=p.vertices[c.vertices[(j+1)%c.vertices.size()]];
        for(double t:{.1,.3,.5,.7,.9}) {
          double x=a.x+(b.x-a.x)*t,z=a.z+(b.z-a.z)*t, len=std::hypot(b.x-a.x,b.z-a.z);
          double dx=1e-6*(b.z-a.z)/len,dz=-1e-6*(b.x-a.x)/len;
          check(std::abs(h(x+dx,z+dz)-h(x-dx,z-dz))<1e-4,"shared edge has no height crack");
        }
      }
      auto id=c.id;std::set<uint64_t> visited;
      while(id && visited.insert(id).second) {auto it=std::find_if(p.cells.begin(),p.cells.end(),[&](auto &q){return q.id==id;});check(it!=p.cells.end(),"uphill link resolves");if(it==p.cells.end())break;
        if(it->uphill){auto next=std::find_if(p.cells.begin(),p.cells.end(),[&](auto &q){return q.id==it->uphill;});check(next->shelf>=it->shelf,"uphill hierarchy is monotonic");}else check(id==p.summit,"connected hierarchy reaches summit");id=it->uphill;
      }
      check(!id,"uphill hierarchy has no cycles");
    }
    check(std::count_if(p.cells.begin(),p.cells.end(),[](const auto &c){return c.shelf==3;})>1,"summit reserves several cells rather than a narrow spike");
    check(shelves.size()==4,"several coherent shelf elevations");
    for(double x:{-20.,220.})for(double z:{-20.,50.,220.})check(h(x,z)==ground(x,z),"outside terrain unchanged");
    std::reverse(cells.begin(),cells.end());CityLandformPlan reverse;
    check(planCityLandforms(cells,ground,{100,0,100},s,reverse,&why),"reordered input accepted");
    auto h2=cityLandformSurface(ground,reverse);
    for(double x=0;x<=200;x+=7)for(double z=0;z<=200;z+=7)check(h(x,z)==h2(x,z),"order-independent terrain");
    auto invalid=s;invalid.reliefMetres=std::numeric_limits<double>::quiet_NaN();auto previous=p.summit;
    check(!planCityLandforms(cells,ground,{},invalid,p,&why)&&p.summit==previous,"invalid plan leaves previous output intact");
    s.reliefMetres=0;CityLandformPlan zero;check(planCityLandforms(cells,ground,{},s,zero),"zero relief accepted");auto z=cityLandformSurface(ground,zero);check(z(93,116)==ground(93,116),"zero relief preserves native terrain");
  }
  std::vector<CityLandformCell> t(3);t[0].id=1;t[0].boundary={{0,0,0},{40,0,0},{40,0,80},{0,0,80}};
  t[1].id=2;t[1].boundary={{40,0,0},{80,0,0},{80,0,40},{40,0,40}};
  t[2].id=3;t[2].boundary={{40,0,40},{80,0,40},{80,0,80},{40,0,80}};
  CityLandformSettings s;s.reliefMetres=12;CityLandformPlan p;
  check(planCityLandforms(t,ground,{20,0,40},s,p),"T junction accepted");
  check(p.cells[0].vertices.size()==5&&p.cells[0].neighbours.size()==2,"T junction split into shared conforming edges");
  auto h=cityLandformSurface(ground,p);for(double z=1;z<80;z+=1)check(std::abs(h(40-1e-6,z)-h(40+1e-6,z))<1e-4,"T junction continuous");
  t.push_back(t.front());check(!planCityLandforms(t,ground,{},s,p),"duplicate identities rejected");
  const std::vector<Position> waterCell={{0,0,0},{40,0,0},{40,0,40},{0,0,40}};
  check(cityLandformTouchesWater(waterCell,{-10,0,13},{50,0,13},1),"river crossing between dry corners protected");
  check(cityLandformTouchesWater(waterCell,{-10,0,-2},{50,0,-2},3),"river bank buffer protected");
  check(!cityLandformTouchesWater(waterCell,{-10,0,-4},{50,0,-4},3),"dry land outside bank remains available");
  check(cityLandformTouchesWater(waterCell,{12,0,12},{15,0,14},0),"contained water boundary protected");
  const auto flat=[](double,double){return 0.;};
  const auto hump=[](double x,double){return 10*std::sin(x*3.141592653589793/40);};
  check(cityLandformRouteCost({0,0,0},{40,0,0},flat,.12)==40,"flat road cost is length");
  check(cityLandformRouteCost({0,0,0},{40,0,0},hump,.12)>120,"route cost sees shoulders hidden by equal endpoint heights");
  check(std::abs(cityLandformRouteCost({0,0,0},{40,0,0},hump,.12)-cityLandformRouteCost({40,0,0},{0,0,0},hump,.12))<1e-7,"route cost symmetric");
  check(!std::isfinite(cityLandformRouteCost({0,0,0},{40,0,0},[](double,double){return NAN;},.12)),"non-finite route terrain rejected");
  std::cout<<checks-failures<<" passed, "<<failures<<" failed\n";return failures?1:0;
}
