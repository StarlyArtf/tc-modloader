/* M0.5 probe: does suppressing the game's circular-dependency annotation let a
   cyclic board reach the code generator?

   The M0 A/B showed the difference plainly: with a self-loop wire the emitted
   program carries only the `com_none` placeholder, without it the NAND is
   emitted.  `generate_source` consumes the preorder result, so this probe takes
   out the one call that marks the cycle

       set_circular_dependency__modelZsimulationZpreorder_u27466

   and nothing else.  If the NAND blocks show up in the next generated-source
   dump, the "let it through" route (C1) is alive; if the board is still empty,
   the emptying happens somewhere else and C2 (cut for compile, re-close in the
   rewrite) is the way. */
#include "../sdk/tc_mod_api.h"
#include <cstdio>
#include <string>

static const TCHost* host=nullptr;
static void (*originalSetCircular)(void*)=nullptr;
static int suppressed=0;

static void noopFrame(void*,const TCFrame*) {}

static void allowCycle(void* context) {
    ++suppressed;
    if(host&&host->log&&suppressed<=10) {
        char line[128];
        std::snprintf(line,sizeof(line),"CYCLE-ALLOW: set_circular_dependency suppressed (#%d)",suppressed);
        host->log(host->context,line);
    }
    /* Deliberately do not call the original: the point is to see whether the
       compile still produces a circuit without the annotation. */
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h,TCPlugin* out) {
    if(!h||!out||h->api_version!=TC_MOD_API_VERSION) return 1;
    host=h;
    void* target=h->resolve_symbol(h->context,"set_circular_dependency__modelZsimulationZpreorder_u27466");
    if(!target) {
        if(h->log) h->log(h->context,"CYCLE-ALLOW: set_circular_dependency not found in this build");
        return 2;
    }
    if(h->create_hook(h->context,target,reinterpret_cast<void*>(allowCycle),
                      reinterpret_cast<void**>(&originalSetCircular))!=0) {
        if(h->log) h->log(h->context,"CYCLE-ALLOW: hook rejected");
        return 3;
    }
    if(h->log) h->log(h->context,"CYCLE-ALLOW: hooked set_circular_dependency (annotation off)");
    out->on_frame=&noopFrame;
    return 0;
}
