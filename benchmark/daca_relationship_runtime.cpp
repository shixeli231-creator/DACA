// DACA relationship-memory runtime prototype.
//
// This is a user-space software model of the Remembering Core plus its
// request path. It stores computation records in RAM and supports:
//   * exact reuse for any caller-defined operation;
//   * compositional reuse for two built-in, validated schemas;
//   * context/version/dependency invalidation;
//   * a bounded RAM budget with value-aware eviction; and
//   * an opt-in Unix-domain-socket daemon API.
//
// It is intentionally conservative. A daemon cannot safely invent the meaning
// of arbitrary values, so new relationship types require a schema with explicit
// matching, composition, and validation rules.

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <unistd.h>

namespace daca {

constexpr std::uint32_t kProtocolVersion = 1;
constexpr std::size_t kMaxMessageBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaxStringBytes = 1024U * 1024U;
constexpr std::size_t kMaxInputs = 4096U;

enum class ValueKind : std::uint8_t {
    kInt64 = 1,
    kUtf8 = 2,
};

struct Value {
    ValueKind kind = ValueKind::kInt64;
    std::int64_t integer = 0;
    std::string text;

    static Value int64(std::int64_t value) {
        Value result;
        result.kind = ValueKind::kInt64;
        result.integer = value;
        return result;
    }

    static Value utf8(std::string value) {
        Value result;
        result.kind = ValueKind::kUtf8;
        result.text = std::move(value);
        return result;
    }

    bool operator==(const Value& other) const {
        if (kind != other.kind) {
            return false;
        }
        return kind == ValueKind::kInt64 ? integer == other.integer
                                         : text == other.text;
    }

    std::size_t byte_cost() const {
        return sizeof(Value) + (kind == ValueKind::kUtf8 ? text.size() : 0U);
    }
};

std::string value_label(const Value& value) {
    if (value.kind == ValueKind::kInt64) {
        return std::to_string(value.integer);
    }
    return '"' + value.text + '"';
}

struct Input {
    std::string id;
    std::uint64_t version = 0;
    Value value;
};

bool same_input(const Input& left, const Input& right) {
    return left.id == right.id && left.version == right.version &&
           left.value == right.value;
}

// semantic_version represents the version of the operation/schema itself.
// context carries the rest of the execution conditions: world seed, dimension,
// mod version, model weights, shape/dtype/precision, or another domain key.
struct Request {
    std::string operation;
    std::string context;
    std::uint64_t semantic_version = 0;
    std::uint64_t estimated_direct_cost = 1;
    std::vector<Input> inputs;
};

enum class RuleKind {
    kExactOnly,
    kCommutativeSumI64,
    kOrderedConcatUtf8,
};

RuleKind rule_for(const std::string& operation) {
    if (operation == "sum_i64") {
        return RuleKind::kCommutativeSumI64;
    }
    if (operation == "concat_utf8") {
        return RuleKind::kOrderedConcatUtf8;
    }
    return RuleKind::kExactOnly;
}

bool checked_add(std::int64_t left, std::int64_t right, std::int64_t* result) {
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
        return false;
    }
    *result = left + right;
    return true;
}

void append_field(std::string* destination, const std::string& field) {
    destination->append(std::to_string(field.size()));
    destination->push_back(':');
    destination->append(field);
}

std::string value_fingerprint(const Value& value) {
    std::string result;
    result.push_back(static_cast<char>(value.kind));
    if (value.kind == ValueKind::kInt64) {
        append_field(&result, std::to_string(value.integer));
    } else {
        append_field(&result, value.text);
    }
    return result;
}

std::string input_fingerprint(const Input& input) {
    std::string result;
    append_field(&result, input.id);
    append_field(&result, std::to_string(input.version));
    append_field(&result, value_fingerprint(input.value));
    return result;
}

std::string request_key(const Request& request) {
    std::vector<Input> ordered = request.inputs;
    if (rule_for(request.operation) == RuleKind::kCommutativeSumI64) {
        std::sort(ordered.begin(), ordered.end(), [](const Input& left, const Input& right) {
            return input_fingerprint(left) < input_fingerprint(right);
        });
    }

    std::string key;
    append_field(&key, request.operation);
    append_field(&key, request.context);
    append_field(&key, std::to_string(request.semantic_version));
    append_field(&key, std::to_string(ordered.size()));
    for (const Input& input : ordered) {
        append_field(&key, input_fingerprint(input));
    }
    return key;
}

bool validate_request(const Request& request, std::string* reason) {
    if (request.operation.empty()) {
        *reason = "operation must not be empty";
        return false;
    }
    if (request.context.empty()) {
        *reason = "context must not be empty";
        return false;
    }
    if (request.inputs.size() > kMaxInputs) {
        *reason = "too many inputs";
        return false;
    }

    switch (rule_for(request.operation)) {
        case RuleKind::kCommutativeSumI64:
            if (request.inputs.empty()) {
                *reason = "sum_i64 requires at least one input";
                return false;
            }
            for (const Input& input : request.inputs) {
                if (input.value.kind != ValueKind::kInt64) {
                    *reason = "sum_i64 requires int64 inputs";
                    return false;
                }
            }
            return true;

        case RuleKind::kOrderedConcatUtf8:
            if (request.inputs.empty()) {
                *reason = "concat_utf8 requires at least one input";
                return false;
            }
            for (const Input& input : request.inputs) {
                if (input.value.kind != ValueKind::kUtf8) {
                    *reason = "concat_utf8 requires UTF-8 inputs";
                    return false;
                }
            }
            return true;

        case RuleKind::kExactOnly:
            return true;
    }
    *reason = "unknown rule kind";
    return false;
}

// Known schemas can be validated without trusting the client. Exact-only
// schemas are still useful, but their result is supplied by the host program.
std::optional<Value> evaluate_known_schema(const Request& request,
                                           std::string* reason) {
    switch (rule_for(request.operation)) {
        case RuleKind::kCommutativeSumI64: {
            std::int64_t total = 0;
            for (const Input& input : request.inputs) {
                if (!checked_add(total, input.value.integer, &total)) {
                    *reason = "sum_i64 overflow";
                    return std::nullopt;
                }
            }
            return Value::int64(total);
        }

        case RuleKind::kOrderedConcatUtf8: {
            std::string total;
            for (const Input& input : request.inputs) {
                if (total.size() + input.value.text.size() > kMaxMessageBytes) {
                    *reason = "concat_utf8 result exceeds message limit";
                    return std::nullopt;
                }
                total += input.value.text;
            }
            return Value::utf8(std::move(total));
        }

        case RuleKind::kExactOnly:
            return std::nullopt;
    }
    return std::nullopt;
}

