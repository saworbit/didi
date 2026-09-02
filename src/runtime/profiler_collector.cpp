#include "didi/runtime/profiler_collector.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace didi {
namespace runtime {

namespace {

constexpr int kMaxDurationMs = 5000;
constexpr int kMaxSamples = 120;

bool integerInRange(const json& value, int64_t minimum, int64_t maximum) {
    if (value.is_number_integer()) {
        const auto number = value.get<int64_t>();
        return number >= minimum && number <= maximum;
    }
    if (value.is_number_unsigned()) {
        const auto number = value.get<uint64_t>();
        return number <= static_cast<uint64_t>(maximum) &&
               number >= static_cast<uint64_t>(minimum < 0 ? 0 : minimum);
    }
    return false;
}

} // namespace

Result<ProfilerRequest> parseProfilerRequest(const json& params) {
    if (!params.is_object()) {
        return Error::invalidArgument("Profiler params must be an object");
    }
    for (const auto& [key, value] : params.items()) {
        (void)value;
        if (key != "duration_ms" && key != "sample_count" && key != "categories") {
            return Error::invalidArgument("Unknown profiler parameter: " + key);
        }
    }

    ProfilerRequest request;
    if (params.contains("duration_ms")) {
        if (!integerInRange(params["duration_ms"], 0, kMaxDurationMs)) {
            return Error::invalidArgument("duration_ms must be an integer from 0 to 5000");
        }
        request.duration_ms = params["duration_ms"].get<int>();
    }
    if (params.contains("sample_count")) {
        if (!integerInRange(params["sample_count"], 1, kMaxSamples)) {
            return Error::invalidArgument("sample_count must be an integer from 1 to 120");
        }
        request.sample_count = params["sample_count"].get<int>();
    }
    if (request.duration_ms == 0 && request.sample_count != 1) {
        return Error::invalidArgument("duration_ms 0 requires sample_count 1");
    }

    std::set<std::string> categories;
    if (params.contains("categories")) {
        const auto& requested = params["categories"];
        if (!requested.is_array() || requested.empty() || requested.size() > 4) {
            return Error::invalidArgument("categories must be an array of 1 to 4 category names");
        }
        for (const auto& entry : requested) {
            if (!entry.is_string()) {
                return Error::invalidArgument("categories entries must be strings");
            }
            const auto& name = entry.get_ref<const std::string&>();
            if (name != "frame" && name != "process" && name != "physics" && name != "render") {
                return Error::invalidArgument("Unknown profiler category: " + name);
            }
            if (!categories.insert(name).second) {
                return Error::invalidArgument("Duplicate profiler category: " + name);
            }
        }
    } else {
        categories = {"frame", "process", "physics", "render"};
    }

    for (size_t index = 0; index < kProfilerMetrics.size(); ++index) {
        if (categories.count(kProfilerMetrics[index].category)) {
            request.metric_indices.push_back(index);
        }
    }
    return request;
}

ProfilerCollector::ProfilerCollector(ProfilerRequest request)
    : m_request(std::move(request)) {
    const int count = m_request.sample_count;
    m_offsets.reserve(static_cast<size_t>(count));
    if (count <= 1) {
        m_offsets.push_back(0);
    } else {
        for (int index = 0; index < count; ++index) {
            const double offset = static_cast<double>(index) *
                                  static_cast<double>(m_request.duration_ms) /
                                  static_cast<double>(count - 1);
            m_offsets.push_back(static_cast<int64_t>(std::llround(offset)));
        }
    }
    m_stats.resize(m_request.metric_indices.size());
}

std::vector<int64_t> ProfilerCollector::monitors() const {
    std::vector<int64_t> result;
    result.reserve(m_request.metric_indices.size());
    for (const auto index : m_request.metric_indices) {
        result.push_back(kProfilerMetrics[index].monitor);
    }
    return result;
}

int64_t ProfilerCollector::nextOffsetMs() const {
    if (complete()) return -1;
    return m_offsets[static_cast<size_t>(m_collected)];
}

bool ProfilerCollector::due(int64_t elapsed_ms) const {
    return !complete() && elapsed_ms >= nextOffsetMs();
}

bool ProfilerCollector::observe(int64_t elapsed_ms, const std::vector<double>& values) {
    if (complete()) return true;
    if (values.size() != m_stats.size()) return false;
    // Every offset already due is satisfied by this reading. A slow frame that
    // crosses several offsets records the same values for each rather than
    // stretching the window past the requested duration.
    while (!complete() && elapsed_ms >= m_offsets[static_cast<size_t>(m_collected)]) {
        for (size_t index = 0; index < values.size(); ++index) {
            auto& stats = m_stats[index];
            const double value = values[index];
            if (!std::isfinite(value)) {
                ++stats.invalid;
                continue;
            }
            if (stats.valid == 0) {
                stats.min = value;
                stats.max = value;
            } else {
                stats.min = std::min(stats.min, value);
                stats.max = std::max(stats.max, value);
            }
            stats.sum += value;
            stats.last = value;
            ++stats.valid;
        }
        ++m_collected;
        m_last_elapsed_ms = elapsed_ms;
    }
    return complete();
}

json ProfilerCollector::response() const {
    json metrics = json::array();
    for (size_t index = 0; index < m_request.metric_indices.size(); ++index) {
        const auto& spec = kProfilerMetrics[m_request.metric_indices[index]];
        const auto& stats = m_stats[index];
        json metric = {
            {"name", spec.name},
            {"unit", spec.unit},
            {"available", true},
            {"availability_basis", "api_bind_and_enum"},
            {"valid_samples", stats.valid},
            {"invalid_samples", stats.invalid},
            {"min", nullptr},
            {"max", nullptr},
            {"mean", nullptr},
            {"last", nullptr},
        };
        if (stats.valid > 0) {
            // A running sum divided back out can land an ulp outside the
            // observed range when every sample is the same value. The mean
            // of readings between min and max is between min and max, so
            // report it that way rather than leaking the rounding.
            const double mean = std::clamp(stats.sum / static_cast<double>(stats.valid),
                                           stats.min, stats.max);
            metric["min"] = stats.min;
            metric["max"] = stats.max;
            metric["mean"] = mean;
            metric["last"] = stats.last;
        }
        metrics.push_back(std::move(metric));
    }
    return {
        {"duration_ms", m_request.duration_ms},
        {"actual_elapsed_ms", m_last_elapsed_ms},
        {"samples_requested", m_request.sample_count},
        {"samples_collected", m_collected},
        {"metrics", std::move(metrics)},
    };
}

} // namespace runtime
} // namespace didi
