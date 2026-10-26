#include "procedural/Geometry.hpp"
#include "procedural/geometry/TerrainLandforms.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace procedural::geometry
{
namespace
{
double smooth(double t) { t=std::clamp(t,0.0,1.0); return t*t*(3-2*t); }
Point2 catmull(Point2 a,Point2 b,Point2 c,Point2 d,double t)
{
  const auto f=[&](double w,double x,double y,double z)
  { return .5*(2*x+(-w+y)*t+(2*w-5*x+4*y-z)*t*t+(-w+3*x-3*y+z)*t*t*t); };
  return {f(a.x,b.x,c.x,d.x),f(a.y,b.y,c.y,d.y)};
}
double faceWidth(Point2 p,Point2 center,const RockFormation &s)
{
  const double dx=p.x-center.x,dy=p.y-center.y;
  const double len=std::max(1e-9,std::hypot(dx,dy));
  const double facing=std::max(0.0,(dx*std::cos(s.rampAzimuth)+dy*std::sin(s.rampAzimuth))/len);
  const double variation=1+.22*std::sin(p.x/700+s.seed%97)*std::sin(p.y/850+s.seed%71);
  return s.faceWidthMetres*variation+s.rampWidthMetres*facing*facing;
}
double signedDistance(Point2 p,const std::vector<Point2>& ring,Point2 &foot,Point2 &tangent)
{
  bool inside=false; double closest=std::numeric_limits<double>::max();
  for(size_t i=0,j=ring.size()-1;i<ring.size();j=i++)
  {
    const auto a=ring[j],b=ring[i]; const double dx=b.x-a.x,dy=b.y-a.y;
    const double t=std::clamp(((p.x-a.x)*dx+(p.y-a.y)*dy)/std::max(1e-12,dx*dx+dy*dy),0.0,1.0);
    const double ex=p.x-a.x-t*dx,ey=p.y-a.y-t*dy;
    const double dist=ex*ex+ey*ey;
    if(dist<closest){closest=dist;foot={a.x+t*dx,a.y+t*dy};tangent={dx,dy};}
    if((a.y>p.y)!=(b.y>p.y) && p.x<a.x+(b.x-a.x)*(p.y-a.y)/(b.y-a.y))inside=!inside;
  }
  return inside?-std::sqrt(closest):std::sqrt(closest);
}
}
// Segment `i` of a closed ring runs from `rim[i-1]` to `rim[i]`, matching the
// order the linear scan walked so the per-segment arithmetic is unchanged.
namespace
{
inline void rimSegment(const std::vector<Point2> &rim, const uint32_t index, Point2 &a, Point2 &b)
{
  a = rim[(index + rim.size() - 1u) % rim.size()];
  b = rim[index];
}
}
void SplineLandforms::RimTree::build(const std::vector<Point2> &rim)
{
  nodes.clear(); segments.clear();
  if (rim.size() < 2u) return;
  segments.resize(rim.size());
  for (uint32_t i = 0; i < segments.size(); ++i) segments[i] = i;
  std::vector<Point2> lo(segments.size()), hi(segments.size());
  for (uint32_t i = 0; i < segments.size(); ++i)
  {
    Point2 a, b; rimSegment(rim, i, a, b);
    lo[i] = {std::min(a.x, b.x), std::min(a.y, b.y)};
    hi[i] = {std::max(a.x, b.x), std::max(a.y, b.y)};
  }
  nodes.reserve(2u * segments.size());
  // Iterative median split on the wider axis; leaves hold at most eight
  // segments, below which the box tests cost more than the distances.
  struct Pending { uint32_t node, start, count; };
  nodes.push_back({});
  std::vector<Pending> stack{{0u, 0u, uint32_t(segments.size())}};
  while (!stack.empty())
  {
    const Pending job = stack.back(); stack.pop_back();
    double minX = std::numeric_limits<double>::max(), minY = minX;
    double maxX = std::numeric_limits<double>::lowest(), maxY = maxX;
    for (uint32_t k = job.start; k < job.start + job.count; ++k)
    {
      const uint32_t id = segments[k];
      minX = std::min(minX, lo[id].x); minY = std::min(minY, lo[id].y);
      maxX = std::max(maxX, hi[id].x); maxY = std::max(maxY, hi[id].y);
    }
    nodes[job.node].minX = minX; nodes[job.node].minY = minY;
    nodes[job.node].maxX = maxX; nodes[job.node].maxY = maxY;
    if (job.count <= 8u)
    { nodes[job.node].start = job.start; nodes[job.node].count = job.count; continue; }
    const bool splitX = (maxX - minX) >= (maxY - minY);
    const uint32_t middle = job.count / 2u;
    std::nth_element(segments.begin() + job.start, segments.begin() + job.start + middle,
        segments.begin() + job.start + job.count, [&](const uint32_t l, const uint32_t r)
        {
          const double a = splitX ? lo[l].x + hi[l].x : lo[l].y + hi[l].y;
          const double b = splitX ? lo[r].x + hi[r].x : lo[r].y + hi[r].y;
          return a != b ? a < b : l < r; // total order keeps the build deterministic
        });
    const uint32_t left = uint32_t(nodes.size());
    nodes.push_back({}); nodes.push_back({});
    nodes[job.node].count = 0u;
    nodes[job.node].right = left + 1u;
    stack.push_back({left, job.start, middle});
    stack.push_back({left + 1u, job.start + middle, job.count - middle});
  }
}
double SplineLandforms::RimTree::signedDistance(const Point2 p, const std::vector<Point2> &rim) const
{
  if (nodes.empty()) return std::numeric_limits<double>::max();
  double closest = std::numeric_limits<double>::max();
  bool inside = false;
  uint32_t stack[64]; uint32_t depth = 0; stack[depth++] = 0u;
  while (depth != 0u)
  {
    const Node &node = nodes[stack[--depth]];
    // Distance from the sample to this node's box; a box no nearer than the
    // best segment so far cannot hold a nearer one. The parity ray still has
    // to be followed, so only descend when neither query needs the node.
    const double gapX = std::max({node.minX - p.x, 0.0, p.x - node.maxX});
    const double gapY = std::max({node.minY - p.y, 0.0, p.y - node.maxY});
    const bool nearer = gapX * gapX + gapY * gapY < closest;
    const bool crossable = p.y >= node.minY && p.y <= node.maxY && p.x < node.maxX;
    if (!nearer && !crossable) continue;
    if (node.count == 0u)
    {
      // Left child sits immediately before the right one, and a median split
      // keeps the depth logarithmic, so the fixed stack cannot overflow.
      stack[depth++] = node.right;
      stack[depth++] = node.right - 1u;
      continue;
    }
    for (uint32_t k = node.start; k < node.start + node.count; ++k)
    {
      Point2 a, b; rimSegment(rim, segments[k], a, b);
      const double dx = b.x - a.x, dy = b.y - a.y;
      if (nearer)
      {
        const double t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / std::max(1e-12, dx * dx + dy * dy), 0.0, 1.0);
        const double ex = p.x - a.x - t * dx, ey = p.y - a.y - t * dy;
        closest = std::min(closest, ex * ex + ey * ey);
      }
      if (crossable && (a.y > p.y) != (b.y > p.y) && p.x < a.x + dx * (p.y - a.y) / dy) inside = !inside;
    }
  }
  return inside ? -std::sqrt(closest) : std::sqrt(closest);
}
size_t SplineLandforms::bucket(Point2 p) const
{
  const auto cell=[&](double x){return size_t(std::clamp(x/extent_*side_,0.0,double(side_-1)));};
  return cell(p.y)*side_+cell(p.x);
}
void SplineLandforms::build(std::vector<RockFormation> specs,double extent,double spacing)
{
  *this={};
  if(!std::isfinite(extent)||extent<=0||!std::isfinite(spacing)||spacing<=0)
    throw std::invalid_argument("invalid spline landform extent or spacing");
  extent_=extent; buckets_.resize(side_*side_);
  for(auto &s:specs)
  {
    if(s.controls.size()<3||!std::isfinite(s.topMetres)||!std::isfinite(s.faceWidthMetres)||
        !std::isfinite(s.talusWidthMetres)||!std::isfinite(s.stratumMetres)||
        s.faceWidthMetres<=0||s.talusWidthMetres<=0||s.stratumMetres<=0||
        !std::isfinite(s.rampWidthMetres)||s.rampWidthMetres<0||!std::isfinite(s.rampAzimuth)||
        uint32_t(s.lithology)>=kPlanetLithologyCount||
        !std::isfinite(s.capReliefMetres)||s.capReliefMetres<0||
        !std::isfinite(s.capRoundness)||s.capRoundness<0||s.capRoundness>1||
        !std::isfinite(s.upliftMetresPerYear)||s.upliftMetresPerYear<0||
        !std::isfinite(s.capResistance)||s.capResistance<1||
        (s.parent!=UINT32_MAX && (s.parent>=forms_.size()||s.topMetres<=forms_[s.parent].spec.topMetres)))
      throw std::invalid_argument("invalid spline landform profile");
    for(const auto p:s.controls)if(!std::isfinite(p.x)||!std::isfinite(p.y))
      throw std::invalid_argument("nonfinite landform control point");
    if(s.parent!=UINT32_MAX)s.upliftMetresPerYear=forms_[s.parent].spec.upliftMetresPerYear;
    Form form; form.spec=std::move(s); const auto &c=form.spec.controls;
    for(const auto p:c){form.center.x+=p.x/c.size();form.center.y+=p.y/c.size();}
    for(size_t i=0;i<c.size();++i)
    {
      const auto a=c[(i+c.size()-1)%c.size()],b=c[i],d=c[(i+2)%c.size()],next=c[(i+1)%c.size()];
      const uint32_t steps=uint32_t(std::clamp(std::ceil(distance(b,next)/spacing),2.0,256.0));
      for(uint32_t j=0;j<steps;++j)form.rim.push_back(catmull(a,b,next,d,double(j)/steps));
    }
    double area=0;
    for(size_t i=0,j=form.rim.size()-1;i<form.rim.size();j=i++)area+=form.rim[j].x*form.rim[i].y-form.rim[i].x*form.rim[j].y;
    if(std::abs(area)<1e-6)throw std::invalid_argument("degenerate landform footprint");
    if(area<0)std::reverse(form.rim.begin(),form.rim.end());
    if(form.spec.parent!=UINT32_MAX)
    {
      const auto &parent=forms_[form.spec.parent];
      const double apron=form.spec.faceWidthMetres*1.22+form.spec.rampWidthMetres+form.spec.talusWidthMetres;
      for(const auto p:form.rim)
      {
        Point2 foot,tangent;
        if(signedDistance(p,parent.rim,foot,tangent)>-apron)
          throw std::invalid_argument("nested cliff apron must fit inside its parent cap");
      }
    }
    Point2 lo=form.rim.front(),hi=lo;
    for(const auto p:form.rim){lo.x=std::min(lo.x,p.x);lo.y=std::min(lo.y,p.y);hi.x=std::max(hi.x,p.x);hi.y=std::max(hi.y,p.y);}
    const double apron=form.spec.faceWidthMetres*1.22+form.spec.rampWidthMetres+form.spec.talusWidthMetres;
    // Every rim segment lies within the hull of its endpoints, so the farthest
    // vertex bounds the whole boundary. `faceWidth` peaks at 1.22 face plus the
    // full ramp, which is the same apron the bucketing already uses.
    double outer=0;
    for(const auto p:form.rim)outer=std::max(outer,distance(form.center,p));
    form.reach=outer+apron;
    form.tree.build(form.rim);
    const size_t first=bucket({lo.x-apron,lo.y-apron}),last=bucket({hi.x+apron,hi.y+apron});
    for(size_t y=first/side_;y<=last/side_;++y)for(size_t x=first%side_;x<=last%side_;++x)
      buckets_[y*side_+x].push_back(uint32_t(forms_.size()));
    // Sampling both face breaks and the debris apron prevents a large Poisson
    // triangle spanning the entire cliff. Heights remain the eroded source's.
    const size_t firstRing=chains_.size();
    for(const double fraction:{0.0,.33,.67,1.0,2.0})
    {
      Chain chain;chain.closed=true;chain.kind=fraction==0?ChainKind::Brink:ChainKind::Toe;
      for(size_t i=0;i<form.rim.size();++i)
      {
        const auto a=form.rim[(i+form.rim.size()-1)%form.rim.size()],b=form.rim[(i+1)%form.rim.size()],p=form.rim[i];
        const double len=std::max(1e-9,distance(a,b));
        double offset=0;
        for(int iteration=0;iteration<12;++iteration)
        {
          const Point2 q{p.x+(b.y-a.y)*offset/len,p.y-(b.x-a.x)*offset/len};
          const double width=faceWidth(q,form.center,form.spec);
          offset=fraction<=1?fraction*width:width+form.spec.talusWidthMetres;
        }
        chain.points.push_back({p.x+(b.y-a.y)*offset/len,p.y-(b.x-a.x)*offset/len});
      }
      chains_.push_back(std::move(chain));
    }
    // Ribs share the exact ring vertices, orienting triangles down the face
    // rather than leaving five disconnected concentric constraints.
    for(size_t i=0;i<form.rim.size();i+=4)
    {
      Chain rib;rib.kind=ChainKind::Slope;
      for(size_t ring=firstRing;ring<firstRing+5;++ring)rib.points.push_back(chains_[ring].points[i]);
      chains_.push_back(std::move(rib));
    }
    forms_.push_back(std::move(form));
  }
}
LandformSample SplineLandforms::sample(Point2 p,double ground) const
{
  LandformSample result;result.height=ground;
  if(buckets_.empty())return result;
  for(const auto id:buckets_[bucket(p)])
  {
    const auto &f=forms_[id];const auto &s=f.spec;
    // The boundary distance is never less than the distance to the centre less
    // the farthest rim vertex, and `w+t` never exceeds the apron, so a sample
    // beyond `reach` is one the test below would reject anyway. Checking it
    // first turns a walk over a rim of up to a few thousand Catmull-Rom
    // samples into two multiplies.
    const double toCentreX=p.x-f.center.x,toCentreY=p.y-f.center.y;
    if(toCentreX*toCentreX+toCentreY*toCentreY>f.reach*f.reach)continue;
    // A nested tier blends into the resolved parent cap, not the original
    // continental field. This leaves a real bench between successive cliffs.
    const double substrate=s.parent==UINT32_MAX?ground:result.height;
    const double d=f.tree.signedDistance(p,f.rim),w=faceWidth(p,f.center,s),t=s.talusWidthMetres;
    if(d>=w+t)continue;
    // Low slope cap, steep weathered face, shallow debris apron. A small
    // monotone oscillation in the face profile suggests resistant bedding;
    // it never produces inverted slopes or discontinuous height steps.
    const double capDetail=s.capReliefMetres*std::sin(p.x/850+s.seed%97)*std::sin(p.y/1100+s.seed%71);
    const double rise=s.cutCap?s.topMetres+capDetail-substrate:std::max(0.0,s.topMetres+capDetail-substrate);
    double profile=1;
    SurfaceRole role=SurfaceRole::Ground;
    if(d>0 && d<w)
    {
      double q=d/w;
      // Gullies: the face is advanced or held back along its strike, so it
      // comes out as spurs and re-entrants instead of a smooth curtain. Zero at
      // the brink and at the toe, so neither line is disturbed.
      if(s.gullyDepth>0&&s.gullySpacingMetres>0)
      {
        const double along=(p.x*.8660254+p.y*.5)/s.gullySpacingMetres;
        const double cross=(p.y*.8660254-p.x*.5)/(s.gullySpacingMetres*2.7);
        const double cut=std::sin(along*6.28318530718+s.seed%13)*
            (.6+.4*std::sin(cross*6.28318530718+s.seed%29));
        q=std::clamp(q+s.gullyDepth*cut*std::sin(q*3.14159265359),.0,1.0);
      }
      const double beds=std::clamp((s.topMetres-substrate)/s.stratumMetres,1.0,12.0);
      const double u=q+.65/(beds*6.28318530718)*std::sin(q*beds*6.28318530718)*std::sin(q*3.14159265359);
      // Two segments, not one ramp: a resistant free face takes its share of
      // the drop at its own angle, the debris slope takes the rest at the angle
      // of repose, and the break between them is where the scree begins.
      //
      // The face width must be derived from the same two numbers - see
      // TerrainMapLandforms - or the free face is compressed into whatever
      // width it is given. Holding the profile while narrowing the width to the
      // old rise/2.1 put 32% of the drop into 6.6% of the run, an 84 degree
      // wall, and the triangulator could not build the tile at all.
      const double repose=std::tan(std::clamp(s.reposeAngleDegrees,5.0,60.0)*3.14159265359/180.0);
      const double share=std::clamp(s.freeFaceFraction,0.0,.9);
      const double steep=repose*2.4;
      const double faceRun=share/steep,debrisRun=(1-share)/repose;
      const double brink=faceRun/std::max(1e-6,faceRun+debrisRun);
      const double drop=u<brink?share*(u/std::max(1e-6,brink))
          :share+(1-share)*((u-brink)/std::max(1e-6,1-brink));
      profile=1-.8*drop;role=SurfaceRole::Face;
    }
    else if(d>=w){profile=.2*(1-smooth((d-w)/t));role=SurfaceRole::Talus;}
    const double roundCap=s.capRoundness>0?1-s.capRoundness*std::exp(std::min(0.0,d)/(s.faceWidthMetres*2)):1;
    const double target=substrate+rise*profile*roundCap;
    // A gentle access slope carries cover; sparse talus doesn't paint a solid
    // stone halo around every plateau.
    if(role==SurfaceRole::Face && std::abs(rise)*.8/w<.65)role=SurfaceRole::Ground;
    if(role==SurfaceRole::Talus && (profile<.07 || std::abs(rise)*.3/t<.2))role=SurfaceRole::Ground;
    if(target<=result.height && !(s.parent==UINT32_MAX && s.cutCap && profile>result.influence))continue;
    result.height=target;result.influence=s.parent==UINT32_MAX?profile:std::max(profile,result.influence);
    result.upliftMetresPerYear=s.upliftMetresPerYear;result.role=role;result.lithology=s.lithology;
    // Resistant caprock over softer talus. This participates in the existing
    // stream-power solver, so drainage cuts the primitive instead of being
    // covered by a feature pasted onto the finished rivers.
    result.hardness=std::max(result.hardness,1+profile*(role==SurfaceRole::Talus?.5:s.capResistance-1));
  }
  return result;
}
std::vector<RockFormation> nestRockFormations(std::vector<RockFormation> roots,uint32_t levels,double spacing)
{
  const size_t rootCount=roots.size();
  for(size_t root=0;root<rootCount;++root)
  {
    uint32_t parent=uint32_t(root);
    for(uint32_t level=1;level<std::min(levels,4u);++level)
    {
      const auto previous=roots[parent];
      Point2 center{};for(const auto p:previous.controls)
      {center.x+=p.x/previous.controls.size();center.y+=p.y/previous.controls.size();}
      double radius=1e30;for(const auto p:previous.controls)radius=std::min(radius,distance(center,p));
      if(radius<spacing*5)break;
      RockFormation child=previous;child.parent=parent;child.seed+=level*7919;
      child.topMetres=previous.topMetres+std::clamp(radius*.09,90.0,220.0);
      child.faceWidthMetres=std::max(35.0,std::min(110.0,radius*.07));
      child.talusWidthMetres=radius*.06;child.rampWidthMetres=radius*.05;
      child.rampAzimuth+=1.2;child.capRoundness=0;
      for(auto &p:child.controls){p.x=center.x+(p.x-center.x)*.46;p.y=center.y+(p.y-center.y)*.46;}
      // Validate the full sampled parent/child footprint, including concavities.
      // Reject a tier that cannot leave a bench rather than intersecting cliffs.
      auto candidate=roots;candidate.push_back(child);
      try { SplineLandforms check;check.build(candidate,1e9,spacing); }
      catch(const std::invalid_argument&){break;}
      parent=uint32_t(roots.size());roots.push_back(std::move(child));
    }
  }
  return roots;
}
} // namespace procedural::geometry
