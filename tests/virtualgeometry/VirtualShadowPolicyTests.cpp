#include "virtualshadowmap/ShadowPolicy.hpp"
#include <iostream>
using namespace virtualgeometry;
static void check(bool value) { if (!value) throw std::runtime_error("Shadow policy assertion failed"); }
int main() {
  try {
    ShadowDistancePolicy p{10,20}; p.validate();
    check(p.weight(0)==1 && p.weight(10)==1 && p.weight(15)==.5f && p.weight(20)==0);
    check(ShadowDistancePolicy{0,0}.weight(0)==0);
    check(ShadowDistancePolicy{10,10}.weight(9)==1 && ShadowDistancePolicy{10,10}.weight(10)==0);
    check(std::abs(shadowFirstClipmapExtent(32,128,1080,1.04719755f,1)-4.37938f)<.001f);
    check(shadowClipmapLevels(4,1000,32,16)==11);
    check(shadowClipmapLevels(16,7,32,16)==1);
    check(shadowClipmapLevels(16,8,32,16)==2); // snap margin matters
    bool rejected=false; try { ShadowDistancePolicy{2,1}.validate(); } catch (const std::invalid_argument &) { rejected=true; } check(rejected);
    rejected=false; try { shadowClipmapLevels(4,1000,32,4); } catch (const std::invalid_argument &) { rejected=true; } check(rejected);
    std::cout << "PASS: receiver distance fade and clipmap coverage margins\n";
  } catch (const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
