// RouteFlow — the dispatch ledger (ARCHITECTURE §6.3, D4).
//
// NodeState arrives by poll and SSE and is always stale by up to one sample
// interval. Two concurrent requests scored against the same snapshot both see
// an idle warm node and both pick it; for a *cold* model they instead pick two
// different nodes and load the same model twice — spending VRAM to manufacture
// the exact problem RouteFlow exists to solve.
//
// This ledger is the router's own authoritative account of what it has already
// committed. It is updated synchronously at dispatch and never derived from
// telemetry.
//
// THREADING: this class contains no lock of its own. It is owned by RouterState
// and every method is called with that object's shared_mutex held — const
// methods under a shared lock, mutating methods under an exclusive one (D13).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "common/types.h"

namespace rf {

class NodeLedger : public LedgerView {
public:
    using Token = uint64_t;
    static constexpr Token kInvalid = 0;

    // Taken after scoring and before the dispatch I/O begins. `eta_ms` is the
    // predicted load time, which is what a second request for the same model
    // will see as its own T_load instead of paying a full cold load elsewhere.
    Token reserve(const std::string& node_id, const std::string& model,
                  uint64_t footprint_bytes, bool will_load, double eta_ms,
                  int64_t now_ms);

    // The load finished: release the VRAM reservation but keep the request
    // counted as in flight.
    void note_load_done(Token token);

    // First token seen. Until this point the request is not yet contending for
    // decode throughput, which is the distinction §6.2 prices.
    // Returns how many requests are decoding on that node once this one is
    // counted -- which is the number §6.2's contention term needs, observed at
    // the only moment it is true (D44).
    uint32_t note_decoding(Token token);

    void release(Token token);

    // --- LedgerView -------------------------------------------------------
    uint32_t inflight(const std::string& node_id) const override;
    uint32_t concurrent_decoders(const std::string& node_id) const override;
    uint64_t reserved_vram_bytes(const std::string& node_id) const override;
    const PendingLoad* pending(const std::string& node_id,
                               const std::string& model) const override;
    bool model_busy(const std::string& node_id, const std::string& model) const override;
    int64_t last_served_ms(const std::string& node_id,
                           const std::string& model) const override;
    uint32_t models_served_since(const std::string& node_id,
                                 const std::string& except_model,
                                 int64_t since_ms) const override;

    // Diagnostics for /admin/stats and the UI.
    Json to_json() const;

private:
    struct Entry {
        std::string node_id;
        std::string model;
        uint64_t footprint_bytes = 0;
        bool loading = false;
        bool decoding = false;
        PendingLoad load;
    };

    struct NodeAccount {
        uint32_t inflight = 0;
        uint32_t decoders = 0;
        uint64_t reserved_vram = 0;
        // model -> (pending load, how many requests are waiting behind it)
        std::map<std::string, std::pair<PendingLoad, uint32_t>> pending_loads;
        std::map<std::string, uint32_t> busy_models;
    };

    NodeAccount& account(const std::string& node_id) { return nodes_[node_id]; }
    const NodeAccount* find(const std::string& node_id) const;

    std::map<std::string, NodeAccount> nodes_;
    std::map<Token, Entry> entries_;

    // "node|model -> when we last finished serving it there". Deliberately not
    // part of NodeAccount: an account is live accounting and is erased the
    // moment a node goes idle, which is precisely when this fact becomes the
    // only evidence anyone has (D36).
    std::map<std::string, int64_t> last_served_;
    Token next_token_ = 1;
};

}  // namespace rf
