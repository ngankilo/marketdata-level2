#include "utils/LatencyTracker.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace md_l2 {

LatencyTracker::LatencyTracker(const Config& config)
    : config_(config), start_time_(std::chrono::steady_clock::now()) {
    SPDLOG_DEBUG("LatencyTracker initialized with {} percentiles", config_.percentiles.size());
}

void LatencyTracker::record_latency(const std::string& metric_name,
                                   TimePoint start_time,
                                   TimePoint end_time) {
    if (!detailed_tracking_enabled_.load(std::memory_order_relaxed)) {
        return;
    }
    
    auto latency_ns = std::chrono::duration_cast<Duration>(end_time - start_time);
    double latency_us = static_cast<double>(latency_ns.count()) / 1000.0;
    
    record_latency_us(metric_name, latency_us);
}

void LatencyTracker::record_latency_us(const std::string& metric_name, double latency_us) {
    if (!detailed_tracking_enabled_.load(std::memory_order_relaxed)) {
        return;
    }
    
    total_measurements_.fetch_add(1, std::memory_order_relaxed);
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    auto& stats = metrics_[metric_name];
    
    // Update basic statistics
    if (stats.sample_count == 0) {
        stats.min_latency_us = latency_us;
        stats.max_latency_us = latency_us;
        stats.avg_latency_us = latency_us;
        stats.total_latency_us = latency_us;
    } else {
        stats.min_latency_us = std::min(stats.min_latency_us, latency_us);
        stats.max_latency_us = std::max(stats.max_latency_us, latency_us);
        stats.total_latency_us += latency_us;
        stats.avg_latency_us = stats.total_latency_us / (stats.sample_count + 1);
    }
    
    stats.sample_count++;
    stats.last_update_time = std::chrono::steady_clock::now();
    
    // Add to recent samples for percentile calculation
    if (config_.enable_detailed_tracking) {
        stats.recent_samples.push_back(latency_us);
        
        // Keep only recent samples for memory management
        if (stats.recent_samples.size() > config_.max_samples_for_percentiles) {
            // Remove oldest samples (keep most recent half)
            size_t keep_count = config_.max_samples_for_percentiles / 2;
            stats.recent_samples.erase(
                stats.recent_samples.begin(),
                stats.recent_samples.end() - keep_count
            );
        }
        
        // Update percentiles periodically
        auto now = std::chrono::steady_clock::now();
        auto time_since_update = now - stats.last_update_time;
        if (time_since_update >= config_.stats_update_interval || 
            stats.sample_count % 1000 == 0) {
            update_percentiles(stats);
        }
    }
}

std::unique_ptr<LatencyTracker::LatencyStats> LatencyTracker::get_stats(const std::string& metric_name) const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    auto it = metrics_.find(metric_name);
    if (it == metrics_.end()) {
        return nullptr;
    }
    
    auto stats = std::make_unique<LatencyStats>(it->second);
    
    // Update percentiles if needed
    if (config_.enable_detailed_tracking && !stats->recent_samples.empty()) {
        const_cast<LatencyTracker*>(this)->update_percentiles(*stats);
    }
    
    return stats;
}

std::unordered_map<std::string, LatencyTracker::LatencyStats> LatencyTracker::get_all_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    auto stats_copy = metrics_;
    
    // Update percentiles for all metrics if needed
    if (config_.enable_detailed_tracking) {
        for (auto& [name, stats] : stats_copy) {
            if (!stats.recent_samples.empty()) {
                const_cast<LatencyTracker*>(this)->update_percentiles(stats);
            }
        }
    }
    
    return stats_copy;
}

void LatencyTracker::reset_metric(const std::string& metric_name) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    auto it = metrics_.find(metric_name);
    if (it != metrics_.end()) {
        metrics_.erase(it);
        SPDLOG_DEBUG("Reset latency statistics for metric: {}", metric_name);
    }
}

void LatencyTracker::reset_all() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    size_t metric_count = metrics_.size();
    metrics_.clear();
    total_measurements_.store(0, std::memory_order_relaxed);
    overhead_measurements_.store(0, std::memory_order_relaxed);
    total_overhead_ns_.store(0, std::memory_order_relaxed);
    
    SPDLOG_INFO("Reset all latency statistics ({} metrics)", metric_count);
}

std::vector<std::string> LatencyTracker::get_metric_names() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    std::vector<std::string> names;
    names.reserve(metrics_.size());
    
    for (const auto& [name, stats] : metrics_) {
        names.push_back(name);
    }
    
    return names;
}

LatencyTracker::SystemStats LatencyTracker::get_system_stats() const {
    SystemStats sys_stats;
    
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        sys_stats.total_metrics = metrics_.size();
        sys_stats.memory_usage_bytes = estimate_memory_usage();
    }
    
    sys_stats.total_measurements = total_measurements_.load(std::memory_order_relaxed);
    sys_stats.start_time = start_time_;
    
    auto now = std::chrono::steady_clock::now();
    sys_stats.uptime_seconds = std::chrono::duration_cast<std::chrono::duration<double>>(now - start_time_);
    
    // Calculate average measurement overhead
    uint64_t overhead_measurements = overhead_measurements_.load(std::memory_order_relaxed);
    uint64_t total_overhead_ns = total_overhead_ns_.load(std::memory_order_relaxed);
    
    if (overhead_measurements > 0) {
        sys_stats.avg_measurement_overhead_ns = static_cast<double>(total_overhead_ns) / overhead_measurements;
    }
    
    return sys_stats;
}

