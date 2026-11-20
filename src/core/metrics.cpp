#include "apostol/metrics.hpp"

#include <fmt/format.h>

#include <cmath>
#include <iterator>
#include <stdexcept>

namespace apostol
{

namespace
{

std::string format_value(double v)
{
    if (std::isnan(v))
        return "NaN";
    if (std::isinf(v))
        return v > 0 ? "+Inf" : "-Inf";
    return fmt::format("{}", v);
}

// HELP text: \ and a newline escaped (a quote is fine there).
std::string escape_help(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == '\\')
            out += "\\\\";
        else if (c == '\n')
            out += "\\n";
        else
            out += c;
    }
    return out;
}

void append_labels(std::string& out, const MetricLabels& labels)
{
    for (const auto& [k, v] : labels) {
        if (!out.empty())
            out += ',';
        out += k;
        out += "=\"";
        out += MetricsText::escape(v);
        out += '"';
    }
}

// "{a=\"1\"}" — nothing for an empty set.
std::string braced(const std::string& set)
{
    return set.empty() ? std::string() : fmt::format("{{{}}}", set);
}

constexpr std::string_view k_histogram_suffixes[] = {"_bucket", "_sum", "_count"};

std::string_view sample_name(std::string_view line)
{
    const auto end = line.find_first_of("{ ");
    return end == std::string_view::npos ? line : line.substr(0, end);
}

} // namespace

std::string MetricsText::escape(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\\')
            out += "\\\\";
        else if (c == '"')
            out += "\\\"";
        else if (c == '\n')
            out += "\\n";
        else
            out += c;
    }
    return out;
}

MetricsText::MetricsText(std::string prefix, MetricLabels common)
    : prefix_(std::move(prefix))
{
    append_labels(common_, common);
    for (const auto& [k, v] : common)
        if (k == "le")
            common_le_ = true;
}

void MetricsText::gauge(std::string_view name, std::string_view help, double value,
                        const MetricLabels& labels)
{
    add("gauge", name, help, value, labels);
}

void MetricsText::counter(std::string_view name, std::string_view help, double value,
                          const MetricLabels& labels)
{
    add("counter", name, help, value, labels);
}

std::string MetricsText::full_name(std::string_view name) const
{
    return prefix_.empty() ? std::string(name) : fmt::format("{}_{}", prefix_, name);
}

std::string MetricsText::label_set(const MetricLabels& labels) const
{
    std::string set = common_;
    append_labels(set, labels);
    return set;
}

const std::string* MetricsText::histogram_of(const std::string& full) const
{
    if (auto it = families_.find(full); it != families_.end())
        return it->second.type == "histogram" ? &it->first : nullptr;
    for (std::string_view suffix : k_histogram_suffixes) {
        if (full.size() > suffix.size() && full.ends_with(suffix)) {
            auto it = families_.find(full.substr(0, full.size() - suffix.size()));
            if (it != families_.end() && it->second.type == "histogram")
                return &it->first;
        }
    }
    return nullptr;
}

void MetricsText::histogram(std::string_view name, std::string_view help,
                            std::span<const double> bounds,
                            std::span<const std::uint64_t> buckets, double sum,
                            const MetricLabels& labels)
{
    std::string full = full_name(name);

    auto refuse = [&full](std::string_view why) {
        throw std::invalid_argument(fmt::format("histogram {}: {}", full, why));
    };

    if (buckets.size() != bounds.size() + 1)
        refuse(fmt::format("{} bounds want {} buckets, not {}", bounds.size(), bounds.size() + 1,
                           buckets.size()));
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        if (!std::isfinite(bounds[i]))
            refuse(fmt::format("bound {} is not finite", i));
        if (i > 0 && !(bounds[i - 1] < bounds[i]))
            refuse(fmt::format("the bounds do not increase at {}", i));
    }
    if (common_le_)
        refuse("a common label is named \"le\"");
    for (const auto& [k, v] : labels)
        if (k == "le")
            refuse("a label is named \"le\"");
    if (auto it = families_.find(full); it != families_.end() && it->second.type != "histogram")
        refuse(fmt::format("the name is declared as a {}", it->second.type));
    if (auto h = histogram_of(full); h != nullptr && *h != full)
        refuse(fmt::format("the name is a sample of the histogram {}", *h));
    for (std::string_view suffix : k_histogram_suffixes)
        if (families_.contains(full + std::string(suffix)))
            refuse(fmt::format("{}{} is declared", full, suffix));

    // The lines are built aside and appended at once: a throw on the way
    // leaves no half of a series in the text.
    const std::string set = label_set(labels);
    const std::string le  = set.empty() ? "le" : set + ",le";

    std::vector<std::string> lines;
    lines.reserve(buckets.size() + 2);
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < buckets.size(); ++i) {
        cumulative += buckets[i];
        const std::string bound = i < bounds.size() ? format_value(bounds[i]) : "+Inf";
        lines.push_back(fmt::format("{}_bucket{{{}=\"{}\"}} {}", full, le, bound, cumulative));
    }
    lines.push_back(fmt::format("{}_sum{} {}", full, braced(set), format_value(sum)));
    lines.push_back(fmt::format("{}_count{} {}", full, braced(set), cumulative));

    auto [it, fresh] = families_.try_emplace(full);
    if (fresh) {
        it->second.type = "histogram";
        it->second.help = std::string(help);
        order_.push_back(full);
    }
    auto& samples = it->second.samples;
    samples.reserve(samples.size() + lines.size());
    std::move(lines.begin(), lines.end(), std::back_inserter(samples));
}

