#include "procedural/geometry/TerrainStreamPower.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
namespace procedural::geometry
{
namespace
{
constexpr int dx[8]={-1,0,1,-1,1,-1,0,1};
constexpr int dy[8]={-1,-1,-1,0,0,1,1,1};
constexpr double invDistance[8]={.7071067811865475,1,.7071067811865475,1,1,.7071067811865475,1,.7071067811865475};
using Weights=std::array<float,8>;

// Every pass here is a per-cell stencil over the same grid: `weightsFor` reads
// the eight neighbours of each cell, `accumulate` is a Jacobi gather that never
// writes its own input, and the Euler update reads four neighbours. None of the
// three carries a cross-cell dependency, so each splits into row bands.
//
// The solver runs those three passes 1800 times, so the pool has to be
// persistent: spawning a thread per pass would cost more than the pass. Bands
// are fixed and indexed, which is what keeps the reductions deterministic -
// each band accumulates its own partial and the partials are combined in band
// order, so the result does not depend on which thread finished first.
class BandPool
{
public:
  explicit BandPool(const uint32_t bands):mBands(std::max(1u,bands))
  {
    if(mBands<=1u)return;
    mWorkers.reserve(mBands-1u);
    for(uint32_t band=1u;band<mBands;++band)mWorkers.emplace_back([this,band]{loop(band);});
  }
  ~BandPool()
  {
    if(!mWorkers.empty())
    {
      {std::lock_guard<std::mutex> lock(mMutex);mStop=true;++mGeneration;}
      mWake.notify_all();
      for(std::thread &worker:mWorkers)worker.join();
    }
  }
  BandPool(const BandPool&)=delete;
  BandPool &operator=(const BandPool&)=delete;
  uint32_t bands() const {return mBands;}
  // Runs `body(band)` for every band. The calling thread takes band 0, so a
  // one-band pool is exactly the serial path with no synchronisation at all.
  void run(const std::function<void(uint32_t)> &body)
  {
    if(mBands<=1u){body(0u);return;}
    {std::lock_guard<std::mutex> lock(mMutex);mBody=&body;mRemaining=mBands-1u;++mGeneration;}
    mWake.notify_all();
    body(0u);
    std::unique_lock<std::mutex> lock(mMutex);
    mDone.wait(lock,[this]{return mRemaining==0u;});
    mBody=nullptr;
  }
private:
  void loop(const uint32_t band)
  {
    uint64_t seen=0u;
    for(;;)
    {
      const std::function<void(uint32_t)> *body=nullptr;
      {
        std::unique_lock<std::mutex> lock(mMutex);
        mWake.wait(lock,[&]{return mGeneration!=seen;});
        seen=mGeneration;
        if(mStop)return;
        body=mBody;
      }
      (*body)(band);
      {std::lock_guard<std::mutex> lock(mMutex);if(--mRemaining==0u)mDone.notify_one();}
    }
  }
  uint32_t mBands;
  std::vector<std::thread> mWorkers;
  std::mutex mMutex;
  std::condition_variable mWake,mDone;
  const std::function<void(uint32_t)> *mBody=nullptr;
  uint64_t mGeneration=0u;
  uint32_t mRemaining=0u;
  bool mStop=false;
};
// Row range owned by a band. Bands never overlap and always cover the grid.
inline uint32_t bandBegin(uint32_t side,uint32_t band,uint32_t bands){return uint32_t(uint64_t(side)*band/bands);}
inline uint32_t bandEnd(uint32_t side,uint32_t band,uint32_t bands){return uint32_t(uint64_t(side)*(band+1u)/bands);}
uint32_t weightsFor(uint32_t side,double cell,const std::vector<double>& h,
    const std::vector<uint8_t>& outlet,std::vector<Weights>& weights,std::vector<double>& slope,
    BandPool &pool,std::vector<uint32_t>& bandPits)
{
  const uint32_t bands=pool.bands();
  bandPits.assign(bands,0u);
  pool.run([&](const uint32_t band)
  {
  uint32_t pits=0;
  for(uint32_t y=bandBegin(side,band,bands);y<bandEnd(side,band,bands);++y)for(uint32_t x=0;x<side;++x)
  {
    const size_t id=size_t(y)*side+x;
    weights[id]={}; slope[id]=0;
    if(outlet[id])continue;
    double sum=0;
    for(int k=0;k<8;++k)
    {
      const int xx=int(x)+dx[k],yy=int(y)+dy[k];
      if(xx<0||yy<0||xx>=int(side)||yy>=int(side))continue;
      const double s=std::max(0.0,(h[id]-h[size_t(yy)*side+xx])*invDistance[k]/cell);
      slope[id]=std::max(slope[id],s);
      const double s2=s*s;
      sum+=weights[id][k]=float(s2*s2); // Lp routing, p=4 (eq. 4)
    }
    if(sum>0)for(auto &w:weights[id])w=float(w/sum);
    else ++pits;
  }
  bandPits[band]=pits;
  });
  // Integer counts, so the fold is exact whatever the band count.
  uint32_t pits=0;
  for(const uint32_t band:bandPits)pits+=band;
  return pits;
}
// Angle-of-repose relaxation (McDonald and Cordonnier 2026, eq. 5).
//
// `outflow` is what each cell must shed to stand at the repose angle, split
// across the neighbours below it in proportion to how far each exceeds it.
// `apply` then gathers: a cell receives each upslope neighbour's share and
// loses its own. Two gathers rather than one scatter keeps it free of atomics
// and per-cell independent, so it bands like everything else here.
void landslideOutflow(uint32_t side,double cell,const std::vector<double>& h,
    const std::vector<uint8_t>& outlet,double tanRepose,std::vector<double>& excess,BandPool &pool)
{
  const uint32_t bands=pool.bands();
  pool.run([&](const uint32_t band)
  {
  for(uint32_t y=bandBegin(side,band,bands);y<bandEnd(side,band,bands);++y)for(uint32_t x=0;x<side;++x)
  {
    const size_t id=size_t(y)*side+x;
    excess[id]=0;
    if(outlet[id])continue;
    for(int k=0;k<8;++k)
    {
      const int xx=int(x)+dx[k],yy=int(y)+dy[k];
      if(xx<0||yy<0||xx>=int(side)||yy>=int(side))continue;
      const double span=cell/invDistance[k];
      const double over=(h[id]-h[size_t(yy)*side+xx])-tanRepose*span;
      if(over>0)excess[id]+=over;
    }
  }
  });
}
void landslideApply(uint32_t side,double cell,const std::vector<double>& h,
    const std::vector<uint8_t>& outlet,double tanRepose,double rate,
    const std::vector<double>& excess,std::vector<double>& delta,BandPool &pool)
{
  const uint32_t bands=pool.bands();
  pool.run([&](const uint32_t band)
  {
  for(uint32_t y=bandBegin(side,band,bands);y<bandEnd(side,band,bands);++y)for(uint32_t x=0;x<side;++x)
  {
    const size_t id=size_t(y)*side+x;
    delta[id]=0;
    if(outlet[id])continue;
    double received=0;
    for(int k=0;k<8;++k)
    {
      const int xx=int(x)+dx[k],yy=int(y)+dy[k];
      if(xx<0||yy<0||xx>=int(side)||yy>=int(side))continue;
      const size_t nid=size_t(yy)*side+xx;
      if(outlet[nid]||excess[nid]<=0)continue;
      const double span=cell/invDistance[k];
      // The share this cell takes of that neighbour's shed material is its own
      // portion of the neighbour's total excess.
      const double over=(h[nid]-h[id])-tanRepose*span;
      if(over>0)received+=over*(over/excess[nid]);
    }
    // Half the relaxation each side of the exchange: the shedding cell drops by
    // its excess, the receiving cells rise by their share of it.
    delta[id]=rate*.5*(received-excess[id]);
  }
  });
}
void accumulate(uint32_t side,const std::vector<Weights>& weights,
    const std::vector<double>& rain,const std::vector<double>& previous,std::vector<double>& next,
    BandPool &pool)
{
  next.resize(previous.size());
  const uint32_t bands=pool.bands();
  pool.run([&](const uint32_t band)
  {
  for(uint32_t y=bandBegin(side,band,bands);y<bandEnd(side,band,bands);++y)for(uint32_t x=0;x<side;++x)
  {
    const size_t id=size_t(y)*side+x;
    double a=rain[id];
    for(int k=0;k<8;++k)
    {
      const int xx=int(x)+dx[k],yy=int(y)+dy[k];
      if(xx<0||yy<0||xx>=int(side)||yy>=int(side))continue;
      const size_t j=size_t(yy)*side+xx;
      a+=previous[j]*weights[j][7-k];
    }
    next[id]=a; // Jacobi gather, eq. 5; never update the input in-place.
  }
  });
}
}
void streamPowerDrainageStep(uint32_t side,double cell,const std::vector<double>& height,
    const std::vector<uint8_t>& outlet,const std::vector<double>& rain,
    const std::vector<double>& previous,std::vector<double>& next)
{
  const size_t count=size_t(side)*side;
  if(side<2||!(cell>0)||height.size()!=count||outlet.size()!=count||rain.size()!=count||previous.size()!=count)
    throw std::invalid_argument("invalid stream-power drainage field");
  std::vector<Weights> weights(count); std::vector<double> slopes(count);
  BandPool pool(1u); std::vector<uint32_t> bandPits;
  weightsFor(side,cell,height,outlet,weights,slopes,pool,bandPits);
  accumulate(side,weights,rain,previous,next,pool);
}
StreamPowerReport erodeStreamPower(uint32_t side,double cell,const std::vector<double>& uplift,
    const std::vector<double>& rain,const std::vector<double>& hardness,
    const std::vector<uint8_t>& outlet,std::vector<double>& height,const StreamPowerSettings& settings)
{
  StreamPowerReport report;
  if(!settings.enabled)return report;
  if(!(settings.erodibility>=0) || !std::isfinite(settings.erodibility) ||
      !(settings.diffusion>=0) || !std::isfinite(settings.diffusion) ||
      !(settings.maximumTimeStep>0) || !std::isfinite(settings.maximumTimeStep) ||
      !(settings.durationYears>=0) || !std::isfinite(settings.durationYears))
    throw std::invalid_argument("invalid stream-power time or erosion coefficients");
  const size_t count=size_t(side)*side;
  if(side<3||!(cell>0)||height.size()!=count||uplift.size()!=count||rain.size()!=count||hardness.size()!=count||outlet.size()!=count)
    throw std::invalid_argument("invalid stream-power terrain field");
  for(size_t i=0;i<count;++i)
    if(!std::isfinite(height[i])||!std::isfinite(uplift[i])||!(rain[i]>=0)||!(hardness[i]>0))
      throw std::invalid_argument("non-finite or invalid stream-power input");
  const auto original=height;
  std::vector<double> next(count),area=rain,nextArea(count),slope(count);
  std::vector<Weights> weights(count);
  // One band per hardware thread. The grid is 1024 rows by default, so even at
  // 64 bands every band still owns whole rows and the decomposition is exact.
  const uint32_t available=std::max(1u,std::thread::hardware_concurrency());
  const uint32_t bands=std::min(side,settings.workerCount==0u?available:std::clamp(settings.workerCount,1u,64u));
  BandPool pool(bands);
  std::vector<uint32_t> bandPits;
  std::vector<double> bandSpeed(pool.bands()),bandEroded(pool.bands()),bandUplift(pool.bands());
  const double tanRepose=settings.reposeAngleDegrees>0&&settings.reposeAngleDegrees<89
      ?std::tan(settings.reposeAngleDegrees*3.14159265358979323846/180.0):0.0;
  std::vector<double> landslideExcess(tanRepose>0?count:0),landslideDelta(tanRepose>0?count:0);
  double dt=settings.maximumTimeStep;
  uint32_t previousPits=UINT32_MAX;
  for(uint32_t iteration=0;iteration<settings.iterations && report.years<settings.durationYears;++iteration)
  {
    const uint32_t pits=weightsFor(side,cell,height,outlet,weights,slope,pool,bandPits);
    if(!iteration)report.initialPits=pits;
    // Drainage iterations are interleaved with erosion, as in the paper.
    accumulate(side,weights,rain,area,nextArea,pool); area.swap(nextArea);
    // A max fold is associative and exact, so banding it cannot move `dt` -
    // and `dt` is what the whole simulation is conditioned on.
    pool.run([&](const uint32_t band)
    {
      double speed=0;
      const size_t from=size_t(bandBegin(side,band,pool.bands()))*side;
      const size_t to=size_t(bandEnd(side,band,pool.bands()))*side;
      for(size_t i=from;i<to;++i)if(!outlet[i])
        speed=std::max(speed,settings.erodibility*std::sqrt(std::max(0.0,area[i]))*cell/hardness[i]);
      bandSpeed[band]=speed;
    });
    double maxSpeed=0;
    for(const double speed:bandSpeed)maxSpeed=std::max(maxSpeed,speed);
    const double cfl=.35/(maxSpeed/cell+4*settings.diffusion/(cell*cell)+1e-12);
    if(iteration%16==0)
    {
      if(previousPits!=UINT32_MAX)
      {
        if(pits<previousPits)dt*=1.01;
        else if(pits>previousPits+std::max(2u,previousPits/20))dt*=.75;
      }
      previousPits=pits;
    }
    dt=std::min({dt,cfl,settings.maximumTimeStep,settings.durationYears-report.years});
    // Forward Euler for uplift - K s sqrt(a) + D laplacian (eq. 2).
    pool.run([&](const uint32_t band)
    {
    double eroded=0,uplifted=0;
    for(uint32_t y=bandBegin(side,band,pool.bands());y<bandEnd(side,band,pool.bands());++y)for(uint32_t x=0;x<side;++x)
    {
      const size_t id=size_t(y)*side+x;
      next[id]=height[id];
      if(outlet[id])continue;
      double lap=0;
      for(int k:{1,3,4,6})
      {
        const int xx=int(x)+dx[k],yy=int(y)+dy[k];
        if(xx>=0&&yy>=0&&xx<int(side)&&yy<int(side))lap+=height[size_t(yy)*side+xx]-height[id];
      }
      const double incision=settings.erodibility*std::sqrt(std::max(0.0,area[id]))*cell*slope[id]/hardness[id];
      // Hardness resists creep as well as incision. Without this a resistant
      // band slowed the rivers cutting into a scarp and did nothing at all
      // about the hillslope diffusion smoothing it away, and diffusion is what
      // actually destroys a cliff: at D=1 m^2/yr over 82,000 years the
      // diffusion length is 405 m, wider than any escarpment face, so every
      // cliff was flattened whatever its rock. Scaled down only, never up, so
      // the CFL bound computed from the nominal diffusion stays valid.
      const double creep=settings.diffusion/std::max(1.0,hardness[id]);
      next[id]+=dt*(uplift[id]-incision+creep*lap/(cell*cell));
      eroded+=dt*incision*cell*cell;
      uplifted+=dt*uplift[id]*cell*cell;
    }
    bandEroded[band]=eroded; bandUplift[band]=uplifted;
    });
    // Volume totals are diagnostics and never feed the field, but they are
    // still folded in band order so a run reproduces itself exactly.
    for(uint32_t band=0;band<pool.bands();++band)
    { report.erodedCubicMetres+=bandEroded[band]; report.upliftCubicMetres+=bandUplift[band]; }
    height.swap(next);
    if(tanRepose>0)
    {
      landslideOutflow(side,cell,height,outlet,tanRepose,landslideExcess,pool);
      landslideApply(side,cell,height,outlet,tanRepose,settings.landslideRate,landslideExcess,landslideDelta,pool);
      pool.run([&](const uint32_t band)
      {
        const size_t from=size_t(bandBegin(side,band,pool.bands()))*side;
        const size_t to=size_t(bandEnd(side,band,pool.bands()))*side;
        for(size_t i=from;i<to;++i)height[i]+=landslideDelta[i];
      });
    }
    report.years+=dt; report.iterations=iteration+1;
    if(settings.progress && (iteration%100==0 || iteration+1==settings.iterations))settings.progress(iteration+1,report.years);
  }
  report.finalPits=weightsFor(side,cell,height,outlet,weights,slope,pool,bandPits);
  for(size_t i=0;i<count;++i)
  { report.maximumCutMetres=std::max(report.maximumCutMetres,original[i]-height[i]);
    report.maximumRiseMetres=std::max(report.maximumRiseMetres,height[i]-original[i]); }
  return report;
}
}
