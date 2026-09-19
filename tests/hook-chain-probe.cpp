/* Probe plugin for the loader-owned hook chains (sdk/tc_hook.h).

   The same file is built several times with different macros; tests/hook-chain.cpp
   stages the packages, drives fake game functions, and asserts the order, the
   argument edits, the swallowing link and the raw-hook rejection.  The host
   process exports tc_test_note(), which the probe finds with resolve_symbol, so
   the host can see exactly what the plugin did and in which order.

   Modes:
     TC_PROBE_ORDER  two links: tag A at priority A, tag B at priority B
     TC_PROBE_PEER   one link: tag A at priority A
     TC_PROBE_SKIP   one link that swallows the game's own function
     TC_PROBE_RAW    raw create_hook on a plain target (ok) and on sim.do (must fail)
*/
#include "../sdk/tc_mod_api.h"
#include "../sdk/tc_hook.h"
#include "../sdk/tc_event.h"
#include <cstdio>
#include <string>

#ifndef TC_PROBE_TAG_A
#define TC_PROBE_TAG_A "a"
#endif
#ifndef TC_PROBE_PRIORITY_A
#define TC_PROBE_PRIORITY_A 0
#endif
#ifndef TC_PROBE_TAG_B
#define TC_PROBE_TAG_B "b"
#endif
#ifndef TC_PROBE_PRIORITY_B
#define TC_PROBE_PRIORITY_B 10
#endif

static const TCHost* host;
static void (*note)(const char*);

static void say(const std::string& message) {
    if (host && host->log) host->log(host->context, message.c_str());
    /* The host process collects these lines; a null pointer just means the probe
       is running somewhere else (the real game), where the log is enough. */
    if (note) note(message.c_str());
}

/* What every mode reports first: the stable aliases the loader resolved. */
static void reportAliases() {
    const bool simDo = tc::resolveAlias(host, "sim.do") != nullptr;
    const bool cycle = tc::resolveAlias(host, "sim.cycle") != nullptr;
    const bool bogus = tc::resolveAlias(host, "no.such.alias") != nullptr;
    say(std::string("alias sim.do=") + (simDo ? "1" : "0") + " sim.cycle=" + (cycle ? "1" : "0") +
        " bogus=" + (bogus ? "1" : "0"));
}

#if defined(TC_PROBE_ORDER) || defined(TC_PROBE_PEER)
static int taggedLink(TCHookCall* call, const char* tag) {
    auto* args = tc::hook::simDoArgs(call);
    say(std::string("link ") + tag + (args ? " target=" + std::to_string(args->target) : " no-args"));
    /* Proves the chain gives the plugin the real arguments and that an edit
       reaches the game's own function. */
    if (args) args->target += 1;
    return 0;
}
#endif

#ifdef TC_PROBE_ORDER
static int linkA(TCHookCall* call) { return taggedLink(call, TC_PROBE_TAG_A); }
static int linkB(TCHookCall* call) { return taggedLink(call, TC_PROBE_TAG_B); }
#endif
#ifdef TC_PROBE_PEER
static int linkA(TCHookCall* call) { return taggedLink(call, TC_PROBE_TAG_A); }
#endif

#ifdef TC_PROBE_SKIP
/* Stops the chain and keeps the game's own function from running: this is how a
   mod replaces behaviour instead of watching it. */
static int skippingLink(TCHookCall* call) {
    say("skip swallow=1");
    call->skip_original = 1;
    return 1;
}
static int afterLink(TCHookCall* call) {
    (void)call;
    say("skip later-link-ran");
    return 0;
}
#endif

#ifdef TC_PROBE_EVENTS
/* One listener for every kind: the two lines it writes are the whole evidence
   the host needs (which event arrived, and its payload). */
static void onEvent(TCEvent* event) {
    std::string line = std::string("event ") + tc::events::kindName(event->kind);
    if (event->kind == TC_EVENT_SIM_COMMAND) line += " command=" + std::to_string(event->flags);
    if (event->kind == TC_EVENT_SAVE) line += " count=" + std::to_string(event->flags);
    if (event->kind == TC_EVENT_SCENE_CHANGE)
        line += " scene=" + std::to_string(event->flags) +
                (tc::events::sceneContext(event) ? " subject=1" : " subject=0");
    if (event->kind == TC_EVENT_LEVEL_LOAD)
        line += event->subject ? " subject=1" : " subject=0";
    say(line);
}
#endif

#ifdef TC_PROBE_CRASH
/* A link that dereferences a bad pointer.  The loader must survive it, drop this
   link, and still run the rest of the chain and the game's own function. */