enum class Outcome : std::uint8_t {
    kExactHit = 1,
    kRelationshipHit = 2,
    kMiss = 3,
    kRejected = 4,
};

const char* outcome_name(Outcome outcome) {
    switch (outcome) {
        case Outcome::kExactHit:
            return "EXACT_HIT";
        case Outcome::kRelationshipHit:
            return "RELATIONSHIP_HIT";
        case Outcome::kMiss:
            return "MISS";
        case Outcome::kRejected:
            return "REJECTED";
    }
    return "UNKNOWN";
}

struct Resolution {
    Outcome outcome = Outcome::kMiss;
    std::optional<Value> value;
    std::vector<std::uint64_t> proof_record_ids;
    std::string reason;
};

struct LearnResult {
    bool admitted = false;
    std::uint64_t record_id = 0;
    std::string reason;
};

struct Statistics {
    std::uint64_t lookups = 0;
    std::uint64_t exact_hits = 0;
    std::uint64_t relationship_hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t rejected = 0;
    std::uint64_t learned = 0;
    std::uint64_t derived_records = 0;
    std::uint64_t evictions = 0;
    std::uint64_t invalidations = 0;
    std::uint64_t record_count = 0;
    std::uint64_t bytes_used = 0;
    std::uint64_t byte_budget = 0;
};

struct Record {
    std::uint64_t id = 0;
    std::string key;
    Request request;
    Value output;
    std::uint64_t estimated_direct_cost = 1;
    std::uint64_t hits = 0;
    std::uint64_t last_used = 0;
    bool derived = false;
    std::size_t byte_cost = 0;
};

std::size_t estimate_record_bytes(const Record& record) {
    std::size_t total = sizeof(Record) + record.key.size() +
                        record.request.operation.size() +
                        record.request.context.size() + record.output.byte_cost();
    for (const Input& input : record.request.inputs) {
        total += sizeof(Input) + input.id.size() + input.value.byte_cost();
    }
    return total;
}

class RelationshipMemory {
public:
    explicit RelationshipMemory(std::size_t byte_budget)
        : byte_budget_(byte_budget) {}

    LearnResult learn(const Request& request, const Value& output,
                      bool client_verified) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string reason;
        if (!validate_request(request, &reason)) {
            ++statistics_.rejected;
            return {false, 0, reason};
        }

        const std::optional<Value> expected = evaluate_known_schema(request, &reason);
        if (rule_for(request.operation) != RuleKind::kExactOnly) {
            if (!expected.has_value()) {
                ++statistics_.rejected;
                return {false, 0, reason};
            }
            if (!(output == *expected)) {
                ++statistics_.rejected;
                return {false, 0, "provided output fails schema validation"};
            }
        } else if (!client_verified) {
            ++statistics_.rejected;
            return {false, 0,
                    "exact-only records require caller confirmation of the direct result"};
        }

        Record record;
        record.key = request_key(request);
        record.request = request;
        record.output = output;
        record.estimated_direct_cost = std::max<std::uint64_t>(
            1, request.estimated_direct_cost);
        record.last_used = ++use_clock_;
        record.byte_cost = estimate_record_bytes(record);

