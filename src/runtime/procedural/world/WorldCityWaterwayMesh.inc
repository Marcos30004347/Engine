// Included beside the native city's prism and triangle builders.
std::vector<Position> waterwayBox(const CityWaterway &w,double a,double b,double left,double right)
{
  const double dx=w.b.x-w.a.x,dz=w.b.z-w.a.z,L=std::hypot(dx,dz);
  const auto p=[&](double t,double c){return Position{w.a.x+dx*t/L-dz*c/L,0,w.a.z+dz*t/L+dx*c/L};};
  return {p(a,left),p(b,left),p(b,right),p(a,right)};
}

void waterwayMesh(CityModelPlan &out,VertexIndex &index,const CityWaterway &w)
{
  const double L=std::hypot(w.b.x-w.a.x,w.b.z-w.a.z),outer=w.halfWidth+w.bankWidth;
  prism(out,index,waterwayBox(w,0,L,-outer,outer),w.bedLevel-.2,w.bedLevel,31,32);
  auto surface=waterwayBox(w,0,L,-w.halfWidth,w.halfWidth);
  for(auto &p:surface)p.y=w.waterLevel;
  for(auto f:std::vector<std::array<Position,3>>{{surface[0],surface[2],surface[1]},{surface[0],surface[3],surface[2]}}){triangle(out,index,f,56);out.surfaceMaterials.back()=28;}
  std::vector<double> cuts{0,L};for(double t=2;t<L;t+=2)cuts.push_back(t);
  for(auto b:w.bridges){cuts.push_back(b.first);cuts.push_back(b.second);}
  std::sort(cuts.begin(),cuts.end());cuts.erase(std::unique(cuts.begin(),cuts.end()),cuts.end());
  for(size_t i=1;i<cuts.size();++i){
    const double a=cuts[i-1],b=cuts[i],mid=(a+b)*.5;if(b-a<1e-5)continue;
    const auto centre=waterwayBox(w,mid,mid,0,0)[0];const double level=w.bankGround(centre.x,centre.z);
    bool crossing=false;for(auto bridge:w.bridges)if(mid>=bridge.first && mid<=bridge.second)crossing=true;
    for(int side:{-1,1}) {
      const double left=side<0?-outer:w.halfWidth,right=side<0?-w.halfWidth:outer;
      prism(out,index,waterwayBox(w,a,b,left,right),w.bedLevel-.2,level+(crossing?-.28:.48),31,32);
    }
    if(crossing)prism(out,index,waterwayBox(w,a,b,-outer,outer),level-.3,level+.04,31,32);
  }
  // Retained ends make this a contained reach until a hydrological inlet /
  // outlet is explicitly planned. They are not open holes into solid ground.
  for(double t:{0.,L-.3}){auto p=waterwayBox(w,t,t,0,0)[0];prism(out,index,waterwayBox(w,t,t+.3,-outer,outer),w.bedLevel-.2,w.bankGround(p.x,p.z)+.04,31,32);}
  for(auto bridge:w.bridges)for(double edge:{bridge.first,bridge.second}) {
    if(edge<=.01 || edge>=L-.01)continue;
    auto p=waterwayBox(w,edge,edge,0,0)[0];const double h=w.bankGround(p.x,p.z);
    prism(out,index,waterwayBox(w,edge-.12,edge+.12,-w.halfWidth,w.halfWidth),h+.04,h+.85,31,32);
  }
}
