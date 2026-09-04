#include "router/core/trace_writer.h"

#include <cerrno>
#include <cstring>

#include "common/json.h"
#include "common/util.h"

namespace rf {

TraceWriter::~TraceWriter() {
    std::lock_guard<std::mutex> lock(mu_);
    if (out_.is_open()) out_.close();
}

bool TraceWriter::open(const std::string& path, std::string* err) {
    std::lock_guard<std::mutex> lock(mu_);
    if (out_.is_open()) out_.close();
    out_.open(path, std::ios::out | std::ios::app | std::ios::binary);
    if (!out_) {
        if (err) *err = "cannot open trace file " + path + ": " + std::strerror(errno);
        return false;
    }
    path_ = path;
    return true;
}

bool TraceWriter::is_open() const {
    std::lock_guard<std::mutex> lock(mu_);
    return out_.is_open();
}

void TraceWriter::set_observer(std::function<void(const TraceRecord&)> observer) {
    std::lock_guard<std::mutex> lock(mu_);
    observer_ = std::move(observer);
}

void TraceWriter::append(const TraceRecord& record) {
    // Serialise outside the lock: it is the expensive part and it needs nothing
    // this object owns.
    const std::string line = record.to_json().dump();

    std::function<void(const TraceRecord&)> observer;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!out_.is_open()) {
            ++errors_;
            return;
        }
        out_ << line << '\n';
        // Flush to the OS on every record, but do not fsync. A record is small
        // and the rate is a handful per second, so the cost is negligible;
        // fsync per job would add milliseconds to a latency measurement this
        // file exists to record, which would be self-defeating. The trade is
        // that a machine losing power may lose the last records — acceptable
        // for a measurement log, and the reader tolerates a truncated line.
        out_.flush();
        if (!out_) {
            ++errors_;
            // Losing the trace silently would be the worst failure this project
            // has: every downstream conclusion would then be drawn from a file
            // with holes in it. Say so, loudly.
            RF_ERROR("trace write failed on %s (%llu total)", path_.c_str(),
                     static_cast<unsigned long long>(errors_));
            out_.clear();
        } else {
            ++written_;
        }
        observer = observer_;
    }

    // Invoked with the lock released: an observer that blocked while holding it
    // would stall every dispatch thread behind it.
    if (observer) observer(record);
}

uint64_t TraceWriter::records_written() const {
    std::lock_guard<std::mutex> lock(mu_);
    return written_;
}

uint64_t TraceWriter::write_errors() const {
    std::lock_guard<std::mutex> lock(mu_);
    return errors_;
}

bool read_trace(const std::string& path,
                const std::function<void(const TraceRecord&)>& on_record,
                TraceReadStats* stats, std::string* err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) *err = "cannot read trace file " + path;
        return false;
    }

    TraceReadStats local;
    std::string line;
    while (std::getline(in, line)) {
        ++local.lines;
        if (line.empty()) continue;
        // A file written on one platform and read on another can carry CR.
        if (line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        Json j;
        std::string parse_err;
        if (!Json::parse(line, j, &parse_err)) {
            ++local.parse_errors;
            RF_DEBUG("trace %s:%llu: %s", path.c_str(),
                     static_cast<unsigned long long>(local.lines), parse_err.c_str());
            continue;
        }

        TraceRecord record;
        std::string rec_err;
        if (!TraceRecord::from_json(j, record, &rec_err)) {
            ++local.parse_errors;
            RF_DEBUG("trace %s:%llu: %s", path.c_str(),
                     static_cast<unsigned long long>(local.lines), rec_err.c_str());
            continue;
        }
        // Parsed, but written by a newer schema than this binary knows: the
        // record is usable (unknown fields are ignored, rule 3) and counted so
        // the operator can see they are reading a file from a newer build.
        if (j["v"].as_i64(0) > TraceRecord::kSchemaVersion) ++local.newer_version;

        ++local.parsed;
        if (on_record) on_record(record);
    }

    if (local.parse_errors > 0)
        RF_WARN("trace %s: skipped %llu malformed line(s) of %llu", path.c_str(),
                static_cast<unsigned long long>(local.parse_errors),
                static_cast<unsigned long long>(local.lines));
    if (local.newer_version > 0)
        RF_WARN("trace %s: %llu record(s) from a newer schema version; read partially",
                path.c_str(), static_cast<unsigned long long>(local.newer_version));

    if (stats) *stats = local;
    return true;
}

}  // namespace rf
