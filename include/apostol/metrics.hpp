#pragma once

// Metrics of the processes of one node, in the Prometheus text format.
//
// Every process builds its own rows (MetricsText) when it is asked — over Ipc,
// by the type "metrics" — and the process that serves the scrape gathers the
// answers of all of them and merges them (merge_metrics): one HELP and one
// TYPE per family, its samples together. A process's rows carry the labels
// `role` and `worker` (its number within the role), so two processes never
// give the same series. See Application::enable_metrics().

#include <cstdint>
#include <functional>
#include <span>
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
/// type and help of a later sample of the same name are ignored. A histogram
/// shares no name with a family of another type: see histogram().
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
    /// One histogram series: `<name>_bucket` with `le` for each bound and for
    /// "+Inf", then `<name>_sum` and `<name>_count`, `le` the last label.
    /// @p buckets holds one more entry than @p bounds: buckets[i] counts the
    /// observations in (bounds[i-1], bounds[i]], the last one those above the
    /// last bound. The written buckets are cumulative; "+Inf" and `_count` are
    /// the total of @p buckets. The series of one family are to share the
    /// bounds.
    ///
    /// Throws std::invalid_argument, the text left as it was, on a mistake of
    /// the calling code — the same on every call, whatever was observed: the
    /// sizes do not match, the bounds are not finite and strictly increasing,
    /// a label (common or of this call) is named "le", @p name is declared as
    /// another type, or `<name>_bucket`, `<name>_sum` or `<name>_count` is.
    /// A gauge or a counter likewise refuses the name of a histogram and the
    /// names of its samples.
    void histogram(std::string_view name, std::string_view help,
                   std::span<const double> bounds,
                   std::span<const std::uint64_t> buckets, double sum,
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

    std::string full_name(std::string_view name) const;
    std::string label_set(const MetricLabels& labels) const;
    /// The histogram @p full is, or whose sample it is named as; nullptr if none.
    const std::string* histogram_of(const std::string& full) const;

    struct Family
    {
        std::string              type;
        std::string              help;
        std::vector<std::string> samples;
    };

    std::string                             prefix_;
    std::string                             common_;   // `a="1",b="2"` — empty when none
    bool                                    common_le_ = false;   // a common label "le"
    std::vector<std::string>                order_;
    std::unordered_map<std::string, Family> families_;
};

/// Merge the texts of several processes into one: each family once, with its
/// first HELP and TYPE, followed by its samples from every text in order.
/// Samples of a histogram or a summary (`_bucket`, `_sum`, `_count`) go with
/// their declared family. A sample of no declared family stays on its own.
std::string merge_metrics(const std::vector<std::string_view>& texts);

/// Rows that code with no Application at hand — a layer of the library — adds
/// to the answer of every process it runs in. Registered for the process (a
/// fork keeps the list); called on the process's loop at every collection,
/// before the sources of Application::add_metrics(). Not thread-safe:
/// register from the loop's thread, or before it runs.
using ProcessMetricsSource = std::function<void(MetricsText&)>;
void add_process_metrics(ProcessMetricsSource source);

/// The sources add_process_metrics() registered, in that order.
const std::vector<ProcessMetricsSource>& process_metrics_sources() noexcept;

} // namespace apostol
