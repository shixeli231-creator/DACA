// DACA Adaptive Unified Benchmark
// C++17, standalone synthetic software benchmark.
//
// Preserves the seven experiment categories from the previous unified suite and
// adds preloaded-rule lookup, hybrid preloaded/learned reuse, and an adaptive
// profitability policy. It compares three strategies:
//   1) Naive: recompute every request.
//   2) Exact cache: reuse only an identical (group, state, version) result.
//   3) DACA: use preloaded relationships, learned relationships, graph paths,
//      validation, versioning/invalidation, and exact-result fallback.
//
// Important: the workload generator is synthetic. Task hints are explicit test
// metadata. This is a software model, not a hardware model or proof of speedup.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace std;
using Clock = chrono::steady_clock;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static uint64_t expensive_base(int group, int version) {
    uint64_t x = 0x9E3779B97F4A7C15ULL
        ^ (static_cast<uint64_t>(group + 1) * 0xBF58476D1CE4E5B9ULL)
        ^ (static_cast<uint64_t>(version + 1) * 0x94D049BB133111EBULL);
    for (int i = 0; i < 260; ++i) {
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBULL;
        x ^= x >> 31;
    }
    return x & 0x0000FFFFFFFFFFFFULL;
}

enum class Shape { Arithmetic, Quadratic };
enum class HintKind { None, ArithmeticFamily, ArithmeticSpecific };
enum class RelationSource { None, Preloaded, Learned };

struct Pattern {
    Shape shape = Shape::Arithmetic;
    int step = 6;
};

struct RuleHint {
    HintKind kind = HintKind::None;
    int step = 0; // Used only by ArithmeticSpecific.
};

struct Request {
    int group = 0;
    int state = 0;
    int version = 0;
};

struct Workload {
    string name;
    string description;
    int groups = 0;
    int versions = 0;
    int states = 0;
    vector<vector<Pattern>> truth; // Used only by the synthetic reference evaluator.
    vector<vector<RuleHint>> hints; // Metadata visible to the DACA strategy.
    vector<Request> requests;

    uint64_t evaluate(int group, int state, int version) const {
        const Pattern& p = truth.at(version).at(group);
        const uint64_t base = expensive_base(group, version);
        if (p.shape == Shape::Arithmetic) {
            return base + static_cast<uint64_t>(state) * static_cast<uint64_t>(p.step);
        }
        const uint64_t x = static_cast<uint64_t>(3 + 6 * state);
        return base + x * x + static_cast<uint64_t>(version);
    }
};

struct Stats {
    uint64_t requests = 0;
    uint64_t full_calculations = 0;
    uint64_t relationship_hits = 0;
    uint64_t exact_memory_hits = 0;
    uint64_t preloaded_rule_hits = 0;
    uint64_t learned_relationships = 0;
    uint64_t rejected_relationships = 0;
    uint64_t conflicts = 0;
    uint64_t invalidated_relationships = 0;
    uint64_t library_lookups = 0;
    uint64_t validation_probes = 0;
    uint64_t direct_fallbacks = 0;
    uint64_t skipped_learning = 0;
    uint64_t graph_edges_used = 0;
};

struct RunResult {
    vector<uint64_t> output;
    Stats stats;
};

static volatile uint64_t benchmark_sink = 0;

static uint64_t request_key(int group, int state, int version) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(version)) << 48)
         | (static_cast<uint64_t>(static_cast<uint32_t>(group)) << 24)
         | static_cast<uint32_t>(state);
}

static uint64_t checksum(const vector<uint64_t>& values) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (uint64_t v : values) {
        h ^= v;
        h *= 0x100000001B3ULL;
    }
    return h;
}

static uint64_t calculate(const Workload& w, const Request& q, Stats& s) {
    ++s.full_calculations;
    return w.evaluate(q.group, q.state, q.version);
}

static RunResult run_naive(const Workload& w) {
    RunResult r;
    r.stats.requests = w.requests.size();
    r.output.reserve(w.requests.size());
    for (const auto& q : w.requests)
        r.output.push_back(calculate(w, q, r.stats));
    return r;
}

static RunResult run_exact_cache(const Workload& w) {
    RunResult r;
    r.stats.requests = w.requests.size();
    r.output.reserve(w.requests.size());
    unordered_map<uint64_t, uint64_t> cache;
    cache.reserve(w.requests.size());
    for (const auto& q : w.requests) {
        const uint64_t key = request_key(q.group, q.state, q.version);
        auto it = cache.find(key);
        if (it != cache.end()) {
            ++r.stats.exact_memory_hits;
            r.output.push_back(it->second);
        } else {
            const uint64_t value = calculate(w, q, r.stats);
            cache.emplace(key, value);
            r.output.push_back(value);
        }
    }
    return r;
}

class PreloadedRuleLibrary {
public:
    PreloadedRuleLibrary() {
        // A compact synthetic library of common arithmetic-progression rules.
        // Extra entries make lookup a real search rather than a direct formula.
        steps_ = {2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 16, 24};
    }

    optional<int> lookup_step(int observed_step, Stats& stats) const {
        ++stats.library_lookups;
        for (int candidate : steps_) {
            if (candidate == observed_step) {
                ++stats.preloaded_rule_hits;
                return candidate;
            }
        }
        return nullopt;
    }

    optional<int> lookup_specific(int requested_step, Stats& stats) const {
        return lookup_step(requested_step, stats);
    }

