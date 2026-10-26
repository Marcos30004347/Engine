#pragma once
#include "WorldCitySkeleton.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace procedural::world::rewrite {
// Sample before roads and stairs are graded. Every consumer then sees the same
// continuous planar facets; access cuts must never be resampled afterward.
inline std::function<double(double,double)> cityRampTerrain(
    const CitySkeleton &city,const std::function<double(double,double)> &source,double pitch)
{
  if(!source || !(pitch>0) || !std::isfinite(pitch))return source;
  double x0=INFINITY,z0=INFINITY,x1=-INFINITY,z1=-INFINITY;
  for(const auto &r:city.regions)for(auto p:r.boundary) {
    x0=std::min(x0,p.x);x1=std::max(x1,p.x);z0=std::min(z0,p.z);z1=std::max(z1,p.z);
  }
  if(!std::isfinite(x0+x1+z0+z1))return source;
  x0=std::floor(x0/pitch)*pitch-2*pitch;z0=std::floor(z0/pitch)*pitch-2*pitch;
  const size_t nx=size_t(std::ceil((x1-x0)/pitch))+3,nz=size_t(std::ceil((z1-z0)/pitch))+3;
  // Bound allocation for malformed or world-scale callers; city bounds only.
  if(nx>4096 || nz>4096 || nx*nz>4000000)return source;
  auto heights=std::make_shared<std::vector<double>>(nx*nz);
  for(size_t z=0;z<nz;++z)for(size_t x=0;x<nx;++x)(*heights)[z*nx+x]=source(x0+x*pitch,z0+z*pitch);
  return [=](double x,double z) {
    const double gx=(x-x0)/pitch,gz=(z-z0)/pitch;
    if(gx<0 || gz<0 || gx>nx-1 || gz>nz-1)return source(x,z);
    const size_t ix=std::min(size_t(gx),nx-2),iz=std::min(size_t(gz),nz-2);
    const double u=gx-ix,v=gz-iz;
    const double a=(*heights)[iz*nx+ix],b=(*heights)[iz*nx+ix+1],c=(*heights)[(iz+1)*nx+ix],d=(*heights)[(iz+1)*nx+ix+1];
    return u+v<=1 ? a+(b-a)*u+(c-a)*v : d+(c-d)*(1-u)+(b-d)*(1-v);
  };
}
}