void LatencyTracker::set_detailed_tracking_enabled(bool enabled) {
    detailed_tracking_enabled_.store(enabled, std::memory_order_relaxed);
    SPDLOG_INFO("Detailed latency tracking {}", enabled ? "enabled" : "disabled");
}

std::string LatencyTracker::format_stats(const std::string& metric_name) const {
    std::ostringstream oss;
    
    if (metric_name.empty()) {
        // Format all metrics
        auto all_stats = get_all_stats();
        
        oss << "=== Latency Statistics ===\n";
        oss << std::fixed << std::setprecision(2);
        
        for (const auto& [name, stats] : all_stats) {
            oss << "Metric: " << name << "\n";
            oss << "  Samples: " << stats.sample_count << "\n";
            oss << "  Min: " << stats.min_latency_us << " μs\n";
            oss << "  Avg: " << stats.avg_latency_us << " μs\n";
            oss << "  Max: " << stats.max_latency_us << " μs\n";
            
            for (const auto& [percentile, value] : stats.percentiles) {
                oss << "  P" << std::setprecision(1) << percentile << ": " 
                    << std::setprecision(2) << value << " μs\n";
            }
            oss << "\n";
        }
        
        // System stats
        auto sys_stats = get_system_stats();
        oss << "System Statistics:\n";
        oss << "  Total Metrics: " << sys_stats.total_metrics << "\n";
        oss << "  Total Measurements: " << sys_stats.total_measurements << "\n";
        oss << "  Memory Usage: " << (sys_stats.memory_usage_bytes / 1024) << " KB\n";
        oss << "  Avg Overhead: " << std::setprecision(1) << sys_stats.avg_measurement_overhead_ns << " ns\n";
        oss << "  Uptime: " << std::setprecision(1) << sys_stats.uptime_seconds.count() << " seconds\n";
        
    } else {
        // Format specific metric
        auto stats = get_stats(metric_name);
        if (stats) {
            oss << "Metric: " << metric_name << "\n";
            oss << std::fixed << std::setprecision(2);
            oss << "Samples: " << stats->sample_count << "\n";
            oss << "Min: " << stats->min_latency_us << " μs\n";
            oss << "Avg: " << stats->avg_latency_us << " μs\n";
            oss << "Max: " << stats->max_latency_us << " μs\n";
            
            for (const auto& [percentile, value] : stats->percentiles) {
                oss << "P" << std::setprecision(1) << percentile << ": " 
                    << std::setprecision(2) << value << " μs\n";
            }
        } else {
            oss << "Metric '" << metric_name << "' not found\n";
        }
    }
    
    return oss.str();
}

void LatencyTracker::update_percentiles(LatencyStats& stats) const {
    if (stats.recent_samples.empty() || config_.percentiles.empty()) {
        return;
    }
    
    // Sort samples for percentile calculation
    auto sorted_samples = stats.recent_samples;
    std::sort(sorted_samples.begin(), sorted_samples.end());
    
    // Calculate percentiles
    stats.percentiles.clear();
    for (double percentile : config_.percentiles) {
        double value = calculate_percentile(sorted_samples, percentile);
        stats.percentiles[percentile] = value;
    }
}

double LatencyTracker::calculate_percentile(const std::vector<double>& samples, double percentile) const {
    if (samples.empty()) {
        return 0.0;
    }
    
    if (percentile <= 0.0) {
        return samples.front();
    }
    
    if (percentile >= 100.0) {
        return samples.back();
    }
    
    // Calculate index (using linear interpolation method)
    double index = (percentile / 100.0) * (samples.size() - 1);
    size_t lower_index = static_cast<size_t>(std::floor(index));
    size_t upper_index = static_cast<size_t>(std::ceil(index));
    
    if (lower_index == upper_index) {
        return samples[lower_index];
    }
    
    // Linear interpolation
    double weight = index - lower_index;
    return samples[lower_index] * (1.0 - weight) + samples[upper_index] * weight;
}

void LatencyTracker::measure_overhead() const {
    auto start = std::chrono::steady_clock::now();
    
    // Simulate the overhead of the measurement itself
    auto end = std::chrono::steady_clock::now();
    
    auto overhead_ns = std::chrono::duration_cast<Duration>(end - start).count();
    
    overhead_measurements_.fetch_add(1, std::memory_order_relaxed);
    total_overhead_ns_.fetch_add(overhead_ns, std::memory_order_relaxed);
}

size_t LatencyTracker::estimate_memory_usage() const {
    size_t total_bytes = 0;
    
    // Base object size
    total_bytes += sizeof(*this);
    
    // Metrics map overhead
    total_bytes += metrics_.size() * (sizeof(std::string) + sizeof(LatencyStats));
    
    // Recent samples storage
    for (const auto& [name, stats] : metrics_) {
        total_bytes += name.capacity();
        total_bytes += stats.recent_samples.capacity() * sizeof(double);
        total_bytes += stats.percentiles.size() * (sizeof(double) + sizeof(double));
    }
    
    return total_bytes;
}

} // namespace md_l2