        LearnResult result = admit_locked(std::move(record));
        if (result.admitted) {
            ++statistics_.learned;
        }
        return result;
    }

    Resolution resolve(const Request& request) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++statistics_.lookups;

        std::string reason;
        if (!validate_request(request, &reason)) {
            ++statistics_.rejected;
            return {Outcome::kRejected, std::nullopt, {}, reason};
        }

        const std::string key = request_key(request);
        auto exact = records_.find(key);
        if (exact != records_.end()) {
            touch_locked(exact->second);
            ++statistics_.exact_hits;
            return {Outcome::kExactHit, exact->second.output,
                    {exact->second.id}, "exact request and validity context match"};
        }

        std::optional<Resolution> composed;
        switch (rule_for(request.operation)) {
            case RuleKind::kCommutativeSumI64:
                composed = compose_sum_locked(request);
                break;
            case RuleKind::kOrderedConcatUtf8:
                composed = compose_concat_locked(request);
                break;
            case RuleKind::kExactOnly:
                break;
        }

        if (composed.has_value()) {
            ++statistics_.relationship_hits;

            // A derived result becomes an exact record if memory policy allows.
            // The answer remains valid even when admission is declined.
            Record record;
            record.key = key;
            record.request = request;
            record.output = *composed->value;
            record.estimated_direct_cost = std::max<std::uint64_t>(
                1, request.estimated_direct_cost);
            record.last_used = ++use_clock_;
            record.derived = true;
            record.byte_cost = estimate_record_bytes(record);
            if (admit_locked(std::move(record)).admitted) {
                ++statistics_.derived_records;
            }
            return *composed;
        }

        ++statistics_.misses;
        return {Outcome::kMiss, std::nullopt, {},
                "no valid exact record or composable relationship"};
    }

    std::size_t invalidate_context(const std::string& context) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t removed = 0;
        for (auto it = records_.begin(); it != records_.end();) {
            if (it->second.request.context == context) {
                bytes_used_ -= it->second.byte_cost;
                it = records_.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        statistics_.invalidations += removed;
        return removed;
    }

    std::size_t invalidate_dependency(const std::string& dependency_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t removed = 0;
        for (auto it = records_.begin(); it != records_.end();) {
            const bool depends_on_id = std::any_of(
                it->second.request.inputs.begin(), it->second.request.inputs.end(),
                [&dependency_id](const Input& input) {
                    return input.id == dependency_id;
                });
            if (depends_on_id) {
                bytes_used_ -= it->second.byte_cost;
                it = records_.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        statistics_.invalidations += removed;
        return removed;
    }

    Statistics statistics() const {
        std::lock_guard<std::mutex> lock(mutex_);
        Statistics snapshot = statistics_;
        snapshot.record_count = records_.size();
        snapshot.bytes_used = bytes_used_;
        snapshot.byte_budget = byte_budget_;
        return snapshot;
    }

private:
    struct Candidate {
        std::uint64_t record_id = 0;
        Value output;
        std::vector<std::size_t> indexes;
        std::uint64_t estimated_direct_cost = 1;
    };

    struct Segment {
        std::uint64_t record_id = 0;
        Value output;
        std::size_t start = 0;
        std::size_t length = 0;
        std::uint64_t estimated_direct_cost = 1;
    };

    void touch_locked(Record& record) {
        ++record.hits;
        record.last_used = ++use_clock_;
    }

    std::optional<Candidate> best_sum_candidate_locked(
        const Request& request, const std::vector<bool>& used) {
        std::optional<Candidate> best;

        for (auto& item : records_) {
            Record& record = item.second;
            if (record.request.operation != request.operation ||
                record.request.context != request.context ||
                record.request.semantic_version != request.semantic_version ||
                record.output.kind != ValueKind::kInt64 ||
                record.request.inputs.empty()) {
                continue;
            }

            std::vector<bool> locally_used = used;
            std::vector<std::size_t> matches;
            matches.reserve(record.request.inputs.size());
            bool all_match = true;
            for (const Input& source : record.request.inputs) {
                bool found = false;
                for (std::size_t i = 0; i < request.inputs.size(); ++i) {
                    if (!locally_used[i] && same_input(source, request.inputs[i])) {
                        locally_used[i] = true;
                        matches.push_back(i);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    all_match = false;
                    break;
                }
            }
            if (!all_match) {
                continue;
            }

            if (!best.has_value() ||
                matches.size() > best->indexes.size() ||
                (matches.size() == best->indexes.size() &&
                 record.estimated_direct_cost > best->estimated_direct_cost)) {
                best = Candidate{record.id, record.output, std::move(matches),
                                 record.estimated_direct_cost};
            }
        }
        return best;
    }

    std::optional<Resolution> compose_sum_locked(const Request& request) {
        std::vector<bool> used(request.inputs.size(), false);
        std::vector<Candidate> selected;

        for (;;) {
            std::optional<Candidate> candidate =
                best_sum_candidate_locked(request, used);
            if (!candidate.has_value()) {
                break;
            }
            for (const std::size_t index : candidate->indexes) {
                used[index] = true;
            }
            selected.push_back(std::move(*candidate));
        }

        if (selected.size() < 2 ||
            std::find(used.begin(), used.end(), false) != used.end()) {
            return std::nullopt;
        }

        std::int64_t total = 0;
        std::vector<std::uint64_t> proof;
        proof.reserve(selected.size());
        for (const Candidate& candidate : selected) {
            if (!checked_add(total, candidate.output.integer, &total)) {
                return std::nullopt;
            }
            proof.push_back(candidate.record_id);
            for (auto& item : records_) {
                if (item.second.id == candidate.record_id) {
                    touch_locked(item.second);
                    break;
                }
            }
        }

        std::string reason;
        const std::optional<Value> expected = evaluate_known_schema(request, &reason);
        if (!expected.has_value() || expected->kind != ValueKind::kInt64 ||
            expected->integer != total) {
            return std::nullopt;
        }
        return Resolution{Outcome::kRelationshipHit, Value::int64(total),
                          std::move(proof),
                          "composed validated sum_i64 records"};
    }

    std::optional<Segment> best_concat_segment_locked(
        const Request& request, const std::vector<bool>& used) {
        std::optional<Segment> best;

        for (auto& item : records_) {
            Record& record = item.second;
            if (record.request.operation != request.operation ||
                record.request.context != request.context ||
                record.request.semantic_version != request.semantic_version ||
                record.output.kind != ValueKind::kUtf8 ||
                record.request.inputs.empty() ||
                record.request.inputs.size() > request.inputs.size()) {
                continue;
            }

            const std::size_t length = record.request.inputs.size();
            for (std::size_t start = 0; start + length <= request.inputs.size(); ++start) {
                bool matches = true;
                for (std::size_t offset = 0; offset < length; ++offset) {
                    if (used[start + offset] ||
                        !same_input(record.request.inputs[offset],
                                    request.inputs[start + offset])) {
                        matches = false;
                        break;
                    }
                }
                if (!matches) {
                    continue;
                }

                if (!best.has_value() || length > best->length ||
                    (length == best->length &&
                     record.estimated_direct_cost > best->estimated_direct_cost)) {
                    best = Segment{record.id, record.output, start, length,
                                   record.estimated_direct_cost};
                }
            }
        }
        return best;
    }

    std::optional<Resolution> compose_concat_locked(const Request& request) {
        std::vector<bool> used(request.inputs.size(), false);
        std::vector<Segment> selected;

        for (;;) {
            std::optional<Segment> segment =
                best_concat_segment_locked(request, used);
            if (!segment.has_value()) {
                break;
            }
            for (std::size_t offset = 0; offset < segment->length; ++offset) {
                used[segment->start + offset] = true;
            }
            selected.push_back(std::move(*segment));
        }

        if (selected.size() < 2 ||
            std::find(used.begin(), used.end(), false) != used.end()) {
            return std::nullopt;
        }

        std::sort(selected.begin(), selected.end(), [](const Segment& left,
                                                        const Segment& right) {
            return left.start < right.start;
        });

        std::string total;
        std::vector<std::uint64_t> proof;
        proof.reserve(selected.size());
        for (const Segment& segment : selected) {
            if (total.size() + segment.output.text.size() > kMaxMessageBytes) {
                return std::nullopt;
            }
            total += segment.output.text;
            proof.push_back(segment.record_id);
            for (auto& item : records_) {
                if (item.second.id == segment.record_id) {
                    touch_locked(item.second);
                    break;
                }
            }
        }

        std::string reason;
        const std::optional<Value> expected = evaluate_known_schema(request, &reason);
        if (!expected.has_value() || expected->kind != ValueKind::kUtf8 ||
            expected->text != total) {
            return std::nullopt;
        }
        return Resolution{Outcome::kRelationshipHit, Value::utf8(std::move(total)),
                          std::move(proof),
                          "composed ordered concat_utf8 records"};
    }

    std::optional<std::string> lowest_value_key_locked(
        const std::string& excluded_key) const {
        std::optional<std::string> selected;
        long double selected_score = 0.0L;
        std::uint64_t selected_last_used = 0;

        for (const auto& item : records_) {
            const Record& record = item.second;
            if (record.key == excluded_key) {
                continue;
            }
            const long double score =
                static_cast<long double>(record.hits + 1U) *
                static_cast<long double>(record.estimated_direct_cost + 1U) /
                static_cast<long double>(record.byte_cost + 1U);
            if (!selected.has_value() || score < selected_score ||
                (score == selected_score && record.last_used < selected_last_used)) {
                selected = record.key;
                selected_score = score;
                selected_last_used = record.last_used;
            }
        }
        return selected;
    }

    LearnResult admit_locked(Record record) {
        if (record.byte_cost > byte_budget_) {
            return {false, 0, "record exceeds the configured RAM budget"};
        }

        auto existing = records_.find(record.key);
        const std::size_t old_bytes =
            existing == records_.end() ? 0U : existing->second.byte_cost;
        std::size_t projected = bytes_used_ - old_bytes + record.byte_cost;

        while (projected > byte_budget_) {
            const std::optional<std::string> victim =
                lowest_value_key_locked(record.key);
            if (!victim.has_value()) {
                return {false, 0, "no evictable record can satisfy the RAM budget"};
            }
            auto it = records_.find(*victim);
            bytes_used_ -= it->second.byte_cost;
            records_.erase(it);
            ++statistics_.evictions;
            projected = bytes_used_ - old_bytes + record.byte_cost;
        }

        if (existing != records_.end()) {
            record.id = existing->second.id;
            bytes_used_ -= existing->second.byte_cost;
            records_.erase(existing);
        } else {
            record.id = next_record_id_++;
        }
        const std::uint64_t id = record.id;
        bytes_used_ += record.byte_cost;
        // Keep the map key separate from the moved Record. This avoids relying
        // on argument-evaluation order when inserting the record.
        const std::string map_key = record.key;
        records_.emplace(map_key, std::move(record));
        return {true, id, "record admitted"};
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Record> records_;
    Statistics statistics_;
    std::size_t byte_budget_ = 0;
    std::size_t bytes_used_ = 0;
    std::uint64_t next_record_id_ = 1;
    std::uint64_t use_clock_ = 0;
};

class ByteWriter {
public:
    void u8(std::uint8_t value) { data_.push_back(value); }

    void u32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            data_.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void u64(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            data_.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void i64(std::int64_t value) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u64(bits);
    }

    void string(const std::string& value) {
        u32(static_cast<std::uint32_t>(value.size()));
        data_.insert(data_.end(), value.begin(), value.end());
    }

    void value(const Value& value) {
        u8(static_cast<std::uint8_t>(value.kind));
        if (value.kind == ValueKind::kInt64) {
            i64(value.integer);
        } else {
            string(value.text);
        }
    }

    void input(const Input& input) {
        string(input.id);
        u64(input.version);
        value(input.value);
    }

    void request(const Request& request) {
        string(request.operation);
        string(request.context);
        u64(request.semantic_version);
        u64(request.estimated_direct_cost);
        u32(static_cast<std::uint32_t>(request.inputs.size()));
        for (const Input& input : request.inputs) {
            this->input(input);
        }
    }

    void append(const std::vector<std::uint8_t>& bytes) {
        data_.insert(data_.end(), bytes.begin(), bytes.end());
    }

    const std::vector<std::uint8_t>& data() const { return data_; }

private:
    std::vector<std::uint8_t> data_;
};

class ByteReader {
public:
    explicit ByteReader(const std::vector<std::uint8_t>& data) : data_(data) {}

    bool u8(std::uint8_t* value) {
        if (remaining() < 1) {
            return false;
        }
        *value = data_[position_++];
        return true;
    }

    bool u32(std::uint32_t* value) {
        if (remaining() < 4) {
            return false;
        }
        std::uint32_t result = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            result |= static_cast<std::uint32_t>(data_[position_++]) << shift;
        }
        *value = result;
        return true;
    }

    bool u64(std::uint64_t* value) {
        if (remaining() < 8) {
            return false;
        }
        std::uint64_t result = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            result |= static_cast<std::uint64_t>(data_[position_++]) << shift;
        }
        *value = result;
        return true;
    }

    bool i64(std::int64_t* value) {
        std::uint64_t bits = 0;
        if (!u64(&bits)) {
            return false;
        }
        std::memcpy(value, &bits, sizeof(bits));
        return true;
    }

    bool string(std::string* value) {
        std::uint32_t length = 0;
        if (!u32(&length) || length > kMaxStringBytes || remaining() < length) {
            return false;
        }
        value->assign(reinterpret_cast<const char*>(data_.data() + position_),
                      length);
        position_ += length;
        return true;
    }

    bool value(Value* value) {
        std::uint8_t kind = 0;
        if (!u8(&kind)) {
            return false;
        }
        if (kind == static_cast<std::uint8_t>(ValueKind::kInt64)) {
            value->kind = ValueKind::kInt64;
            value->text.clear();
            return i64(&value->integer);
        }
        if (kind == static_cast<std::uint8_t>(ValueKind::kUtf8)) {
            value->kind = ValueKind::kUtf8;
            value->integer = 0;
            return string(&value->text);
        }
        return false;
    }

    bool input(Input* input) {
        return string(&input->id) && u64(&input->version) && value(&input->value);
    }

    bool request(Request* request) {
        std::uint32_t count = 0;
        if (!string(&request->operation) || !string(&request->context) ||
            !u64(&request->semantic_version) ||
            !u64(&request->estimated_direct_cost) || !u32(&count) ||
            count > kMaxInputs) {
            return false;
        }
        request->inputs.clear();
        request->inputs.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            Input input;
            if (!this->input(&input)) {
                return false;
            }
            request->inputs.push_back(std::move(input));
        }
        return true;
    }

    bool finished() const { return position_ == data_.size(); }

private:
    std::size_t remaining() const { return data_.size() - position_; }

    const std::vector<std::uint8_t>& data_;
    std::size_t position_ = 0;
};

bool read_full(int fd, void* buffer, std::size_t bytes) {
    auto* cursor = static_cast<unsigned char*>(buffer);
    while (bytes != 0) {
        const ssize_t received = ::recv(fd, cursor, bytes, 0);
        if (received == 0) {
            return false;
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        cursor += received;
        bytes -= static_cast<std::size_t>(received);
    }
    return true;
}

bool write_full(int fd, const void* buffer, std::size_t bytes) {
    const auto* cursor = static_cast<const unsigned char*>(buffer);
    while (bytes != 0) {
        const ssize_t sent = ::send(fd, cursor, bytes, MSG_NOSIGNAL);
        if (sent == 0) {
            return false;
        }
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        cursor += sent;
        bytes -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool read_frame(int fd, std::vector<std::uint8_t>* frame) {
    std::array<std::uint8_t, 4> header{};
    if (!read_full(fd, header.data(), header.size())) {
        return false;
    }
    const std::uint32_t size =
        static_cast<std::uint32_t>(header[0]) |
        (static_cast<std::uint32_t>(header[1]) << 8U) |
        (static_cast<std::uint32_t>(header[2]) << 16U) |
        (static_cast<std::uint32_t>(header[3]) << 24U);
    if (size == 0 || size > kMaxMessageBytes) {
        return false;
    }
    frame->resize(size);
    return read_full(fd, frame->data(), frame->size());
}

bool write_frame(int fd, const std::vector<std::uint8_t>& frame) {
    if (frame.empty() || frame.size() > kMaxMessageBytes) {
        return false;
    }
    const std::uint32_t size = static_cast<std::uint32_t>(frame.size());
    const std::array<std::uint8_t, 4> header{{
        static_cast<std::uint8_t>(size),
        static_cast<std::uint8_t>(size >> 8U),
        static_cast<std::uint8_t>(size >> 16U),
        static_cast<std::uint8_t>(size >> 24U),
    }};
    return write_full(fd, header.data(), header.size()) &&
           write_full(fd, frame.data(), frame.size());
}

enum class Command : std::uint8_t {
    kLearn = 1,
    kResolve = 2,
    kInvalidateContext = 3,
    kInvalidateDependency = 4,
    kStats = 5,
    kPing = 6,
};

std::vector<std::uint8_t> error_reply(const std::string& error) {
    ByteWriter writer;
    writer.u32(kProtocolVersion);
    writer.u8(1);
    writer.string(error);
    return writer.data();
}

ByteWriter success_header() {
    ByteWriter writer;
    writer.u32(kProtocolVersion);
    writer.u8(0);
    return writer;
}

void write_statistics(ByteWriter* writer, const Statistics& stats) {
    writer->u64(stats.lookups);
    writer->u64(stats.exact_hits);
    writer->u64(stats.relationship_hits);
    writer->u64(stats.misses);
    writer->u64(stats.rejected);
    writer->u64(stats.learned);
    writer->u64(stats.derived_records);
    writer->u64(stats.evictions);
    writer->u64(stats.invalidations);
    writer->u64(stats.record_count);
    writer->u64(stats.bytes_used);
    writer->u64(stats.byte_budget);
}

bool read_statistics(ByteReader* reader, Statistics* stats) {
    return reader->u64(&stats->lookups) && reader->u64(&stats->exact_hits) &&
           reader->u64(&stats->relationship_hits) && reader->u64(&stats->misses) &&
           reader->u64(&stats->rejected) && reader->u64(&stats->learned) &&
           reader->u64(&stats->derived_records) && reader->u64(&stats->evictions) &&
           reader->u64(&stats->invalidations) && reader->u64(&stats->record_count) &&
           reader->u64(&stats->bytes_used) && reader->u64(&stats->byte_budget);
}

bool handle_message(const std::vector<std::uint8_t>& message,
                    RelationshipMemory* memory,
                    std::vector<std::uint8_t>* reply) {
    ByteReader reader(message);
    std::uint32_t version = 0;
    std::uint8_t raw_command = 0;
    if (!reader.u32(&version) || !reader.u8(&raw_command)) {
        *reply = error_reply("malformed protocol header");
        return true;
    }
    if (version != kProtocolVersion) {
        *reply = error_reply("unsupported protocol version");
        return true;
    }

    const Command command = static_cast<Command>(raw_command);
    switch (command) {
        case Command::kLearn: {
            Request request;
            Value output;
            std::uint8_t client_verified = 0;
            if (!reader.request(&request) || !reader.value(&output) ||
                !reader.u8(&client_verified) || !reader.finished()) {
                *reply = error_reply("malformed LEARN request");
                return true;
            }
            const LearnResult result = memory->learn(request, output,
                                                     client_verified != 0);
            ByteWriter writer = success_header();
            writer.u8(result.admitted ? 1 : 0);
            writer.u64(result.record_id);
            writer.string(result.reason);
            *reply = writer.data();
            return true;
        }

        case Command::kResolve: {
            Request request;
            if (!reader.request(&request) || !reader.finished()) {
                *reply = error_reply("malformed RESOLVE request");
                return true;
            }
            const Resolution result = memory->resolve(request);
            ByteWriter writer = success_header();
            writer.u8(static_cast<std::uint8_t>(result.outcome));
            writer.u8(result.value.has_value() ? 1 : 0);
            if (result.value.has_value()) {
                writer.value(*result.value);
            }
            writer.u32(static_cast<std::uint32_t>(result.proof_record_ids.size()));
            for (const std::uint64_t id : result.proof_record_ids) {
                writer.u64(id);
            }
            writer.string(result.reason);
            *reply = writer.data();
            return true;
        }

        case Command::kInvalidateContext:
        case Command::kInvalidateDependency: {
            std::string key;
            if (!reader.string(&key) || !reader.finished()) {
                *reply = error_reply("malformed invalidation request");
                return true;
            }
            const std::size_t count = command == Command::kInvalidateContext
                ? memory->invalidate_context(key)
                : memory->invalidate_dependency(key);
            ByteWriter writer = success_header();
            writer.u64(static_cast<std::uint64_t>(count));
            *reply = writer.data();
            return true;
        }

        case Command::kStats: {
            if (!reader.finished()) {
                *reply = error_reply("malformed STATS request");
                return true;
            }
            ByteWriter writer = success_header();
            write_statistics(&writer, memory->statistics());
            *reply = writer.data();
            return true;
        }

        case Command::kPing: {
            if (!reader.finished()) {
                *reply = error_reply("malformed PING request");
                return true;
            }
            ByteWriter writer = success_header();
            writer.string("DACA relationship runtime ready");
            *reply = writer.data();
            return true;
        }
    }
    *reply = error_reply("unknown command");
    return true;
}

class RpcClient {
public:
    ~RpcClient() { close(); }

    bool connect_to(const std::string& socket_path, std::string* error) {
        close();
        if (socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
            *error = "socket path is too long";
            return false;
        }
        const int candidate = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (candidate < 0) {
            *error = std::strerror(errno);
            return false;
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, socket_path.c_str(),
                     sizeof(address.sun_path) - 1);
        if (::connect(candidate, reinterpret_cast<const sockaddr*>(&address),
                      sizeof(address)) < 0) {
            *error = std::strerror(errno);
            ::close(candidate);
            return false;
        }
        fd_ = candidate;
        return true;
    }

    bool ping(std::string* message, std::string* error) {
        ByteWriter writer;
        writer.u32(kProtocolVersion);
        writer.u8(static_cast<std::uint8_t>(Command::kPing));
        ByteReader reply = exchange(writer.data(), error);
        return reply.string(message) && reply.finished();
    }

    bool learn(const Request& request, const Value& output, bool client_verified,
               LearnResult* result, std::string* error) {
        ByteWriter writer;
        writer.u32(kProtocolVersion);
        writer.u8(static_cast<std::uint8_t>(Command::kLearn));
        writer.request(request);
        writer.value(output);
        writer.u8(client_verified ? 1 : 0);
        ByteReader reply = exchange(writer.data(), error);
        std::uint8_t admitted = 0;
        if (!reply.u8(&admitted) || !reply.u64(&result->record_id) ||
            !reply.string(&result->reason) || !reply.finished()) {
            *error = "malformed LEARN reply";
            return false;
        }
        result->admitted = admitted != 0;
        return true;
    }

    bool resolve(const Request& request, Resolution* result, std::string* error) {
        ByteWriter writer;
        writer.u32(kProtocolVersion);
        writer.u8(static_cast<std::uint8_t>(Command::kResolve));
        writer.request(request);
        ByteReader reply = exchange(writer.data(), error);
        std::uint8_t outcome = 0;
        std::uint8_t has_value = 0;
        std::uint32_t proof_count = 0;
        if (!reply.u8(&outcome) || !reply.u8(&has_value)) {
            *error = "malformed RESOLVE reply";
            return false;
        }
        result->outcome = static_cast<Outcome>(outcome);
        result->value.reset();
        if (has_value != 0) {
            Value value;
            if (!reply.value(&value)) {
                *error = "malformed RESOLVE value";
                return false;
            }
            result->value = std::move(value);
        }
        if (!reply.u32(&proof_count) || proof_count > kMaxInputs) {
            *error = "malformed RESOLVE proof";
            return false;
        }
        result->proof_record_ids.clear();
        result->proof_record_ids.reserve(proof_count);
        for (std::uint32_t i = 0; i < proof_count; ++i) {
            std::uint64_t id = 0;
            if (!reply.u64(&id)) {
                *error = "truncated RESOLVE proof";
                return false;
            }
            result->proof_record_ids.push_back(id);
        }
        if (!reply.string(&result->reason) || !reply.finished()) {
            *error = "malformed RESOLVE metadata";
            return false;
        }
        return true;
    }

    bool invalidate_context(const std::string& context, std::size_t* count,
                            std::string* error) {
        return invalidate(Command::kInvalidateContext, context, count, error);
    }

    bool invalidate_dependency(const std::string& id, std::size_t* count,
                              std::string* error) {
        return invalidate(Command::kInvalidateDependency, id, count, error);
    }

    bool statistics(Statistics* stats, std::string* error) {
        ByteWriter writer;
        writer.u32(kProtocolVersion);
        writer.u8(static_cast<std::uint8_t>(Command::kStats));
        ByteReader reply = exchange(writer.data(), error);
        if (!read_statistics(&reply, stats) || !reply.finished()) {
            *error = "malformed STATS reply";
            return false;
        }
        return true;
    }

private:
    ByteReader exchange(const std::vector<std::uint8_t>& request,
                        std::string* error) {
        static const std::vector<std::uint8_t> empty;
        if (fd_ < 0) {
            *error = "client is not connected";
            return ByteReader(empty);
        }
        if (!write_frame(fd_, request)) {
            *error = "failed to send request: " + std::string(std::strerror(errno));
            return ByteReader(empty);
        }
        std::vector<std::uint8_t> reply_bytes;
        if (!read_frame(fd_, &reply_bytes)) {
            *error = "failed to receive reply: " + std::string(std::strerror(errno));
            return ByteReader(empty);
        }

        // Keep response storage alive until the reader is consumed by the
        // immediate caller. Every public client method consumes it before return.
        reply_storage_ = std::move(reply_bytes);
        ByteReader reply(reply_storage_);
        std::uint32_t version = 0;
        std::uint8_t status = 0;
        if (!reply.u32(&version) || !reply.u8(&status) || version != kProtocolVersion) {
            *error = "malformed protocol reply";
            return ByteReader(empty);
        }
        if (status != 0) {
            std::string daemon_error;
            if (!reply.string(&daemon_error)) {
                daemon_error = "unknown daemon error";
            }
            *error = daemon_error;
            return ByteReader(empty);
        }

        // Return a reader starting immediately after the response header.
        response_payload_.assign(reply_storage_.begin() + 5, reply_storage_.end());
        return ByteReader(response_payload_);
    }

    bool invalidate(Command command, const std::string& key, std::size_t* count,
                    std::string* error) {
        ByteWriter writer;
        writer.u32(kProtocolVersion);
        writer.u8(static_cast<std::uint8_t>(command));
        writer.string(key);
        ByteReader reply = exchange(writer.data(), error);
        std::uint64_t raw_count = 0;
        if (!reply.u64(&raw_count) || !reply.finished()) {
            *error = "malformed invalidation reply";
            return false;
        }
        *count = static_cast<std::size_t>(raw_count);
        return true;
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int fd_ = -1;
    std::vector<std::uint8_t> reply_storage_;
    std::vector<std::uint8_t> response_payload_;
};

volatile sig_atomic_t g_running = 1;

void stop_daemon(int) {
    g_running = 0;
}

void serve_client(int client_fd, RelationshipMemory* memory) {
    for (;;) {
        std::vector<std::uint8_t> message;
        if (!read_frame(client_fd, &message)) {
            break;
        }
        std::vector<std::uint8_t> reply;
        try {
            handle_message(message, memory, &reply);
        } catch (const std::exception& error) {
            reply = error_reply(std::string("daemon exception: ") + error.what());
        } catch (...) {
            reply = error_reply("daemon exception: unknown error");
        }
        if (!write_frame(client_fd, reply)) {
            break;
        }
    }
    ::close(client_fd);
}

std::string default_socket_path() {
    if (const char* explicit_path = std::getenv("DACA_SOCKET")) {
        if (*explicit_path != '\0') {
            return explicit_path;
        }
    }
    if (const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR")) {
        if (*runtime_dir != '\0') {
            return std::string(runtime_dir) + "/daca-relationship.sock";
        }
    }
    return "/tmp/daca-relationship-" + std::to_string(::getuid()) + ".sock";
}

bool remove_stale_socket(const std::string& path, std::string* error) {
    struct stat info {};
    if (::lstat(path.c_str(), &info) == 0) {
        if (!S_ISSOCK(info.st_mode) || info.st_uid != ::getuid()) {
            *error = "refusing to replace a path that is not this user's socket";
            return false;
        }
        if (::unlink(path.c_str()) < 0) {
            *error = std::strerror(errno);
            return false;
        }
        return true;
    }
    if (errno != ENOENT) {
        *error = std::strerror(errno);
        return false;
    }
    return true;
}

int create_listening_socket(const std::string& path, std::string* error) {
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        *error = "socket path is too long";
        return -1;
    }
    if (!remove_stale_socket(path, error)) {
        return -1;
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *error = std::strerror(errno);
        return -1;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        *error = std::strerror(errno);
        ::close(fd);
        return -1;
    }
    if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) < 0) {
        *error = std::strerror(errno);
        ::close(fd);
        ::unlink(path.c_str());
        return -1;
    }
    if (::listen(fd, 64) < 0) {
        *error = std::strerror(errno);
        ::close(fd);
        ::unlink(path.c_str());
        return -1;
    }
    return fd;
}

void print_statistics(const Statistics& stats) {
    std::cout << "DACA relationship-memory statistics\n"
              << "  Lookups:             " << stats.lookups << "\n"
              << "  Exact hits:          " << stats.exact_hits << "\n"
              << "  Relationship hits:   " << stats.relationship_hits << "\n"
              << "  Misses:              " << stats.misses << "\n"
              << "  Rejected:            " << stats.rejected << "\n"
              << "  Learned records:     " << stats.learned << "\n"
              << "  Derived records:     " << stats.derived_records << "\n"
              << "  Evictions:           " << stats.evictions << "\n"
              << "  Invalidations:       " << stats.invalidations << "\n"
              << "  Records in RAM:      " << stats.record_count << "\n"
              << "  Accounted RAM:       " << stats.bytes_used << " / "
              << stats.byte_budget << " bytes\n";
}

int run_daemon(const std::string& socket_path, std::size_t memory_budget) {
    struct sigaction action {};
    action.sa_handler = stop_daemon;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    ::sigaction(SIGINT, &action, nullptr);
    ::sigaction(SIGTERM, &action, nullptr);
    ::signal(SIGPIPE, SIG_IGN);

    std::string error;
    const int listen_fd = create_listening_socket(socket_path, &error);
    if (listen_fd < 0) {
        std::cerr << "Cannot start DACA daemon: " << error << "\n";
        return 1;
    }

    RelationshipMemory memory(memory_budget);
    std::cout << "DACA relationship daemon ready\n"
              << "  Socket: " << socket_path << "\n"
              << "  RAM budget: " << memory_budget << " bytes\n"
              << "  Built-in compositional schemas: sum_i64, concat_utf8\n"
              << std::flush;

    while (g_running != 0) {
        pollfd descriptor{};
        descriptor.fd = listen_fd;
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1, 250);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "poll failed: " << std::strerror(errno) << "\n";
            break;
        }
        if (ready == 0 || (descriptor.revents & POLLIN) == 0) {
            continue;
        }
        const int client_fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "accept failed: " << std::strerror(errno) << "\n";
            break;
        }
        std::thread(serve_client, client_fd, &memory).detach();
    }

    ::close(listen_fd);
    ::unlink(socket_path.c_str());
    print_statistics(memory.statistics());
    return 0;
}

