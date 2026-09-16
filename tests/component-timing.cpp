#include "../src/component_timing_graph.hpp"
#include <cassert>
#include <iostream>
using tc::timing::Node;
using tc::timing::solve;
int main() {
    // Real expanded shape: source -> two input bridges -> internal AND -> sink.
    std::vector<Node> n={{-1,0,{}, {1,2}}, {0,0,{1},{3}},
                         {0,0,{2},{4}}, {0,1,{3,4},{5}}, {-1,0,{5},{}}};
    auto r=solve(n,{5},6); assert(r.supported && r.arrival.back()==5);
    r=solve(n,{0},6); assert(r.supported && r.arrival.back()==0);
    // Serial components: internal gates must not add to declared group delays.
    n.push_back({1,0,{5},{6}}); n.push_back({1,1,{6},{7}});
    n.push_back({-1,0,{7},{}});
    r=solve(n,{5,5},8); assert(r.supported && r.arrival.back()==10);
    // Parallel branches take max, including a longer native alternative.
    n={{-1,0,{}, {1}}, {0,1,{1},{2}}, {1,1,{1},{3}}, {-1,0,{2,3},{}}};
    r=solve(n,{5,5},4); assert(r.supported && r.arrival.back()==5);
    n[2].group=-1;n[2].delay=8;
    r=solve(n,{5},4);assert(r.supported && r.arrival.back()==8);
    // Nested descendants are assigned to the outer declared group: count once.
    n={{-1,0,{}, {1}}, {0,3,{1},{2}}, {0,5,{2},{3}}, {-1,1,{3},{4}}};
    r=solve(n,{7},5);assert(r.supported && r.arrival.back()==8);
    // Disconnected branches do not increase another branch's arrival.
    n.push_back({1,1,{}, {5}});
    r=solve(n,{7,99},6);assert(r.supported && r.arrival[3]==8);
    // Native-only graph retains ordinary critical-path addition.
    n={{-1,2,{}, {1}}, {-1,3,{1},{2}}, {-1,0,{2},{}}};
    r=solve(n,{},3);assert(r.supported && r.arrival.back()==5);
    // Cycles, multi-driver nets, invalid indices, negative delays and overflow
    // must fall back rather than invent a score.
    n[0].inputs={2};assert(!solve(n,{},3).supported);
    n[0].inputs={};n[1].outputs={1};assert(!solve(n,{},3).supported);
    n[1].outputs={9};assert(!solve(n,{},3).supported);
    n[1].outputs={2};n[1].delay=-1;assert(!solve(n,{},3).supported);
    n[1].delay=INT64_MAX;assert(!solve(n,{},3).supported);
    n={{0,1,{2},{1}},{0,1,{1},{2}}};
    assert(!solve(n,{5},3).supported); // A group must not hide internal feedback.
    std::cout<<"PASS component timing: serial, parallel, nested, native, zero, fallback\n";
}
