// Fallback telemetry: no GPU signal at all.
//
// A node with no readable GPU must still be routable — it has an engine, models
// on disk and a queue, which is enough to serve requests. What it loses is the
// utilization and thermal terms, and those are *omitted* from the score rather
// than defaulted to zero, so the scheduler never mistakes "unknown" for "idle"
// (ARCHITECTURE §10).
//
// VRAM is the exception: without it, admission cannot decide whether a model
// fits. The agent reports zero total VRAM, and the router treats a node whose
// vram_total is 0 as VRAM-unconstrained only when explicitly configured to.

#include "agent/telemetry/telemetry.h"

namespace rf {
namespace {

class NullTelemetry : public ITelemetry {
public:
    const char* name() const override { return "null"; }
    bool probe() override { return true; }  // always available, by definition

    bool sample(std::vector<GpuSample>& out) override {
        out.clear();
        GpuSample s;
        s.name = "unknown";
        // Every has_* stays false: nothing here is measured, so nothing here is
        // reported as a number.
        out.push_back(std::move(s));
        return true;
    }
};

}  // namespace

std::unique_ptr<ITelemetry> make_null_telemetry() {
    return std::unique_ptr<ITelemetry>(new NullTelemetry());
}

}  // namespace rf
