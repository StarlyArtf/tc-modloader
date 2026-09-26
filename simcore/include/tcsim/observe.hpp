#pragma once
/* L5 of the sandbox simulator (docs/PLAN-sandbox-simulator.md section 4.7):
   per-event observation.  Two shapes of the same data:

     - a canonical text event list, which is what the iverilog cross-check
       compares (tools/simcore-iverilog.ps1) and what a failure report shows;
     - standard VCD, which GTKWave and the game's own scope panel already read.

   Both take the engine's trace directly, so what the panel shows and what the
   external tool sees cannot drift apart. */

#include "engine.hpp"
#include "netlist.hpp"
#include "value.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tcsim {

/* `<tick> <net> <value>` per net change, oldest first, with the t = 0 record
   of every net first.  This is the file iverilog's VCD is compared against. */
inline std::string canonicalEvents(const NetList& list, const std::vector<TraceRecord>& trace) {
    std::string text;
    for (const TraceRecord& record : trace) {
        if (record.net >= list.nets().size()) continue;
        text += std::to_string(record.when);
        text += ' ';
        text += list.net(record.net).name;
        text += ' ';
        text += record.value.toString();
        text += '\n';
    }
    return text;
}

/* A VCD writer that skips no-op changes, so a trace with several records for
   the same net at the same tick still produces a clean waveform. */
class VcdWriter {
public:
    explicit VcdWriter(std::string scope = "tcsim", std::string timescale = "1ps")
        : scope_(std::move(scope)), timescale_(std::move(timescale)) {}

    void addNet(NetId id, const std::string& name, uint16_t bits) {
        Signal signal;
        signal.id = id;
        signal.name = name;
        signal.bits = bits ? bits : 1;
        signal.code = identifier(next_code_++);
        index_[id] = signals_.size();
        signals_.push_back(std::move(signal));
    }

    void setTime(Tick when) {
        if (time_written_ && when < time_) return;
        time_ = when;
        time_written_ = true;
    }

    void change(NetId id, const BitVector& value) {
        const auto found = index_.find(id);
        if (found == index_.end()) return;
        Signal& signal = signals_[found->second];
        if (signal.has_last && signal.last == value) return;
        signal.last = value;
        signal.has_last = true;
        body_ += '#';
        body_ += std::to_string(time_);
        body_ += '\n';
        body_ += format(signal, value);
        body_ += '\n';
    }

    size_t signalCount() const { return signals_.size(); }

    std::string text() const {
        std::string text;
        text += "$timescale " + timescale_ + " $end\n";
        text += "$scope module " + scope_ + " $end\n";
        for (const Signal& signal : signals_) {
            text += "$var wire ";
            text += std::to_string(signal.bits);
            text += ' ';
            text += signal.code;
            text += ' ';
            text += signal.name;
            text += " $end\n";
        }
        text += "$upscope $end\n";
        text += "$enddefinitions $end\n";
        text += body_;
        return text;
    }

private:
    struct Signal {
        NetId id = kNoNet;
        std::string name;
        uint16_t bits = 1;
        std::string code;
        BitVector last;
        bool has_last = false;
    };

    static std::string identifier(uint64_t index) {
        std::string code;
        for (;;) {
            code.insert(code.begin(), static_cast<char>(33 + static_cast<int>(index % 94)));
            index /= 94;
            if (index == 0) break;
        }
        return code;
    }
    static std::string format(const Signal& signal, const BitVector& value) {
        if (signal.bits == 1) {
            std::string text(1, logicChar(value.size() ? value[0] : Logic::kX));
            text += signal.code;
            return text;
        }
        std::string text = "b" + value.toString() + " " + signal.code;
        return text;
    }

    std::string scope_;
    std::string timescale_;
    std::vector<Signal> signals_;
    std::map<NetId, size_t> index_;
    std::string body_;
    Tick time_ = 0;
    bool time_written_ = false;
    size_t next_code_ = 0;
};

/* The whole netlist plus one trace as a VCD document. */
inline std::string writeVcd(const NetList& list, const std::vector<TraceRecord>& trace,
                            const std::string& scope = "tcsim", const std::string& timescale = "1ps") {
    VcdWriter writer(scope, timescale);
    for (size_t index = 0; index < list.nets().size(); ++index) {
        writer.addNet(static_cast<NetId>(index), list.nets()[index].name, list.nets()[index].bits);
    }
    for (const TraceRecord& record : trace) {
        writer.setTime(record.when);
        writer.change(record.net, record.value);
    }
    return writer.text();
}

/* A one-line-per-violation report, the shape S4 turns into a panel. */
inline std::string violationReport(const NetList& list, const std::vector<Violation>& violations) {
    std::string text;
    for (const Violation& violation : violations) {
        text += std::to_string(violation.when);
        text += ' ';
        text += violationName(violation.kind);
        if (violation.net != kNoNet) text += " net=" + list.net(violation.net).name;
        if (violation.device != kNoDevice) text += " device=" + list.device(violation.device).name;
        if (!violation.detail.empty()) text += " (" + violation.detail + ")";
        text += '\n';
    }
    return text;
}

}  // namespace tcsim