static int crashLink(TCHookCall* call) {
    (void)call;
    say("crash link entered");
    volatile int* nowhere = reinterpret_cast<volatile int*>(8);
    *nowhere = 1;
    say("crash link survived (must never happen)");
    return 0;
}
static int survivorLink(TCHookCall* call) {
    (void)call;
    say("crash survivor ran");
    return 0;
}
#endif

#if defined(TC_PROBE_RAW) || defined(TC_PROBE_DUP)
/* The plain target is sim.cycle: every test host defines the same fake symbol,
   it is not a chain point, and forwarding through the trampoline proves the raw
   hook really took over the function. */
static int64_t (*plainOriginal)() = nullptr;
static int64_t plainDetour() {
    return plainOriginal ? plainOriginal() : 0;
}

/* A plain (non-chain) target: the first package wins, a second package asking
   for the same function is refused.  This is what the conflict fixture checks. */
static void rawButtonAttempt() {
    void* plain = tc::resolveAlias(host, "sim.cycle");
    void (*detour)() = nullptr;
    const int plainResult =
        plain ? host->create_hook(host->context, plain, reinterpret_cast<void*>(&plainDetour),
                                  reinterpret_cast<void**>(&detour))
              : -99;
    plainOriginal = reinterpret_cast<int64_t (*)()>(detour);
    say(std::string("raw plain-hook ok=") + (plainResult == 0 ? "1" : "0"));
}
#endif

#ifdef TC_PROBE_RAW
static void rawSimDoDetour(void*, unsigned char, long long) {}

static void rawProbe() {
    rawButtonAttempt();
    void* chainPoint = tc::hook::callPoint(host, TC_HOOK_SIM_DO);
    void (*detour2)() = nullptr;
    const int chainResult =
        chainPoint ? host->create_hook(host->context, chainPoint,
                                       reinterpret_cast<void*>(&rawSimDoDetour),
                                       reinterpret_cast<void**>(&detour2))
                   : -99;
    say(std::string("raw sim.do ok=") + (chainResult == 0 ? "1" : "0"));
}
#endif

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || h->api_version != TC_MOD_API_VERSION || h->size < TC_HOST_BASE_SIZE || !out ||
        out->size < sizeof(TCPlugin))
        return 1;
    host = h;
    note = reinterpret_cast<void (*)(const char*)>(h->resolve_symbol(h->context, "tc_test_note"));
    reportAliases();
#ifdef TC_PROBE_ORDER
    const int first = tc::hook::addSimDo(h, TC_PROBE_PRIORITY_A, &linkA, nullptr);
    const int second = tc::hook::addSimDo(h, TC_PROBE_PRIORITY_B, &linkB, nullptr);
    say(std::string("order joined ") + TC_PROBE_TAG_A + "=" + tc::hook::errorText(first) + " " +
        TC_PROBE_TAG_B + "=" + tc::hook::errorText(second));
    if (first != TC_HOOK_OK || second != TC_HOOK_OK) return 2;
#endif
#ifdef TC_PROBE_PEER
    const int only = tc::hook::addSimDo(h, TC_PROBE_PRIORITY_A, &linkA, nullptr);
    say(std::string("peer joined ") + TC_PROBE_TAG_A + "=" + tc::hook::errorText(only));
    if (only != TC_HOOK_OK) return 2;
#endif
#ifdef TC_PROBE_SKIP
    const int head = tc::hook::addSimDo(h, 0, &skippingLink, nullptr);
    const int tail = tc::hook::addSimDo(h, 100, &afterLink, nullptr);
    say(std::string("skip joined head=") + tc::hook::errorText(head) + " tail=" +
        tc::hook::errorText(tail));
    if (head != TC_HOOK_OK || tail != TC_HOOK_OK) return 2;
#endif
#ifdef TC_PROBE_RAW
    rawProbe();
#endif
#ifdef TC_PROBE_DUP
    rawButtonAttempt();
#endif
#ifdef TC_PROBE_EVENTS
    const uint32_t mask = TC_EVENT_LEVEL_LOAD | TC_EVENT_SCENE_CHANGE | TC_EVENT_SIM_COMMAND |
                          TC_EVENT_SAVE;
    const int subscribed = tc::events::subscribe(h, mask, &onEvent, nullptr);
    say(std::string("events subscribed=") + tc::events::errorText(subscribed));
    if (subscribed != TC_EVENT_OK) return 2;
#endif
#ifdef TC_PROBE_CRASH
    const int crasher = tc::hook::addSimDo(h, 0, &crashLink, nullptr);
    const int survivor = tc::hook::addSimDo(h, 100, &survivorLink, nullptr);
    say(std::string("crash joined crasher=") + tc::hook::errorText(crasher) + " survivor=" +
        tc::hook::errorText(survivor));
    if (crasher != TC_HOOK_OK || survivor != TC_HOOK_OK) return 2;
#endif
    return 0;
}