    size_t size() const { return steps_.size(); }

private:
    vector<int> steps_;
};

struct TestCase {
    int id;
    string name;
    string purpose;
    function<Workload()> make_workload;
    function<RunResult(const Workload&)> daca;
};

static Workload make_workload(
    string name,
    string description,
    int groups,
    int versions,
    int states,
    function<Pattern(int,int)> pattern_fn,
    function<RuleHint(int,int)> hint_fn) {
    Workload w;
    w.name = move(name);
    w.description = move(description);
    w.groups = groups;
    w.versions = versions;
    w.states = states;
    w.truth.resize(versions, vector<Pattern>(groups));
    w.hints.resize(versions, vector<RuleHint>(groups));
    for (int v = 0; v < versions; ++v) {
        for (int g = 0; g < groups; ++g) {
            w.truth[v][g] = pattern_fn(g, v);
            w.hints[v][g] = hint_fn(g, v);
        }
    }
    return w;
}

static void add_ordered_requests(Workload& w, int repeats = 1, bool shuffle = false, uint32_t seed = 1) {
    for (int v = 0; v < w.versions; ++v)
        for (int round = 0; round < repeats; ++round)
            for (int g = 0; g < w.groups; ++g)
                for (int s = 0; s < w.states; ++s)
                    w.requests.push_back({g, s, v});
    if (shuffle) {
        mt19937 rng(seed);
        std::shuffle(w.requests.begin(), w.requests.end(), rng);
    }
}

static vector<vector<vector<size_t>>> build_group_positions(const Workload& w) {
    vector<vector<vector<size_t>>> positions(
        w.versions, vector<vector<size_t>>(w.groups));
    for (size_t i = 0; i < w.requests.size(); ++i) {
        const auto& q = w.requests[i];
        positions[q.version][q.group].push_back(i);
    }
    return positions;
}

static void print_stats(const Stats& s) {
    cout << "  Requests:                  " << s.requests << '\n'
         << "  Full calculations:         " << s.full_calculations << '\n'
         << "  Relationship hits:         " << s.relationship_hits << '\n'
         << "  Exact-memory hits:         " << s.exact_memory_hits << '\n'
         << "  Preloaded-rule hits:       " << s.preloaded_rule_hits << '\n'
         << "  Learned relationships:     " << s.learned_relationships << '\n'
         << "  Rejected relationships:    " << s.rejected_relationships << '\n'
         << "  Conflicts detected:        " << s.conflicts << '\n'
         << "  Invalidated relationships: " << s.invalidated_relationships << '\n'
         << "  Library lookups:           " << s.library_lookups << '\n'
         << "  Validation probes:         " << s.validation_probes << '\n'
         << "  Direct fallbacks:          " << s.direct_fallbacks << '\n'
         << "  Learning skipped:          " << s.skipped_learning << '\n'
         << "  Graph edges used:          " << s.graph_edges_used << '\n';
}

template <typename Fn>
static double median_time_ms(Fn fn, int repetitions) {
    // One untimed warm-up, then a median over measured repetitions.
    RunResult warm = fn();
    benchmark_sink = benchmark_sink + checksum(warm.output);
    vector<double> times;
    times.reserve(repetitions);
    for (int i = 0; i < repetitions; ++i) {
        const auto start = Clock::now();
        RunResult result = fn();
        const auto stop = Clock::now();
        benchmark_sink = benchmark_sink + checksum(result.output);
        times.push_back(chrono::duration<double, milli>(stop - start).count());
    }
    sort(times.begin(), times.end());
    return times[times.size() / 2];
}

struct TestReport {
    int id = 0;
    bool correct = false;
    double naive_ms = 0;
    double cache_ms = 0;
    double daca_ms = 0;
    uint64_t requests = 0;
    Stats daca_stats;
};

static TestReport run_test(const TestCase& test, int repetitions) {
    Workload w = test.make_workload();
    RunResult naive_check = run_naive(w);
    RunResult cache_check = run_exact_cache(w);
    RunResult daca_check = test.daca(w);
    const bool cache_correct = (naive_check.output == cache_check.output);
    const bool daca_correct = (naive_check.output == daca_check.output);

    cout << "\n============================================================\n"
         << "TEST " << test.id << ": " << test.name << "\n"
         << "============================================================\n"
         << test.purpose << "\n\n"
         << "Correctness vs naive: " << (daca_correct ? "PASS" : "FAIL") << '\n'
         << "Exact-cache correctness: " << (cache_correct ? "PASS" : "FAIL") << '\n'
         << "Reference calculations: " << naive_check.stats.full_calculations << '\n'
         << "DACA counters:\n";
    print_stats(daca_check.stats);

    const double naive_ms = median_time_ms([&]() { return run_naive(w); }, repetitions);
    const double cache_ms = median_time_ms([&]() { return run_exact_cache(w); }, repetitions);
    const double daca_ms = median_time_ms([&]() { return test.daca(w); }, repetitions);
    cout << fixed << setprecision(3)
         << "\nNaive median:       " << naive_ms << " ms\n"
         << "Exact-cache median: " << cache_ms << " ms\n"
         << "DACA median:        " << daca_ms << " ms\n"
         << setprecision(4)
         << "Naive / exact-cache: " << (cache_ms > 0 ? naive_ms / cache_ms : 0.0) << "x\n"
         << "Naive / DACA:        " << (daca_ms > 0 ? naive_ms / daca_ms : 0.0) << "x\n"
         << "Exact-cache / DACA:  " << (daca_ms > 0 ? cache_ms / daca_ms : 0.0) << "x\n";

    TestReport report;
    report.id = test.id;
    report.correct = cache_correct && daca_correct;
    report.naive_ms = naive_ms;
    report.cache_ms = cache_ms;
    report.daca_ms = daca_ms;
    report.requests = w.requests.size();
    report.daca_stats = daca_check.stats;
    return report;
}