void MetricsText::add(std::string_view type, std::string_view name, std::string_view help,
                      double value, const MetricLabels& labels)
{
    std::string full = full_name(name);

    if (auto h = histogram_of(full); h != nullptr)
        throw std::invalid_argument(fmt::format(
            "{} {}: the name belongs to the histogram {}", type, full, *h));

    auto [it, fresh] = families_.try_emplace(full);
    if (fresh) {
        it->second.type = std::string(type);
        it->second.help = std::string(help);
        order_.push_back(full);
    }

    it->second.samples.push_back(
        fmt::format("{}{} {}", full, braced(label_set(labels)), format_value(value)));
}

std::string MetricsText::str() const
{
    std::string out;
    for (const auto& name : order_) {
        const auto& f = families_.at(name);
        if (!f.help.empty())
            out += fmt::format("# HELP {} {}\n", name, escape_help(f.help));
        out += fmt::format("# TYPE {} {}\n", name, f.type);
        for (const auto& s : f.samples) {
            out += s;
            out += '\n';
        }
    }
    return out;
}

std::string merge_metrics(const std::vector<std::string_view>& texts)
{
    struct Family
    {
        std::string              help;   // the whole "# HELP" line
        std::string              type;   // the whole "# TYPE" line
        std::vector<std::string_view> samples;
    };
    std::vector<std::string>                order;
    std::unordered_map<std::string, Family> families;

    auto family_of = [&](std::string_view name) -> Family& {
        std::string key(name);
        if (!families.contains(key)) {
            // A histogram's or a summary's samples belong to the declared base —
            // only to a base declared as one: "x_count" next to a gauge "x" is a
            // family of its own.
            for (std::string_view suffix : {"_bucket", "_sum", "_count"}) {
                if (name.size() > suffix.size() && name.ends_with(suffix)) {
                    std::string base(name.substr(0, name.size() - suffix.size()));
                    auto it = families.find(base);
                    if (it == families.end())
                        continue;
                    const std::string_view type = it->second.type;
                    if (type.ends_with(" histogram")
                        || (suffix != "_bucket" && type.ends_with(" summary")))
                        return it->second;
                }
            }
            order.push_back(key);
        }
        return families[key];
    };

    for (std::string_view text : texts) {
        std::size_t pos = 0;
        while (pos < text.size()) {
            auto nl = text.find('\n', pos);
            if (nl == std::string_view::npos)
                nl = text.size();
            std::string_view line = text.substr(pos, nl - pos);
            pos = nl + 1;
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            if (line.empty())
                continue;

            if (line.front() == '#') {
                const bool help = line.starts_with("# HELP ");
                const bool type = line.starts_with("# TYPE ");
                if (!help && !type)
                    continue;   // another comment: nothing to keep
                std::string_view rest = line.substr(7);
                auto& f = family_of(sample_name(rest));
                auto& slot = help ? f.help : f.type;
                if (slot.empty())
                    slot = std::string(line);
                continue;
            }
            family_of(sample_name(line)).samples.push_back(line);
        }
    }

    std::string out;
    for (const auto& name : order) {
        const auto& f = families.at(name);
        if (!f.help.empty()) {
            out += f.help;
            out += '\n';
        }
        if (!f.type.empty()) {
            out += f.type;
            out += '\n';
        }
        for (auto s : f.samples) {
            out += s;
            out += '\n';
        }
    }
    return out;
}

// ─── Process sources ─────────────────────────────────────────────────────────

namespace
{

std::vector<ProcessMetricsSource>& process_sources() noexcept
{
    static std::vector<ProcessMetricsSource> sources;
    return sources;
}

} // namespace

void add_process_metrics(ProcessMetricsSource source)
{
    process_sources().push_back(std::move(source));
}

const std::vector<ProcessMetricsSource>& process_metrics_sources() noexcept
{
    return process_sources();
}

} // namespace apostol
