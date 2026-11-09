#include "apostol/metrics.hpp"

#include <fmt/format.h>

#include <cmath>

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

void MetricsText::add(std::string_view type, std::string_view name, std::string_view help,
                      double value, const MetricLabels& labels)
{
    std::string full = prefix_.empty() ? std::string(name) : fmt::format("{}_{}", prefix_, name);

    auto [it, fresh] = families_.try_emplace(full);
    if (fresh) {
        it->second.type = std::string(type);
        it->second.help = std::string(help);
        order_.push_back(full);
    }

    std::string set = common_;
    append_labels(set, labels);

    std::string line = full;
    if (!set.empty()) {
        line += '{';
        line += set;
        line += '}';
    }
    line += ' ';
    line += format_value(value);
    it->second.samples.push_back(std::move(line));
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
            // A histogram's or a summary's samples belong to the declared base.
            for (std::string_view suffix : {"_bucket", "_sum", "_count"}) {
                if (name.size() > suffix.size() && name.ends_with(suffix)) {
                    std::string base(name.substr(0, name.size() - suffix.size()));
                    if (families.contains(base))
                        return families[base];
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

} // namespace apostol