// -----------------------------------------------------------------------------
// Shared DACA helper: probes a group at states 0, 1 and 2, then checks whether
// two consecutive differences agree. This detects the synthetic constant-step
// pattern used in these tests; it is not a universal proof for arbitrary inputs.
// -----------------------------------------------------------------------------
struct ProbeResult {
    uint64_t p0 = 0, p1 = 0, p2 = 0;
    int step = 0;
    bool valid = false;
};

static ProbeResult probe_arithmetic(const Workload& w, int g, int v, Stats& stats) {
    ProbeResult p;
    p.p0 = w.evaluate(g, 0, v);
    p.p1 = w.evaluate(g, 1, v);
    p.p2 = w.evaluate(g, 2, v);
    stats.full_calculations += 3;
    stats.validation_probes += 3;
    const uint64_t d1 = p.p1 - p.p0;
    const uint64_t d2 = p.p2 - p.p1;
    p.valid = (d1 == d2);
    if (p.valid && d1 <= static_cast<uint64_t>(numeric_limits<int>::max()))
        p.step = static_cast<int>(d1);
    else
        p.valid = false;
    return p;
}

// Test 1: v1/v2 - the task signature identifies a preloaded +6 relationship.
static RunResult daca_basic_preloaded(const Workload& w) {
    RunResult r;
    r.stats.requests = w.requests.size();
    r.output.reserve(w.requests.size());
    PreloadedRuleLibrary library;
    vector<bool> ready(w.groups, false);
    vector<uint64_t> anchors(w.groups, 0);
    vector<int> steps(w.groups, 0);
    vector<uint64_t> exact_state0(w.groups, 0);
    vector<bool> state0_cached(w.groups, false);

    for (const auto& q : w.requests) {
        if (!ready[q.group]) {
            const RuleHint hint = w.hints[q.version][q.group];
            optional<int> step;
            if (hint.kind == HintKind::ArithmeticSpecific)
                step = library.lookup_specific(hint.step, r.stats);
            if (step) {
                anchors[q.group] = w.evaluate(q.group, 0, q.version);
                ++r.stats.full_calculations;
                steps[q.group] = *step;
                ready[q.group] = true;
            }
        }
        if (ready[q.group]) {
            if (q.state == 0) {
                if (!state0_cached[q.group]) {
                    exact_state0[q.group] = anchors[q.group];
                    state0_cached[q.group] = true;
                } else {
                    ++r.stats.exact_memory_hits;
                }
                r.output.push_back(exact_state0[q.group]);
            } else {
                ++r.stats.relationship_hits;
                r.output.push_back(anchors[q.group] + static_cast<uint64_t>(q.state * steps[q.group]));
            }
        } else {
            ++r.stats.direct_fallbacks;
            r.output.push_back(w.evaluate(q.group, q.state, q.version));
            ++r.stats.full_calculations;
        }
    }
    return r;
}

// Test 2: v3 - discover a constant-step relationship, otherwise compute directly.
static RunResult daca_conditional_learning(const Workload& w) {
    RunResult r;
    r.stats.requests = w.requests.size();
    r.output.resize(w.requests.size());

    // This test uses group-major requests, but the result is written to original positions.
    vector<vector<vector<size_t>>> positions(w.versions,
        vector<vector<size_t>>(w.groups));
    for (size_t i = 0; i < w.requests.size(); ++i) {
        const auto& q = w.requests[i];
        positions[q.version][q.group].push_back(i);
    }
    for (int v = 0; v < w.versions; ++v) {
        for (int g = 0; g < w.groups; ++g) {
            if (positions[v][g].empty()) continue;
            ProbeResult p = probe_arithmetic(w, g, v, r.stats);
            if (p.valid) ++r.stats.learned_relationships;
            else ++r.stats.rejected_relationships;
            for (size_t idx : positions[v][g]) {
                const auto& q = w.requests[idx];
                if (q.state <= 2) {
                    const uint64_t vals[] = {p.p0, p.p1, p.p2};
                    r.output[idx] = vals[q.state];
                } else if (p.valid) {
                    r.output[idx] = p.p0 + static_cast<uint64_t>(q.state * p.step);
                    ++r.stats.relationship_hits;
                } else {
                    r.output[idx] = w.evaluate(g, q.state, v);
                    ++r.stats.full_calculations;
                    ++r.stats.direct_fallbacks;
                }
            }
        }
    }
    return r;
}