Input int_input(std::string id, std::uint64_t version, std::int64_t value) {
    return {std::move(id), version, Value::int64(value)};
}

Input text_input(std::string id, std::uint64_t version, std::string value) {
    return {std::move(id), version, Value::utf8(std::move(value))};
}

Request make_request(std::string operation, std::string context,
                     std::uint64_t semantic_version,
                     std::uint64_t estimated_direct_cost,
                     std::vector<Input> inputs) {
    Request request;
    request.operation = std::move(operation);
    request.context = std::move(context);
    request.semantic_version = semantic_version;
    request.estimated_direct_cost = estimated_direct_cost;
    request.inputs = std::move(inputs);
    return request;
}

void print_resolution(const std::string& label, const Resolution& resolution) {
    std::cout << "  " << label << ": " << outcome_name(resolution.outcome);
    if (resolution.value.has_value()) {
        std::cout << " value=" << value_label(*resolution.value);
    }
    if (!resolution.proof_record_ids.empty()) {
        std::cout << " proof=";
        for (std::size_t i = 0; i < resolution.proof_record_ids.size(); ++i) {
            if (i != 0) {
                std::cout << ',';
            }
            std::cout << resolution.proof_record_ids[i];
        }
    }
    std::cout << "\n";
}

bool require_resolution(RpcClient* client, const Request& request,
                        Outcome expected_outcome,
                        const std::optional<Value>& expected_value,
                        const std::string& label) {
    std::string error;
    Resolution result;
    if (!client->resolve(request, &result, &error)) {
        std::cerr << "  " << label << " failed: " << error << "\n";
        return false;
    }
    print_resolution(label, result);
    if (result.outcome != expected_outcome ||
        (expected_value.has_value() &&
         (!result.value.has_value() || !(*result.value == *expected_value)))) {
        std::cerr << "  Expected " << outcome_name(expected_outcome) << "\n";
        return false;
    }
    return true;
}

