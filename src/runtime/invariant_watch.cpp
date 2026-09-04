#include "didi/runtime/invariant_watch.hpp"

#include "didi/runtime/profiler_collector.hpp"

#include <algorithm>
#include <cmath>

namespace didi {
namespace runtime {

namespace {

constexpr int kMinDurationMs = 1;
constexpr int kMaxDurationMs = 30000;
constexpr size_t kMaxInvariants = 8;
constexpr size_t kMaxNameBytes = 64;
constexpr size_t kMaxExpressionBytes = 512;
constexpr size_t kMaxContextBytes = 256;

const char* kindName(InvariantKind kind) {
    switch (kind) {
        case InvariantKind::performance_between: return "performance_between";
        case InvariantKind::expression_between: return "expression_between";
        case InvariantKind::no_engine_errors: return "no_engine_errors";
    }
    return "unknown";
}

Result<double> boundValue(const json& value, const char* field) {
    if (!value.is_number()) {
        return Error::invalidArgument(std::string(field) + " must be a number");
    }
    const auto number = value.get<double>();
    if (!std::isfinite(number)) {
        return Error::invalidArgument(std::string(field) + " must be finite");
    }
    return number;
}

Result<std::string> boundedString(const json& value, const char* field, size_t maximum) {
    if (!value.is_string()) {
        return Error::invalidArgument(std::string(field) + " must be a string");
    }
    auto text = value.get<std::string>();
    if (text.empty() || text.size() > maximum) {
        return Error::invalidArgument(std::string(field) + " must be 1 to " +
                                      std::to_string(maximum) + " bytes");
    }
    return text;
}

Result<InvariantSpec> parseInvariant(const json& value, size_t position) {
    if (!value.is_object()) {
        return Error::invalidArgument("Each invariant must be an object");
    }
    InvariantSpec spec;
    if (value.contains("name")) {
        auto name = boundedString(value["name"], "name", kMaxNameBytes);
        if (name.isErr()) return name.error();
        spec.name = name.value();
    } else {
        spec.name = "invariant_" + std::to_string(position);
    }

    auto kind_text = boundedString(value.value("kind", json()), "kind", 64);
    if (kind_text.isErr()) return kind_text.error();
    const auto& kind = kind_text.value();
    if (kind == "performance_between") spec.kind = InvariantKind::performance_between;
    else if (kind == "expression_between") spec.kind = InvariantKind::expression_between;
    else if (kind == "no_engine_errors") spec.kind = InvariantKind::no_engine_errors;
    else {
        return Error::invalidArgument(
            "kind must be performance_between, expression_between, or no_engine_errors");
    }

    if (spec.kind == InvariantKind::performance_between) {
        auto metric = boundedString(value.value("metric", json()), "metric", 64);
        if (metric.isErr()) return metric.error();
        const auto found = std::find_if(
            kProfilerMetrics.begin(), kProfilerMetrics.end(),
            [&](const ProfilerMetricSpec& candidate) { return metric.value() == candidate.name; });
        if (found == kProfilerMetrics.end()) {
            return Error::invalidArgument("metric is not one of the readable Performance monitors: " +
                                          metric.value());
        }
        spec.metric_index = static_cast<size_t>(std::distance(kProfilerMetrics.begin(), found));
    } else if (spec.kind == InvariantKind::expression_between) {
        auto expression = boundedString(value.value("expression", json()), "expression",
                                        kMaxExpressionBytes);
        if (expression.isErr()) return expression.error();
        spec.expression = expression.value();
        if (value.contains("context_node")) {
            auto context = boundedString(value["context_node"], "context_node", kMaxContextBytes);
            if (context.isErr()) return context.error();
            spec.context_node = context.value();
        }
    }

    if (spec.kind != InvariantKind::no_engine_errors) {
        if (value.contains("minimum")) {
            auto minimum = boundValue(value["minimum"], "minimum");
            if (minimum.isErr()) return minimum.error();
            spec.minimum = minimum.value();
        }
        if (value.contains("maximum")) {
            auto maximum = boundValue(value["maximum"], "maximum");
            if (maximum.isErr()) return maximum.error();
            spec.maximum = maximum.value();
        }
        // A range with no bound is a condition nothing can break, and reporting
        // it as held would be a fact about nothing.
        if (!spec.minimum.has_value() && !spec.maximum.has_value()) {
            return Error::invalidArgument(
                "an invariant of kind " + std::string(kindName(spec.kind)) +
                " needs a minimum, a maximum, or both; without one it cannot be violated");
        }
        if (spec.minimum.has_value() && spec.maximum.has_value() &&
            *spec.minimum > *spec.maximum) {
            return Error::invalidArgument("minimum must not be greater than maximum");
        }
    }
    return spec;
}

} // namespace

Result<InvariantWatchRequest> parseInvariantWatchRequest(const json& params) {
    if (!params.is_object()) {
        return Error::invalidArgument("Invariant watch params must be an object");
    }
    InvariantWatchRequest request;
    if (params.contains("duration_ms")) {
        const auto& value = params["duration_ms"];
        if (!value.is_number_integer() && !value.is_number_unsigned()) {
            return Error::invalidArgument("duration_ms must be an integer from 1 to 30000");
        }
        const auto duration = value.get<int64_t>();
        if (duration < kMinDurationMs || duration > kMaxDurationMs) {
            return Error::invalidArgument("duration_ms must be an integer from 1 to 30000");
        }
        request.duration_ms = static_cast<int>(duration);
    }
    if (params.contains("pause_on_violation")) {
        if (!params["pause_on_violation"].is_boolean()) {
            return Error::invalidArgument("pause_on_violation must be a boolean");
        }
        request.pause_on_violation = params["pause_on_violation"].get<bool>();
    }
    if (!params.contains("invariants") || !params["invariants"].is_array()) {
        return Error::invalidArgument("invariants must be an array");
    }
    const auto& invariants = params["invariants"];
    if (invariants.empty() || invariants.size() > kMaxInvariants) {
        return Error::invalidArgument("invariants must contain 1 to 8 entries");
    }
    for (size_t index = 0; index < invariants.size(); ++index) {
        auto spec = parseInvariant(invariants[index], index);
        if (spec.isErr()) return spec.error();
        request.invariants.push_back(std::move(spec.value()));
    }
    return request;
}

InvariantWatch::InvariantWatch(InvariantWatchRequest request)
    : m_request(std::move(request)), m_observed(m_request.invariants.size()) {}

bool InvariantWatch::observe(int64_t elapsed_ms, const InvariantSample& sample) {
    if (m_violation.has_value()) return true;
    m_last_elapsed_ms = elapsed_ms;
    ++m_samples;

    for (size_t index = 0; index < m_request.invariants.size(); ++index) {
        const auto& spec = m_request.invariants[index];
        auto& observed = m_observed[index];

        if (spec.kind == InvariantKind::no_engine_errors) {
            // Reading zero errors is a reading. It is the only kind whose value
            // is always available, because the log ring is always there.
            const auto value = static_cast<double>(sample.engine_errors);
            observed.readings == 0 ? (observed.minimum = observed.maximum = value)
                                   : (observed.minimum = std::min(observed.minimum, value),
                                      observed.maximum = std::max(observed.maximum, value));
            observed.last = value;
            ++observed.readings;
            if (sample.engine_errors > 0) {
                m_violation = Violation{index, value, "maximum", 0.0, elapsed_ms, m_samples,
                                        sample.engine_errors};
                return true;
            }
            continue;
        }

        const auto& reading = index < sample.readings.size() ? sample.readings[index]
                                                             : InvariantReading{};
        if (!reading.value.has_value()) {
            // A value that could not be read is recorded as not read. Treating
            // it as within range would report an invariant as held on evidence
            // that never arrived.
            if (!reading.read_error.empty()) observed.last_read_error = reading.read_error;
            continue;
        }
        const auto value = *reading.value;
        observed.readings == 0 ? (observed.minimum = observed.maximum = value)
                               : (observed.minimum = std::min(observed.minimum, value),
                                  observed.maximum = std::max(observed.maximum, value));
        observed.last = value;
        ++observed.readings;

        if (spec.minimum.has_value() && value < *spec.minimum) {
            m_violation = Violation{index, value, "minimum", *spec.minimum, elapsed_ms, m_samples,
                                    sample.engine_errors};
            return true;
        }
        if (spec.maximum.has_value() && value > *spec.maximum) {
            m_violation = Violation{index, value, "maximum", *spec.maximum, elapsed_ms, m_samples,
                                    sample.engine_errors};
            return true;
        }
    }
    return elapsed(elapsed_ms);
}

json InvariantWatch::response(bool paused) const {
    json observed = json::array();
    bool every_invariant_observed = true;
    for (size_t index = 0; index < m_request.invariants.size(); ++index) {
        const auto& spec = m_request.invariants[index];
        const auto& seen = m_observed[index];
        if (seen.readings == 0) every_invariant_observed = false;
        json entry = {
            {"name", spec.name},
            {"kind", kindName(spec.kind)},
            {"readings", seen.readings}
        };
        if (seen.readings > 0) {
            entry["minimum_observed"] = seen.minimum;
            entry["maximum_observed"] = seen.maximum;
            entry["last_observed"] = seen.last;
        }
        if (!seen.last_read_error.empty()) entry["read_error"] = seen.last_read_error;
        observed.push_back(std::move(entry));
    }

    // Three outcomes, not two. An invariant that never produced a reading did
    // not hold; nothing is known about it, and saying "held" would be the same
    // false success an empty impact list would be.
    const char* outcome = m_violation.has_value()
                              ? "violated"
                              : (every_invariant_observed ? "held" : "inconclusive");

    json result = {
        {"outcome", outcome},
        {"duration_ms", m_request.duration_ms},
        {"elapsed_ms", m_last_elapsed_ms},
        {"samples", m_samples},
        {"invariants", std::move(observed)},
        {"paused", paused},
        {"limitations", json::array({
            "Sampling is once per engine frame for the requested window. A "
            "condition that is false between two frames is not observed.",
            "An invariant whose value could never be read is reported with zero "
            "readings and makes the outcome inconclusive, never held.",
            "no_engine_errors sees error-level engine output, which is what an "
            "unhandled script error looks like from outside the script. It is "
            "not a debugger and has no call stack."
        })}
    };

    if (m_violation.has_value()) {
        const auto& violation = *m_violation;
        const auto& spec = m_request.invariants[violation.index];
        json report = {
            {"name", spec.name},
            {"kind", kindName(spec.kind)},
            {"observed", violation.value},
            {"elapsed_ms", violation.elapsed_ms},
            {"sample", violation.sample}
        };
        if (spec.kind == InvariantKind::no_engine_errors) {
            report["engine_errors"] = violation.engine_errors;
        } else {
            report["bound"] = violation.bound;
            report["limit"] = violation.limit;
        }
        if (spec.kind == InvariantKind::performance_between) {
            report["metric"] = kProfilerMetrics[spec.metric_index].name;
        }
        if (spec.kind == InvariantKind::expression_between) {
            report["expression"] = spec.expression;
            if (!spec.context_node.empty()) report["context_node"] = spec.context_node;
        }
        result["violation"] = std::move(report);
    }
    return result;
}

} // namespace runtime
} // namespace didi
