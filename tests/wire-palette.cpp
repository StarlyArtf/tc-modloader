#include "../examples/wire-palette/palette.hpp"
#include <cassert>
#include <iostream>
int main(){RGB expected{10,20,30};Palette p;int a=p.add({10,20,30});for(int i=0;i<7;++i)p.add({i,40,50});assert(p.recent.size()==5);assert(p.colors.at(a-11)==expected);assert(p.add({10,20,30})==a);assert(p.recent.front()==a);assert(p.colors.size()==8);auto file=std::filesystem::path("build/palette-test.json");p.save(file);Palette q;q.load(file);assert(q.colors==p.colors&&q.recent==p.recent);for(int i=8;i<244;++i)q.add({i,60,70});assert(q.colors.size()==244);bool full=false;try{q.add({255,255,255});}catch(...){full=true;}assert(full);assert(q.colors.at(a-11)==expected);std::cout<<"PASS recent five, deduplication, stable IDs, save/reload, capacity preserves old colors\n";}