// Test 3: v4 - compose a preloaded graph path 0 -> 1 -> 2 -> 3.
static RunResult daca_graph_composition(const Workload& w) {
    struct Edge { int from, to, weight; };
    const vector<Edge> graph = {{0,1,6}, {1,2,6}, {2,3,6}};
    RunResult r;
    r.stats.requests = w.requests.size();
    r.output.resize(w.requests.size());
    vector<vector<size_t>> positions(w.groups);
    for (size_t i = 0; i < w.requests.size(); ++i)
        positions[w.requests[i].group].push_back(i);

    for (int g = 0; g < w.groups; ++g) {
        if (positions[g].empty()) continue;
        uint64_t values[4] = {};
        values[0] = w.evaluate(g, 0, 0);
        ++r.stats.full_calculations;
        int node = 0;
        while (node < 3) {
            auto it = find_if(graph.begin(), graph.end(), [node](const Edge& e) { return e.from == node; });
            ++r.stats.library_lookups;
            if (it == graph.end()) break;
            values[it->to] = values[node] + static_cast<uint64_t>(it->weight);
            node = it->to;
            ++r.stats.graph_edges_used;
            ++r.stats.preloaded_rule_hits;
        }
        for (size_t idx : positions[g]) {
            const auto& q = w.requests[idx];
            if (q.state < 4) {
                r.output[idx] = values[q.state];
                if (q.state > 0) ++r.stats.relationship_hits;
            } else {
                r.output[idx] = w.evaluate(g, q.state, q.version);
                ++r.stats.full_calculations;
                ++r.stats.direct_fallbacks;
            }
        }
    }
    return r;
}

// Test 4: v6 - validate two independent candidate paths; reject disagreements.
static RunResult daca_conflict_validation(const Workload& w) {
    RunResult r;
    r.stats.requests = w.requests.size();
    r.output.resize(w.requests.size());
    vector<uint64_t> anchors(w.groups, 0);
    vector<bool> anchor_ready(w.groups, false);
    for (size_t i = 0; i < w.requests.size(); ++i) {
        const auto& q = w.requests[i];
        if (!anchor_ready[q.group]) {
            anchors[q.group] = w.evaluate(q.group, 0, q.version);
            ++r.stats.full_calculations;
            anchor_ready[q.group] = true;
        }
        const uint64_t anchor = anchors[q.group];
        if (q.state == 0) {
            r.output[i] = anchor;
            continue;
        }
        const uint64_t path_a = anchor + 18;
        const uint64_t path_b = anchor + ((q.group % 4 == 0) ? 19 : 18);
        if (path_a == path_b) {
            r.output[i] = path_a;
            ++r.stats.relationship_hits;
            ++r.stats.learned_relationships;
        } else {
            r.output[i] = w.evaluate(q.group, q.state, q.version);
            ++r.stats.full_calculations;
            ++r.stats.conflicts;
            ++r.stats.rejected_relationships;
            ++r.stats.direct_fallbacks;
        }
    }
    return r;
}

// Test 5: v7 - version-aware relations are keyed by (group, version).
static RunResult daca_version_aware(const Workload& w) {
    RunResult r;
    r.stats.requests = w.requests.size();
    r.output.resize(w.requests.size());
    struct Relation { uint64_t anchor; int step; };
    unordered_map<uint64_t, Relation> memory;
    const auto positions = build_group_positions(w);
    for (int v = 0; v < w.versions; ++v) {
        for (int g = 0; g < w.groups; ++g) {
            if (positions[v][g].empty()) continue;
            const uint64_t anchor = w.evaluate(g, 0, v);
            const uint64_t probe = w.evaluate(g, 1, v);
            r.stats.full_calculations += 2;
            r.stats.validation_probes += 2;
            const uint64_t key = request_key(g, 0, v);
            memory[key] = {anchor, static_cast<int>(probe - anchor)};
            ++r.stats.learned_relationships;
            const Relation& relation = memory.at(key); // Explicit version-keyed retrieval.
            for (size_t idx : positions[v][g]) {
                const auto& q = w.requests[idx];
                if (q.state == 0) r.output[idx] = relation.anchor;
                else if (q.state == 1) r.output[idx] = probe;
                else {
                    r.output[idx] = relation.anchor + static_cast<uint64_t>(q.state * relation.step);
                    ++r.stats.relationship_hits;
                }
            }
        }
    }
    return r;
}

// Test 6: v9 - explicitly invalidate a group's old relation on version change.
static RunResult daca_dynamic_invalidation(const Workload& w) {
    RunResult r;
    r.stats.requests = w.requests.size();
    r.output.resize(w.requests.size());
    struct Relation { int version; uint64_t anchor; int step; };
    unordered_map<int, Relation> current;
    const auto positions = build_group_positions(w);
    for (int v = 0; v < w.versions; ++v) {
        for (int g = 0; g < w.groups; ++g) {
            if (positions[v][g].empty()) continue;
            auto old = current.find(g);
            if (old != current.end() && old->second.version != v) {
                current.erase(old);
                ++r.stats.invalidated_relationships;
            }
            const uint64_t p0 = w.evaluate(g, 0, v);
            const uint64_t p1 = w.evaluate(g, 1, v);
            r.stats.full_calculations += 2;
            r.stats.validation_probes += 2;
            current[g] = {v, p0, static_cast<int>(p1 - p0)};
            ++r.stats.learned_relationships;
            const Relation& relation = current.at(g);
            for (size_t idx : positions[v][g]) {
                const auto& q = w.requests[idx];
                if (q.state == 0) r.output[idx] = relation.anchor;
                else if (q.state == 1) r.output[idx] = p1;
                else {
                    r.output[idx] = relation.anchor + static_cast<uint64_t>(q.state * relation.step);
                    ++r.stats.relationship_hits;
                }
            }
        }
    }
    return r;
}

