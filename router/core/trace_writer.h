// RouteFlow — the trace file (ARCHITECTURE §7, docs/TRACE-SCHEMA.md).
//
// Append-only JSONL, one record per dispatch attempt. This file is the
// project's primary asset: the cost model, the UI history and the evaluation
// harness all read it.
//
// THREADING: unlike the ledger and the cost model, this class owns its own
// mutex. Writing is I/O, and §4.2.1 forbids holding the router's shared_mutex
// across I/O — so a trace append happens *after* the state lock is released.
#pragma once

#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "common/types.h"

namespace rf {

class TraceWriter {
public:
    ~TraceWriter();

    // Opens for append, creating the file if needed. Existing records are
    // never rewritten — the format is a contract and old runs must stay
    // readable (§10).
    //
    // `max_bytes` bounds the live file. When it would be exceeded the file is
    // renamed to `<path>.1` — replacing any previous `.1` — and a fresh one
    // started, so the router's disk use is bounded at twice this and the
    // previous generation stays available to read. 0 disables rotation, which
    // is what the benchmarks want: a run's trace is a measurement, and a
    // measurement that silently loses its first half is not one.
    bool open(const std::string& path, std::string* err, uint64_t max_bytes = 0);
    bool is_open() const;
    const std::string& path() const { return path_; }

    void append(const TraceRecord& record);

    // Called for every appended record, after the write. Lets the UI keep a
    // recent-jobs view without the dispatcher knowing the UI exists. Set once
    // at startup, before any dispatch.
    void set_observer(std::function<void(const TraceRecord&)> observer);

    uint64_t records_written() const;
    uint64_t write_errors() const;
    uint64_t rotations() const;

private:
    void rotate_locked();

    mutable std::mutex mu_;
    std::ofstream out_;
    std::string path_;
    std::function<void(const TraceRecord&)> observer_;
    uint64_t written_ = 0;
    uint64_t errors_ = 0;
    uint64_t rotations_ = 0;
    uint64_t max_bytes_ = 0;
    uint64_t bytes_ = 0;
};

// Reading side, shared by the router's warm start and the bench report tools.
struct TraceReadStats {
    uint64_t lines = 0;
    uint64_t parsed = 0;
    uint64_t parse_errors = 0;  // exposed as trace_parse_errors (schema rule 4)
    uint64_t newer_version = 0;
};

// Streams a trace file, calling `on_record` for each record that parses.
// A malformed line is skipped and counted, never fatal: a truncated final line
// after a crash must not stop the router from starting.
//
// `tail_bytes` reads only the last N bytes, discarding the partial line at the
// seam. 0 reads the whole file, which is what every analysis tool wants and
// what a restart does not: the cost model is a set of EWMAs over a 20-sample
// half-life, so a month of records teaches it nothing a few thousand did not,
// while making every start slower than the last.
bool read_trace(const std::string& path,
                const std::function<void(const TraceRecord&)>& on_record,
                TraceReadStats* stats, std::string* err, uint64_t tail_bytes = 0);

}  // namespace rf