bool learn_known(RpcClient* client, const Request& request, const std::string& label) {
    std::string validation_error;
    const std::optional<Value> result = evaluate_known_schema(request, &validation_error);
    if (!result.has_value()) {
        std::cerr << "  " << label << " direct fallback failed: "
                  << validation_error << "\n";
        return false;
    }
    LearnResult learned;
    std::string error;
    if (!client->learn(request, *result, true, &learned, &error)) {
        std::cerr << "  " << label << " learn failed: " << error << "\n";
        return false;
    }
    std::cout << "  " << label << ": "
              << (learned.admitted ? "ADMITTED" : "DECLINED")
              << " record=" << learned.record_id << " (" << learned.reason << ")\n";
    return learned.admitted;
}

int run_demo(const std::string& socket_path) {
    RpcClient client;
    std::string error;
    if (!client.connect_to(socket_path, &error)) {
        std::cerr << "Cannot connect to DACA daemon at " << socket_path
                  << ": " << error << "\n";
        return 1;
    }

    std::string greeting;
    if (!client.ping(&greeting, &error)) {
        std::cerr << "Daemon did not respond: " << error << "\n";
        return 1;
    }
    std::cout << greeting << "\n\n";

    bool passed = true;
    const std::string minecraft_context =
        "minecraft|game=demo-1|seed=42|dimension=overworld|mods=none|dtype=i64";
    const Input a = int_input("chunk:0,0:height", 1, 5);
    const Input b = int_input("chunk:0,1:height", 1, 6);
    const Input c = int_input("chunk:1,0:height", 1, 8);
    const Input d = int_input("chunk:1,1:height", 1, 9);

    const Request pair_ab = make_request(
        "sum_i64", minecraft_context, 1, 50000, {a, b});
    const Request pair_cd = make_request(
        "sum_i64", minecraft_context, 1, 50000, {c, d});
    // Inputs deliberately arrive in a different order. sum_i64 is declared
    // commutative; concat_utf8 below preserves order instead.
    const Request target_sum = make_request(
        "sum_i64", minecraft_context, 1, 200000, {d, a, c, b});

    std::cout << "1. Compositional sum relationship\n";
    passed &= require_resolution(&client, target_sum, Outcome::kMiss,
                                 std::nullopt, "cold target query");
    passed &= learn_known(&client, pair_ab, "learn A+B after CPU fallback");
    passed &= learn_known(&client, pair_cd, "learn C+D after CPU fallback");
    passed &= require_resolution(&client, target_sum, Outcome::kRelationshipHit,
                                 Value::int64(28),
                                 "compose A+B and C+D for D+A+C+B");
    passed &= require_resolution(&client, target_sum, Outcome::kExactHit,
                                 Value::int64(28),
                                 "derived target exact reuse");

    std::size_t invalidated = 0;
    if (!client.invalidate_dependency("chunk:0,0:height", &invalidated, &error)) {
        std::cerr << "  dependency invalidation failed: " << error << "\n";
        passed = false;
    } else {
        std::cout << "  dependency update invalidated " << invalidated
                  << " records\n";
    }
    passed &= require_resolution(&client, target_sum, Outcome::kMiss,
                                 std::nullopt,
                                 "stale dependency must not be reused");

    std::cout << "\n2. Ordered composition relationship\n";
    const std::string text_context =
        "minecraft|game=demo-1|resource-pack=vanilla|language=en_us|dtype=utf8";
    const Input grass = text_input("block:grass", 1, "grass");
    const Input block = text_input("block:block", 1, "block");
    const Input colon = text_input("separator", 1, ":");
    const Input state = text_input("property:state", 1, "state");
    const Request left = make_request("concat_utf8", text_context, 1, 25000,
                                      {grass, block});
    const Request right = make_request("concat_utf8", text_context, 1, 25000,
                                       {colon, state});
    const Request target_text = make_request("concat_utf8", text_context, 1,
                                             100000, {grass, block, colon, state});
    const Request reordered_text = make_request("concat_utf8", text_context, 1,
                                                100000, {block, grass, colon, state});
    passed &= learn_known(&client, left, "learn ordered segment grass+block");
    passed &= learn_known(&client, right, "learn ordered segment :+state");
    passed &= require_resolution(&client, target_text, Outcome::kRelationshipHit,
                                 Value::utf8("grassblock:state"),
                                 "compose ordered segments");
    passed &= require_resolution(&client, reordered_text, Outcome::kMiss,
                                 std::nullopt,
                                 "reject reordered concat relationship");

    std::cout << "\n3. Exact reuse for a caller-defined operation\n";
    const Request opaque = make_request(
        "minecraft_light_step/v1",
        "minecraft|game=demo-1|seed=42|chunk=0,0|lighting-epoch=7",
        1, 750000,
        {text_input("block-state-hash", 7, "a4f1b3")});
    LearnResult opaque_record;
    if (!client.learn(opaque, Value::utf8("light-output-fingerprint-91c2"), true,
                      &opaque_record, &error)) {
        std::cerr << "  opaque learn failed: " << error << "\n";
        passed = false;
    } else {
        std::cout << "  caller-defined exact record: "
                  << (opaque_record.admitted ? "ADMITTED" : "DECLINED")
                  << " record=" << opaque_record.record_id << "\n";
    }
    passed &= require_resolution(&client, opaque, Outcome::kExactHit,
                                 Value::utf8("light-output-fingerprint-91c2"),
                                 "exact opaque operation reuse");

    Statistics stats;
    if (!client.statistics(&stats, &error)) {
        std::cerr << "Could not retrieve statistics: " << error << "\n";
        passed = false;
    } else {
        std::cout << "\n";
        print_statistics(stats);
    }

    std::cout << "\nDemo correctness: " << (passed ? "PASS" : "FAIL") << "\n";
    return passed ? 0 : 1;
}