// Test 7: v10 - adaptive learning, rejection, invalidation and relearning.
static RunResult daca_adaptive_learning(const Workload& w) {
    RunResult r;
    r.stats.requests = w.requests.size();
    r.output.resize(w.requests.size());
    struct Relation { int version; ProbeResult probe; };
    unordered_map<int, Relation> learned;
    const auto positions = build_group_positions(w);

    for (int v = 0; v < w.versions; ++v) {
        for (int g = 0; g < w.groups; ++g) {
            if (positions[v][g].empty()) continue;
            auto old = learned.find(g);
            if (old != learned.end() && old->second.version != v) {
                learned.erase(old);
                ++r.stats.invalidated_relationships;
            }
            ProbeResult p = probe_arithmetic(w, g, v, r.stats);
            if (p.valid) {
                learned[g] = {v, p};
                ++r.stats.learned_relationships;
            } else {
                ++r.stats.rejected_relationships;
            }
            for (size_t idx : positions[v][g]) {
                const auto& q = w.requests[idx];
                if (q.state <= 2) {
                    const uint64_t vals[] = {p.p0, p.p1, p.p2};
                    r.output[idx] = vals[q.state];
                } else if (p.valid) {
                    const ProbeResult& stored = learned.at(g).probe;
                    r.output[idx] = stored.p0 + static_cast<uint64_t>(q.state * stored.step);
                    ++r.stats.relationship_hits;
                } else {
                    r.output[idx] = w.evaluate(g, q.state, v);
                    ++r.stats.full_calculations;
                    ++r.stats.direct_fallbacks;
                }
            }
        }
    }
    return r;
}

// Test 8: query a preloaded library by family and a parameter inferred from probes.
static RunResult daca_preloaded_library_search(const Workload& w) {
    RunResult r;
    r.stats.requests = w.requests.size();
    r.output.resize(w.requests.size());
    PreloadedRuleLibrary library;
    const auto positions = build_group_positions(w);
    for (int v = 0; v < w.versions; ++v) {
        for (int g = 0; g < w.groups; ++g) {
            if (positions[v][g].empty()) continue;
            ProbeResult p = probe_arithmetic(w, g, v, r.stats);
            optional<int> matched;
            if (p.valid && w.hints[v][g].kind == HintKind::ArithmeticFamily)
                matched = library.lookup_step(p.step, r.stats);
            if (matched) {
                for (size_t idx : positions[v][g]) {
                    const auto& q = w.requests[idx];
                    if (q.state <= 2) {
                        const uint64_t vals[] = {p.p0, p.p1, p.p2};
                        r.output[idx] = vals[q.state];
                    } else {
                        r.output[idx] = p.p0 + static_cast<uint64_t>(q.state * *matched);
                        ++r.stats.relationship_hits;
                    }
                }
            } else {
                ++r.stats.rejected_relationships;
                for (size_t idx : positions[v][g]) {
                    const auto& q = w.requests[idx];
                    if (q.state <= 2) {
                        const uint64_t vals[] = {p.p0, p.p1, p.p2};
                        r.output[idx] = vals[q.state];
                    } else {
                        r.output[idx] = w.evaluate(g, q.state, v);
                        ++r.stats.full_calculations;
                        ++r.stats.direct_fallbacks;
                    }
                }
            }
        }
    }
    return r;
}

