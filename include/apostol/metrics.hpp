#pragma once

// Metrics of the processes of one node, in the Prometheus text format.
//
// Every process builds its own rows (MetricsText) when it is asked — over Ipc,
// by the type "metrics" — and the process that serves the scrape gathers the
// answers of all of them and merges them (merge_metrics): one HELP and one
// TYPE per family, its samples together. A process's rows carry the labels
// `role` and `worker` (its number within the role), so two processes never
// give the same series. See Application::enable_metrics().

#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace apostol
{

using MetricLabels = std::vector<std::pair<std::string, std::string>>;

/// The rows of one process. Names are relative: "db_listener_up" is written as
/// "<prefix>_db_listener_up". A family is declared by its first sample; the
/// type and help of a later sample of the same name are ignored.
class MetricsText
{
public:
    /// @p common — labels put first on every sample.
    explicit MetricsText(std::string prefix, MetricLabels common = {});

    void gauge(std::string_view name, std::string_view help, double value,
               const MetricLabels& labels = {});
    /// @p name ends with "_total".
    void counter(std::string_view name, std::string_view help, double value,
                 const MetricLabels& labels = {});

    const std::string& prefix() const noexcept { return prefix_; }
    bool empty() const noexcept { return order_.empty(); }

    /// HELP, TYPE and the samples of each family, families in the order of
    /// their first sample.
    std::string str() const;

    /// A label value as the text format wants it: \, " and a newline escaped.
    static std::string escape(std::string_view value);

private:
    void add(std::string_view type, std::string_view name, std::string_view help, double value,
             const MetricLabels& labels);

    struct Family
    {
        std::string              type;
        std::string              help;
        std::vector<std::string> samples;
    };

    std::string                             prefix_;
    std::string                             common_;   // `a="1",b="2"` — empty when none
    std::vector<std::string>                order_;
    std::unordered_map<std::string, Family> families_;
};

/// Merge the texts of several processes into one: each family once, with its
/// first HELP and TYPE, followed by its samples from every text in order.
/// Samples of a histogram or a summary (`_bucket`, `_sum`, `_count`) go with
/// their declared family. A sample of no declared family stays on its own.
std::string merge_metrics(const std::vector<std::string_view>& texts);

} // namespace apostol