bool parse_memory_mib(const char* text, std::size_t* bytes) {
    errno = 0;
    char* end = nullptr;
    const unsigned long long mib = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || mib == 0 ||
        mib > std::numeric_limits<std::size_t>::max() / (1024U * 1024U)) {
        return false;
    }
    *bytes = static_cast<std::size_t>(mib) * 1024U * 1024U;
    return true;
}

void print_usage(const char* executable) {
    std::cout << "Usage:\n"
              << "  " << executable
              << " --daemon [--socket PATH] [--memory-mib N]\n"
              << "  " << executable << " --demo [--socket PATH]\n"
              << "  " << executable << " --help\n\n"
              << "The daemon stores its live cache in process RAM. DACA_SOCKET\n"
              << "overrides the default socket path.\n";
}

}  // namespace daca

int main(int argc, char** argv) {
    bool daemon = false;
    bool demo = false;
    std::string socket_path = daca::default_socket_path();
    std::size_t memory_budget = 128U * 1024U * 1024U;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--daemon") {
            daemon = true;
        } else if (argument == "--demo") {
            demo = true;
        } else if (argument == "--socket" && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (argument == "--memory-mib" && i + 1 < argc) {
            if (!daca::parse_memory_mib(argv[++i], &memory_budget)) {
                std::cerr << "Invalid --memory-mib value\n";
                return 2;
            }
        } else if (argument == "--help" || argument == "-h") {
            daca::print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown or incomplete argument: " << argument << "\n";
            daca::print_usage(argv[0]);
            return 2;
        }
    }

    if (daemon == demo) {
        daca::print_usage(argv[0]);
        return 2;
    }
    return daemon ? daca::run_daemon(socket_path, memory_budget)
                  : daca::run_demo(socket_path);
}