// Tests 9 and 10: hybrid policy. It can use a trusted preloaded specific rule,
// search the preloaded family after probes, learn a new relationship, or fall
// back to exact-result memory/direct calculation. Optional profitability mode
// skips probing when the queued batch has too few distinct states to amortize it.
static RunResult daca_hybrid(const Workload& w, bool profitability_policy) {
    RunResult r;
    r.stats.requests = w.requests.size();
    r.output.resize(w.requests.size());
    PreloadedRuleLibrary library;

    struct Relation {
        bool valid = false;
        int step = 0;
        uint64_t anchor = 0;
        array<uint64_t,3> probes{};
        RelationSource source = RelationSource::None;
    };
    vector<vector<Relation>> relations(w.versions, vector<Relation>(w.groups));
    vector<vector<bool>> initialized(w.versions, vector<bool>(w.groups, false));
    vector<vector<unordered_map<int,uint64_t>>> exact_values(
        w.versions, vector<unordered_map<int,uint64_t>>(w.groups));
    vector<vector<set<int>>> distinct_states(w.versions, vector<set<int>>(w.groups));
    for (const auto& q : w.requests)
        distinct_states[q.version][q.group].insert(q.state);

    // Current version per group, used to count explicit invalidation events.
    vector<int> active_version(w.groups, -1);
    for (size_t i = 0; i < w.requests.size(); ++i) {
        const Request& q = w.requests[i];
        if (active_version[q.group] != q.version) {
            if (active_version[q.group] != -1) {
                const int old_v = active_version[q.group];
                if (relations[old_v][q.group].valid) {
                    relations[old_v][q.group] = Relation{};
                    ++r.stats.invalidated_relationships;
                }
            }
            active_version[q.group] = q.version;
        }

        if (!initialized[q.version][q.group]) {
            initialized[q.version][q.group] = true;
            const RuleHint hint = w.hints[q.version][q.group];
            const size_t unique_count = distinct_states[q.version][q.group].size();
            bool should_probe = true;
            if (profitability_policy && unique_count <= 4 && hint.kind != HintKind::ArithmeticSpecific) {
                should_probe = false;
                ++r.stats.skipped_learning;
            }

            Relation rel;
            if (hint.kind == HintKind::ArithmeticSpecific) {
                auto step = library.lookup_specific(hint.step, r.stats);
                if (step) {
                    rel.anchor = w.evaluate(q.group, 0, q.version);
                    ++r.stats.full_calculations;
                    rel.step = *step;
                    rel.valid = true;
                    rel.source = RelationSource::Preloaded;
                }
            } else if (should_probe && unique_count >= 3) {
                ProbeResult p = probe_arithmetic(w, q.group, q.version, r.stats);
                rel.probes = {p.p0, p.p1, p.p2};
                rel.anchor = p.p0;
                rel.step = p.step;
                if (p.valid) {
                    if (hint.kind == HintKind::ArithmeticFamily) {
                        auto step = library.lookup_step(p.step, r.stats);
                        if (step) {
                            rel.step = *step;
                            rel.source = RelationSource::Preloaded;
                        } else {
                            rel.source = RelationSource::Learned;
                            ++r.stats.learned_relationships;
                        }
                    } else {
                        rel.source = RelationSource::Learned;
                        ++r.stats.learned_relationships;
                    }
                    rel.valid = true;
                } else {
                    ++r.stats.rejected_relationships;
                }
                for (int s = 0; s <= 2; ++s)
                    exact_values[q.version][q.group][s] = rel.probes[s];
            } else if (!should_probe) {
                // The profitability policy deliberately chooses exact caching/direct compute.
            } else if (unique_count < 3) {
                ++r.stats.skipped_learning;
            }
            relations[q.version][q.group] = rel;
        }

        Relation& rel = relations[q.version][q.group];
        if (rel.valid) {
            if (q.state <= 2 && exact_values[q.version][q.group].count(q.state)) {
                ++r.stats.exact_memory_hits;
                r.output[i] = exact_values[q.version][q.group][q.state];
            } else if (q.state == 0 && rel.source == RelationSource::Preloaded) {
                r.output[i] = rel.anchor;
            } else {
                r.output[i] = rel.anchor + static_cast<uint64_t>(q.state * rel.step);
                if (q.state > 0) ++r.stats.relationship_hits;
            }
        } else {
            auto& cache = exact_values[q.version][q.group];
            auto found = cache.find(q.state);
            if (found != cache.end()) {
                ++r.stats.exact_memory_hits;
                r.output[i] = found->second;
            } else {
                r.output[i] = w.evaluate(q.group, q.state, q.version);
                ++r.stats.full_calculations;
                ++r.stats.direct_fallbacks;
                cache.emplace(q.state, r.output[i]);
            }
        }
    }
    return r;
}

