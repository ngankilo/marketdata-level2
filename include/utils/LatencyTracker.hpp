#pragma once

#include <chrono>
#include <atomic>
#include <array>
#include <mutex>
#include <vector>
#include <string>
#include <unordered_map>

namespace md_l2 {

/**
 * @brief High-performance latency tracking utility
 * 
 * Tracks latency statistics with minimal overhead using lock-free
 * atomic operations and efficient percentile calculations.
 */
class LatencyTracker {
public:
    /**
     * @brief Time point type for consistent timing
     */
    using TimePoint = std::chrono::steady_clock::time_point;
    using Duration = std::chrono::nanoseconds;
    
    /**
     * @brief Latency statistics for a metric
     */
    struct LatencyStats {
        uint64_t sample_count = 0;
        double min_latency_us = 0.0;
        double max_latency_us = 0.0;
        double avg_latency_us = 0.0;
        double total_latency_us = 0.0;
        
        // Percentiles
        std::unordered_map<double, double> percentiles;
        
        // Recent samples (for percentile calculation)
        std::vector<double> recent_samples;
        
        TimePoint last_update_time;
        
        LatencyStats() {
            last_update_time = std::chrono::steady_clock::now();
        }
    };
    
    /**
     * @brief Configuration for latency tracker
     */
    struct Config {
        std::vector<double> percentiles{50.0, 90.0, 95.0, 99.0, 99.9};
        size_t max_samples_for_percentiles = 10000;
        std::chrono::seconds stats_update_interval{5};
        bool enable_detailed_tracking = true;
    };
    
    /**
     * @brief Constructor
     * @param config Configuration
     */
    explicit LatencyTracker(const Config& config = Config{});
    
    /**
     * @brief Destructor
     */
    ~LatencyTracker() = default;
    
    // Non-copyable but movable
    LatencyTracker(const LatencyTracker&) = delete;
    LatencyTracker& operator=(const LatencyTracker&) = delete;
    LatencyTracker(LatencyTracker&&) = default;
    LatencyTracker& operator=(LatencyTracker&&) = default;
    
    /**
     * @brief Record a latency measurement
     * @param metric_name Name of the metric
     * @param start_time Start time
     * @param end_time End time (defaults to now)
     */
    void record_latency(const std::string& metric_name,
                       TimePoint start_time,
                       TimePoint end_time = std::chrono::steady_clock::now());
    
    /**
     * @brief Record a latency measurement in microseconds
     * @param metric_name Name of the metric
     * @param latency_us Latency in microseconds
     */
    void record_latency_us(const std::string& metric_name, double latency_us);
    
    /**
     * @brief Get current statistics for a metric
     * @param metric_name Name of the metric
     * @return Statistics or nullptr if metric not found
     */
    std::unique_ptr<LatencyStats> get_stats(const std::string& metric_name) const;
    
    /**
     * @brief Get statistics for all metrics
     * @return Map of metric name to statistics
     */
    std::unordered_map<std::string, LatencyStats> get_all_stats() const;
    
    /**
     * @brief Reset statistics for a metric
     * @param metric_name Name of the metric
     */
    void reset_metric(const std::string& metric_name);
    
    /**
     * @brief Reset all statistics
     */
    void reset_all();
    
    /**
     * @brief Get list of tracked metrics
     */
    std::vector<std::string> get_metric_names() const;
    
    /**
     * @brief RAII helper for automatic latency tracking
     */
    class ScopedTimer {
    public:
        ScopedTimer(LatencyTracker* tracker, const std::string& metric_name)
            : tracker_(tracker), metric_name_(metric_name),
              start_time_(std::chrono::steady_clock::now()) {}
        
        ~ScopedTimer() {
            if (tracker_) {
                tracker_->record_latency(metric_name_, start_time_);
            }
        }
        
        // Non-copyable, movable
        ScopedTimer(const ScopedTimer&) = delete;
        ScopedTimer& operator=(const ScopedTimer&) = delete;
        ScopedTimer(ScopedTimer&& other) noexcept
            : tracker_(other.tracker_), metric_name_(std::move(other.metric_name_)),
              start_time_(other.start_time_) {
            other.tracker_ = nullptr;
        }
        
        ScopedTimer& operator=(ScopedTimer&& other) noexcept {
            if (this != &other) {
                tracker_ = other.tracker_;
                metric_name_ = std::move(other.metric_name_);
                start_time_ = other.start_time_;
                other.tracker_ = nullptr;
            }
            return *this;
        }
        
    private:
        LatencyTracker* tracker_;
        std::string metric_name_;
        TimePoint start_time_;
    };
    
    /**
     * @brief Create a scoped timer for automatic tracking
     * @param metric_name Name of the metric
     * @return Scoped timer object
     */
    ScopedTimer create_scoped_timer(const std::string& metric_name) {
        return ScopedTimer(this, metric_name);
    }
    
    /**
     * @brief Overall system statistics
     */
    struct SystemStats {
        size_t total_metrics = 0;
        uint64_t total_measurements = 0;
        double avg_measurement_overhead_ns = 0.0;
        size_t memory_usage_bytes = 0;
        TimePoint start_time;
        std::chrono::duration<double> uptime_seconds{0};
    };
    
    /**
     * @brief Get system-wide statistics
     */
    SystemStats get_system_stats() const;
    
    /**
     * @brief Enable/disable detailed tracking
     * @param enabled Enable detailed tracking
     */
    void set_detailed_tracking_enabled(bool enabled);
    
    /**
     * @brief Print statistics summary to string
     * @param metric_name Specific metric (empty for all)
     * @return Formatted statistics string
     */
    std::string format_stats(const std::string& metric_name = "") const;
    
private:
    Config config_;
    
    // Per-metric statistics storage
    mutable std::mutex stats_mutex_;
    std::unordered_map<std::string, LatencyStats> metrics_;
    
    // System tracking
    TimePoint start_time_;
    std::atomic<uint64_t> total_measurements_{0};
    std::atomic<bool> detailed_tracking_enabled_{true};
    
    // Performance tracking
    mutable std::atomic<uint64_t> overhead_measurements_{0};
    mutable std::atomic<uint64_t> total_overhead_ns_{0};
    
    /**
     * @brief Update percentiles for a metric
     * @param stats Statistics to update
     */
    void update_percentiles(LatencyStats& stats) const;
    
    /**
     * @brief Calculate percentile from sorted samples
     * @param samples Sorted sample vector
     * @param percentile Percentile to calculate (0-100)
     * @return Percentile value
     */
    double calculate_percentile(const std::vector<double>& samples, double percentile) const;
    
    /**
     * @brief Measure overhead of the tracking itself
     */
    void measure_overhead() const;
    
    /**
     * @brief Estimate memory usage
     */
    size_t estimate_memory_usage() const;
};

/**
 * @brief Convenience macros for latency tracking
 */
#define LATENCY_TRACK_SCOPE(tracker, metric_name) \
    auto _latency_timer = (tracker)->create_scoped_timer(metric_name)

#define LATENCY_TRACK_START(tracker, metric_name) \
    auto _start_time_##metric_name = std::chrono::steady_clock::now()

#define LATENCY_TRACK_END(tracker, metric_name) \
    (tracker)->record_latency(#metric_name, _start_time_##metric_name)

} // namespace md_l2