#ifndef TC_VERILOG_HPP
#define TC_VERILOG_HPP

#include "tc_component_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tc {
namespace verilog {

/* TC-Verilog is deliberately a synthesizable language kernel, not a second
   event-driven simulator.  A compiled Program is immutable and may be shared
   by every instance of one component; all mutable data lives in the
   TCLogicIOV3 state array supplied by the host. */

struct Diagnostic {
    uint32_t line = 0;
    uint32_t column = 0;
    std::string message;
};

enum class PortDirection : uint8_t { Input, Output };

struct Port {
    std::string name;
    PortDirection direction = PortDirection::Input;
    uint32_t bits = 1;
};

struct CompileOptions {
    /* These are denial-of-service limits as well as useful editor feedback. */
    uint32_t max_source_bytes = 256u * 1024u;
    uint32_t max_tokens = 65536;
    uint32_t max_state_words = 65536;
    uint32_t max_expression_depth = 128;
};

struct ComponentMetadata {
    uint64_t custom_id = 0;
    std::string type_id;       // empty: let the host derive a Mod-namespaced id
    std::string name;          // empty: use the Verilog module name
    std::string description;
    std::string shape_svg;     // empty: use the game's generated default shape
    uint64_t gate_cost = 0;
    uint64_t delay = 0;
    float pin_lane = 0.0f;     // 0: host default (currently 2 board cells)
    uint32_t flags = 0;        // TC_COMPONENT_TYPE_*
};

class Program final {
public:
    struct Impl;  // opaque; exposed only so the separately-built parser can name it

    Program();
    ~Program();
    Program(Program&&) noexcept;
    Program& operator=(Program&&) noexcept;
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;

    /* Compilation is transactional: a failure leaves the previous program
       intact.  Every diagnostic points at the source token that was refused. */
    bool compile(const std::string& source, std::vector<Diagnostic>* diagnostics = nullptr,
                 const CompileOptions& options = {});

    bool valid() const noexcept;
    const std::string& moduleName() const noexcept;
    const std::vector<Port>& ports() const noexcept;
    uint32_t inputCount() const noexcept;
    uint32_t outputCount() const noexcept;
    uint32_t stateWords() const noexcept;
    /* Exact top-level input dependencies for each output.  Bit N names input
       N.  Edge-triggered modules additionally use a hidden all-input CYCLE
       sink; visible outputs remain independent REFRESH evaluations. */
    const std::vector<uint32_t>& outputDependencies() const noexcept;
    /* Optional two-state straight-line programs for the loader's native game
       JIT.  Empty means this source needs the normal callback interpreter. */
    const std::vector<std::string>& inlinePrefixes() const noexcept;
    const std::vector<std::string>& inlineExpressions() const noexcept;
    bool hasClockedState() const noexcept;

    /* Execute one host phase.  CYCLE detects 0->1 edges and commits all
       nonblocking assignments together.  RESET/REFRESH only evaluate outputs;
       the host owns state zeroing and REFRESH isolation. */
    void execute(TCLogicIOV3* io) const noexcept;

    /* Direct TCComponentTypeDefinitionV3 callback.  Set definition.user to a
       compiled Program.  A missing/invalid program drives every output to Z. */
    static void callback(TCLogicIOV3* io) noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

/* Owns the source program and every string/pin buffer referenced by the V3
   definition.  Keep this object alive for the whole plugin lifetime.  It is
   intentionally non-movable: after registration, the host callback's user
   pointer addresses its Program subobject. */
class Component final {
public:
    Component();
    ~Component();
    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;
    Component(Component&&) = delete;
    Component& operator=(Component&&) = delete;

    bool compile(const std::string& source, const ComponentMetadata& metadata,
                 std::vector<Diagnostic>* diagnostics = nullptr,
                 const CompileOptions& options = {});

    /* Hot reload for an already registered component: compile new source and
       put the new program in the object the host already points at, so every
       instance of this type runs the new logic on the next cycle - no restart,
       no re-registration, no change in the generated code.

       Only legal while the shape is unchanged: same ports (name, direction and
       width, in the same order) and the same state_words.  The ports and the
       state layout are what the game's compiled board and the saved instance
       state are keyed to, so a moved port has to wait for the next start. */
    bool hotSwap(const std::string& source, std::vector<Diagnostic>* diagnostics = nullptr,
                 const CompileOptions& options = {});
    /* The compile and shape check on its own, leaving the live program alone. */
    bool checkHotSwap(const std::string& source, std::vector<Diagnostic>* diagnostics = nullptr,
                      const CompileOptions& options = {}) const;

    /* Registers a brand-new type while the game is running
       (tc.component.runtime).  Answers the same TC_COMPONENT_TYPES_* codes:
       TC_COMPONENT_TYPES_ERR_UNAVAILABLE on a loader without the runtime
       service, TC_COMPONENT_TYPES_ERR_STATE while a board is up. */
    int registerLive(const TCHost* host) noexcept;

    /* Replace/remove the runtime registration while the component model is
       idle.  replaceLiveWith keeps the old registration when the host rejects
       the candidate, and marks `replacement` registered only on success. */
    int replaceLiveWith(const TCHost* host, Component* replacement) noexcept;
    int unregisterLive(const TCHost* host) noexcept;

    /* Queries tc.component.types API V2 and submits a V3 definition. */
    int registerWith(const TCHost* host) noexcept;

    bool valid() const noexcept;
    bool registered() const noexcept;
    const Program& program() const noexcept;
    const ComponentMetadata& metadata() const noexcept;
    const TCComponentTypeDefinitionV3* definition() const noexcept;

private:
    void rebuildDefinition();

    Program program_;
    ComponentMetadata metadata_;
    /* Pin labels live here rather than inside the program so that a hot swap
       (which replaces the program) cannot invalidate a name pointer the host
       already holds. */
    std::vector<std::string> portNames_;
    std::vector<TCComponentPinV2> inputs_, outputs_;
    std::vector<uint32_t> outputDependencies_;
    std::vector<const char*> inlinePrefixPointers_, inlineExpressionPointers_;
    TCComponentTypeDefinitionV3 definition_{};
    bool registered_ = false;
};

}  // namespace verilog
}  // namespace tc

#endif  // TC_VERILOG_HPP