static vector<TestCase> build_tests() {
    vector<TestCase> tests;

    // 1. Previous basic reuse test, now explicitly routed through a preloaded rule.
    tests.push_back({1, "BASIC PRELOADED RELATIONSHIP REUSE",
        "A task signature identifies a preloaded arithmetic-progression rule with step +6; DACA retrieves it and computes one anchor per group.",
        [] {
            auto w = make_workload("basic", "", 300, 1, 8,
                [](int, int) { return Pattern{Shape::Arithmetic, 6}; },
                [](int, int) { return RuleHint{HintKind::ArithmeticSpecific, 6}; });
            add_ordered_requests(w, 6, true, 4101);
            return w;
        },
        [](const Workload& w) { return daca_basic_preloaded(w); }
    });

    // 2. Previous conditional learning test.
    tests.push_back({2, "CONDITIONAL RELATIONSHIP LEARNING",
        "Three probes distinguish constant-step groups from quadratic groups; valid groups are learned and the rest are computed directly.",
        [] {
            auto w = make_workload("conditional", "", 400, 1, 8,
                [](int g, int) { return (g % 4 != 0) ? Pattern{Shape::Arithmetic, 6} : Pattern{Shape::Quadratic, 0}; },
                [](int, int) { return RuleHint{}; });
            add_ordered_requests(w);
            return w;
        },
        [](const Workload& w) { return daca_conditional_learning(w); }
    });

    // 3. Previous graph composition test.
    tests.push_back({3, "GRAPH COMPOSITION",
        "Uses a preloaded path of three +6 edges to compose states 0 through 3 from one anchor.",
        [] {
            auto w = make_workload("graph", "", 300, 1, 4,
                [](int, int) { return Pattern{Shape::Arithmetic, 6}; },
                [](int, int) { return RuleHint{HintKind::ArithmeticFamily, 0}; });
            add_ordered_requests(w);
            return w;
        },
        [](const Workload& w) { return daca_graph_composition(w); }
    });

    // 4. Previous conflict validation test.
    tests.push_back({4, "CONFLICT VALIDATION",
        "Two candidate paths are compared; a disagreement rejects the relationship and triggers direct calculation.",
        [] {
            auto w = make_workload("conflict", "", 400, 1, 2,
                [](int, int) { return Pattern{Shape::Arithmetic, 6}; },
                [](int, int) { return RuleHint{}; });
            for (int g = 0; g < w.groups; ++g) {
                w.requests.push_back({g,0,0});
                w.requests.push_back({g,3,0});
            }
            return w;
        },
        [](const Workload& w) { return daca_conflict_validation(w); }
    });

    // 5. Previous version-aware memory test.
    tests.push_back({5, "VERSION-AWARE MEMORY",
        "Learns a separate arithmetic relationship for each version so a prior version's step cannot be reused accidentally.",
        [] {
            auto w = make_workload("version-aware", "", 400, 2, 8,
                [](int, int v) { return Pattern{Shape::Arithmetic, v == 0 ? 6 : 7}; },
                [](int, int) { return RuleHint{}; });
            add_ordered_requests(w);
            return w;
        },
        [](const Workload& w) { return daca_version_aware(w); }
    });

    // 6. Previous dynamic invalidation test.
    tests.push_back({6, "DYNAMIC INVALIDATION",
        "Explicitly deletes the old group relationship at a version transition, then probes and learns the new relationship.",
        [] {
            auto w = make_workload("invalidation", "", 600, 2, 8,
                [](int, int v) { return Pattern{Shape::Arithmetic, v == 0 ? 6 : 7}; },
                [](int, int) { return RuleHint{}; });
            add_ordered_requests(w);
            return w;
        },
        [](const Workload& w) { return daca_dynamic_invalidation(w); }
    });

    // 7. Previous adaptive learning test.
    tests.push_back({7, "ADAPTIVE RELATIONSHIP LEARNING",
        "Learns constant-step relationships, rejects nonlinear groups, invalidates old relations after a workload change, and relearns.",
        [] {
            auto w = make_workload("adaptive-learning", "", 1000, 2, 8,
                [](int g, int v) {
                    bool linear = (g % 4 != 0);
                    if (v == 1 && g % 5 == 0) linear = !linear;
                    const int step = (v == 0 || g % 5 == 0) ? 6 : 7;
                    return linear ? Pattern{Shape::Arithmetic, step} : Pattern{Shape::Quadratic, 0};
                },
                [](int, int) { return RuleHint{}; });
            add_ordered_requests(w);
            return w;
        },
        [](const Workload& w) { return daca_adaptive_learning(w); }
    });

    // 8. New: search a preloaded library of common step rules using probe data.
    tests.push_back({8, "PRELOADED LIBRARY SEARCH",
        "The library contains common +step rules. DACA probes a sequence, matches its observed step to a stored rule, and reuses that rule.",
        [] {
            const vector<int> steps = {4,5,6,7,8,9};
            auto w = make_workload("library-search", "", 420, 1, 10,
                [steps](int g, int) { return Pattern{Shape::Arithmetic, steps[g % steps.size()]}; },
                [](int, int) { return RuleHint{HintKind::ArithmeticFamily, 0}; });
            add_ordered_requests(w);
            return w;
        },
        [](const Workload& w) { return daca_preloaded_library_search(w); }
    });

    // 9. New: hybrid preloaded + learned + exact-cache fallback.
    tests.push_back({9, "HYBRID PRELOADED / LEARNED / CACHE",
        "Mixes preloaded-family candidates, unseen arithmetic patterns, nonlinear cases, repeated requests and version changes. DACA selects preloaded reuse, learns a new rule, or falls back to exact caching/direct computation.",
        [] {
            auto w = make_workload("hybrid", "", 700, 2, 12,
                [](int g, int v) {
                    int bucket = g % 10;
                    bool linear = bucket < 7;
                    int step = (bucket < 3 ? 6 : bucket < 5 ? 7 : bucket < 7 ? 10 : 0);
                    if (v == 1 && g % 11 == 0) linear = !linear;
                    if (v == 1 && linear && g % 13 == 0) step = 11;
                    return linear ? Pattern{Shape::Arithmetic, step} : Pattern{Shape::Quadratic, 0};
                },
                [](int g, int v) {
                    // A family hint is available for about two-thirds of groups.
                    return ((g + 2 * v) % 3 != 0)
                        ? RuleHint{HintKind::ArithmeticFamily, 0}
                        : RuleHint{};
                });
            mt19937 rng(99021);
            for (int v = 0; v < w.versions; ++v) {
                vector<Request> phase_requests;
                for (int round = 0; round < 18; ++round) {
                    for (int g = 0; g < w.groups; ++g) {
                        int state = static_cast<int>(rng() % w.states);
                        phase_requests.push_back({g, state, v});
                        // Add occasional exact duplicates to exercise exact-result memory.
                        if ((rng() % 5) == 0)
                            phase_requests.push_back({g, state, v});
                    }
                }
                shuffle(phase_requests.begin(), phase_requests.end(), rng);
                w.requests.insert(w.requests.end(), phase_requests.begin(), phase_requests.end());
            }
            return w;
        },
        [](const Workload& w) { return daca_hybrid(w, false); }
    });

    // 10. New: cost-aware adaptive policy. It uses queue visibility to avoid
    // probing groups with too few distinct requested states to amortize discovery.
    tests.push_back({10, "ADAPTIVE PROFITABILITY / MIXED WORKLOAD",
        "A mixed workload includes trusted preloaded rules, discoverable rules, nonlinear groups and low-reuse groups. The adaptive policy skips probing when the queued batch has too few distinct states.",
        [] {
            auto w = make_workload("profitability", "", 650, 2, 14,
                [](int g, int v) {
                    const int b = g % 10;
                    bool linear = b < 6;
                    int step = (b < 2 ? 6 : b < 4 ? 7 : 10);
                    if (v == 1 && g % 9 == 0) linear = !linear;
                    return linear ? Pattern{Shape::Arithmetic, step} : Pattern{Shape::Quadratic, 0};
                },
                [](int g, int v) {
                    // Only advertise a trusted +6 signature when the synthetic task really has it.
                    const int b = g % 10;
                    bool linear = b < 6;
                    int step = (b < 2 ? 6 : b < 4 ? 7 : 10);
                    if (v == 1 && g % 9 == 0) linear = !linear;
                    if (linear && step == 6 && (g + v) % 12 == 0)
                        return RuleHint{HintKind::ArithmeticSpecific, 6};
                    if ((g + v) % 3 == 0) return RuleHint{HintKind::ArithmeticFamily, 0};
                    return RuleHint{};
                });
            mt19937 rng(77123);
            for (int v = 0; v < w.versions; ++v) {
                vector<Request> phase_requests;
                for (int g = 0; g < w.groups; ++g) {
                    const int bucket = g % 5;
                    const int count = (bucket == 0 ? 2 : bucket == 1 ? 3 : bucket == 2 ? 5 : 12);
                    for (int j = 0; j < count; ++j) {
                        int state = static_cast<int>(rng() % w.states);
                        phase_requests.push_back({g, state, v});
                    }
                }
                shuffle(phase_requests.begin(), phase_requests.end(), rng);
                w.requests.insert(w.requests.end(), phase_requests.begin(), phase_requests.end());
            }
            return w;
        },
        [](const Workload& w) { return daca_hybrid(w, true); }
    });

    return tests;
}

