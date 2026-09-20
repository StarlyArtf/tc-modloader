#pragma once
#include <cstdint>
#include <limits>
#include <queue>
#include <set>
#include <vector>

namespace tc::timing {
// A group is an opaque Mod component for timing only. The simulator still
// executes every internal node. -1 leaves a node's native timing unchanged.
struct Node {
    int group = -1;
    int64_t delay = 0;
    std::vector<size_t> inputs, outputs;
};
struct Result {
    bool supported = false;
    std::vector<int64_t> arrival;
};
inline Result solve(const std::vector<Node>& nodes,
                    const std::vector<int64_t>& groupDelays, size_t nets) {
    Result result;
    const size_t groups = groupDelays.size(), count = groups + nodes.size();
    std::vector<size_t> owner(nodes.size());
    std::vector<int64_t> delay(count, 0), arrival(count, 0);
    for (size_t i=0;i<groups;++i) {
        if (groupDelays[i]<0) return result;
        delay[i]=groupDelays[i];
    }
    std::vector<std::set<size_t>> drivers(nets), edges(count);
    std::vector<size_t> indegree(count, 0);
    for (size_t i=0;i<nodes.size();++i) {
        const auto& n=nodes[i];
        if (n.group < -1 || (n.group>=0 && size_t(n.group)>=groups) || n.delay<0) return result;
        owner[i]=n.group>=0?size_t(n.group):groups+i;
        if(n.group<0) delay[owner[i]]=n.delay;
        for(auto net:n.outputs) {
            if(net>=nets) return result;
            if(net) drivers[net].insert(i);
        }
    }
    std::vector<std::set<size_t>> nativeEdges(nodes.size());
    std::vector<size_t> nativeIndegree(nodes.size(),0);
    for(size_t i=0;i<nodes.size();++i) for(auto net:nodes[i].inputs) {
        if(net>=nets) return result;
        if(!net) continue; // The pinned game's disconnected-port sentinel.
        // Multiple drivers and feedback need the game's cluster semantics.
        if(drivers[net].size()>1) return result;
        for(auto rawSource:drivers[net]) {
            if(nativeEdges[rawSource].insert(i).second) ++nativeIndegree[i];
            auto source=owner[rawSource];
            auto dest=owner[i];
            if(source==dest) {
                if(nodes[i].group<0) return result;
                continue;
            }
            if(edges[source].insert(dest).second) ++indegree[dest];
        }
    }
    std::queue<size_t> nativeReady;
    for(size_t i=0;i<nodes.size();++i) if(!nativeIndegree[i]) nativeReady.push(i);
    size_t nativeVisited=0;
    while(!nativeReady.empty()) {
        auto i=nativeReady.front();nativeReady.pop();++nativeVisited;
        for(auto j:nativeEdges[i]) if(!--nativeIndegree[j]) nativeReady.push(j);
    }
    if(nativeVisited!=nodes.size()) return result;
    std::queue<size_t> ready;
    for(size_t i=0;i<count;++i) if(!indegree[i]) ready.push(i);
    size_t visited=0;
    while(!ready.empty()) {
        auto i=ready.front(); ready.pop(); ++visited;
        if(arrival[i]>std::numeric_limits<int64_t>::max()-delay[i]) return result;
        arrival[i]+=delay[i];
        for(auto j:edges[i]) {
            if(arrival[j]<arrival[i]) arrival[j]=arrival[i];
            if(!--indegree[j]) ready.push(j);
        }
    }
    if(visited!=count) return result;
    result.arrival.resize(nodes.size());
    for(size_t i=0;i<nodes.size();++i) result.arrival[i]=arrival[owner[i]];
    result.supported=true;
    return result;
}
}