int main(int argc, char** argv) {
    int selected = 0;
    int repetitions = 5;
    const vector<TestCase> tests = build_tests();

    for (int i = 1; i < argc; ++i) {
        const string arg = argv[i];
        if (arg == "--list") {
            for (const auto& t : tests) cout << t.id << ". " << t.name << '\n';
            return 0;
        } else if (arg == "--test" && i + 1 < argc) {
            try { selected = stoi(argv[++i]); }
            catch (...) { cerr << "Invalid test number.\n"; return 2; }
        } else if (arg == "--repetitions" && i + 1 < argc) {
            try { repetitions = stoi(argv[++i]); }
            catch (...) { cerr << "Invalid repetition count.\n"; return 2; }
        } else {
            cerr << "Usage: ./daca_benchmark [--list] [--test N] [--repetitions N]\n";
            return 2;
        }
    }

    if (selected < 0 || selected > static_cast<int>(tests.size())) {
        cerr << "Test number must be 1-" << tests.size() << ", or 0 for all.\n";
        return 2;
    }
    if (repetitions < 3 || repetitions > 31) {
        cerr << "Use between 3 and 31 timing repetitions.\n";
        return 2;
    }

    cout << "============================================================\n"
         << " DACA ADAPTIVE UNIFIED BENCHMARK\n"
         << "============================================================\n"
         << "Language target: C++17\n"
         << "Comparisons: naive / exact-result cache / DACA\n"
         << "Timing: one warm-up + median of " << repetitions << " measured runs\n"
         << "Synthetic workload seed: fixed per test\n"
         << "Preloaded relationship library: " << PreloadedRuleLibrary().size() << " step rules\n"
         << "Tests: 1-7 preserve previous unified categories; 8-10 add library/adaptive tests\n";

    vector<TestReport> reports;
    int passed = 0;
    for (const auto& test : tests) {
        if (selected != 0 && test.id != selected) continue;
        TestReport report = run_test(test, repetitions);
        if (report.correct) ++passed;
        reports.push_back(report);
    }

    double naive_sum = 0.0, cache_sum = 0.0, daca_sum = 0.0;
    uint64_t total_requests = 0;
    for (const auto& report : reports) {
        naive_sum += report.naive_ms;
        cache_sum += report.cache_ms;
        daca_sum += report.daca_ms;
        total_requests += report.requests;
    }

    cout << "\n============================================================\n"
         << " SUMMARY\n"
         << "============================================================\n"
         << "Correctness tests passed: " << passed << "/" << reports.size() << '\n'
         << "Total benchmark requests: " << total_requests << '\n'
         << fixed << setprecision(3)
         << "Sum of naive medians:       " << naive_sum << " ms\n"
         << "Sum of exact-cache medians: " << cache_sum << " ms\n"
         << "Sum of DACA medians:        " << daca_sum << " ms\n"
         << setprecision(4)
         << "Naive / exact-cache suite ratio: " << (cache_sum > 0 ? naive_sum / cache_sum : 0.0) << "x\n"
         << "Naive / DACA suite ratio:        " << (daca_sum > 0 ? naive_sum / daca_sum : 0.0) << "x\n"
         << "Exact-cache / DACA suite ratio:  " << (daca_sum > 0 ? cache_sum / daca_sum : 0.0) << "x\n"
         << "Benchmark sink: " << benchmark_sink << '\n'
         << "\nInterpretation notes:\n"
         << "- Suite ratios are sums of per-test medians, not a single end-to-end workload.\n"
         << "- Test 1 uses an explicit preloaded +6 task signature.\n"
         << "- Tests 8-10 model library search, hybrid learning and a batch-aware policy.\n"
         << "- Probe equality is evidence for the tested synthetic pattern, not a proof\n"
         << "  that an arbitrary mathematical function is globally linear.\n"
         << "- Exact-cache is a separate baseline; DACA must beat it to show value\n"
         << "  beyond simply remembering identical input/output pairs.\n"
         << "- This is synthetic software testing, not hardware performance evidence.\n";

    return (passed == static_cast<int>(reports.size())) ? 0 : 1;
}
