#pragma once

#include <memory>
#include <atomic>
#include <thread>
#include <chrono>
#include <string>
#include <vector>

#include "config/ConfigManager.hpp"
#include "feed/PitchFeedHandler.hpp"
#include "orderbook/OrderBookManager.hpp"
#include "orderbook/Level2Publisher.hpp"
#include "orderbook/SymbolManager.hpp"
#include "utils/LatencyTracker.hpp"

namespace md_l2 {

/**
 * @brief System health status
 */
enum class SystemHealth {
    HEALTHY,
    DEGRADED,
    CRITICAL,
    FAILED
};

/**
 * @brief Main market data processor coordinating all components
 * 
 * This is the main orchestrator class that coordinates:
 * - PITCH feed handling
 * - Multiple order book management
 * - Level 2 data publishing
 * - System monitoring and health checks
 * - Performance tracking
 */
class MarketDataProcessor {
public:
    /**
     * @brief System status information
     */
    struct SystemStatus {
        SystemHealth health = SystemHealth::HEALTHY;
        std::string status_message;
        
        // Component status
        bool feed_handler_running = false;
        bool order_book_manager_running = false;
        bool publisher_running = false;
        
        // Performance metrics
        double total_throughput_msg_per_sec = 0.0;
        double avg_end_to_end_latency_us = 0.0;
        double max_end_to_end_latency_us = 0.0;
        
        // Memory usage
        size_t estimated_memory_usage_mb = 0;
        
        // Uptime
        std::chrono::steady_clock::time_point start_time;
        std::chrono::duration<double> uptime_seconds{0};
        
        // Error counts
        uint64_t total_errors = 0;
        uint64_t feed_errors = 0;
        uint64_t processing_errors = 0;
        uint64_t publishing_errors = 0;
        
        std::chrono::steady_clock::time_point last_update_time;
    };
    
    /**
     * @brief Constructor
     * @param config_manager Configuration manager
     */
    explicit MarketDataProcessor(std::shared_ptr<ConfigManager> config_manager);
    
    /**
     * @brief Destructor
     */
    ~MarketDataProcessor();
    
    // Non-copyable
    MarketDataProcessor(const MarketDataProcessor&) = delete;
    MarketDataProcessor& operator=(const MarketDataProcessor&) = delete;
    
    /**
     * @brief Initialize the market data processor
     * @return true if initialization successful
     */
    bool initialize();
    
    /**
     * @brief Start the market data processor
     * @return true if started successfully
     */
    bool start();
    
    /**
     * @brief Stop the market data processor
     */
    void stop();
    
    /**
     * @brief Graceful shutdown with timeout
     * @param timeout_seconds Maximum time to wait for shutdown
     * @return true if shutdown completed within timeout
     */
    bool shutdown(int timeout_seconds = 30);
    
    /**
     * @brief Check if processor is running
     */
    bool is_running() const { return running_.load(); }
    
    /**
     * @brief Get current system status
     */
    const SystemStatus& get_system_status() const { return system_status_; }
    
    /**
     * @brief Get system health
     */
    SystemHealth get_system_health() const { return system_status_.health; }
    
    /**
     * @brief Add symbol for processing
     * @param symbol Symbol name
     * @param exchange Exchange name (optional)
     * @return true if symbol was added
     */
    bool add_symbol(const std::string& symbol, const std::string& exchange = "");
    
    /**
     * @brief Remove symbol from processing
     * @param symbol Symbol name
     * @return true if symbol was removed
     */
    bool remove_symbol(const std::string& symbol);
    
    /**
     * @brief Get list of active symbols
     */
    std::vector<std::string> get_active_symbols() const;
    
    /**
     * @brief Get best bid/ask for a symbol
     * @param symbol Symbol name
     * @return Pair of (bid, ask) prices
     */
    std::pair<double, double> get_best_bid_ask(const std::string& symbol) const;
    
    /**
     * @brief Get Level 2 snapshot for a symbol
     * @param symbol Symbol name
     * @return Snapshot or nullptr if not available
     */
    std::unique_ptr<Level2Snapshot> get_level2_snapshot(const std::string& symbol) const;
    
    /**
     * @brief Performance statistics aggregated from all components
     */
    struct PerformanceStats {
        // Feed handler stats
        PitchFeedHandler::Statistics feed_stats;
        
        // Order book manager stats
        OrderBookManager::Statistics orderbook_stats;
        
        // Publisher stats
        Level2Publisher::Statistics publisher_stats;
        
        // System-wide metrics
        double total_messages_per_second = 0.0;
        double avg_processing_latency_us = 0.0;
        double max_processing_latency_us = 0.0;
        
        // Resource usage
        size_t total_memory_usage_mb = 0;
        double cpu_usage_percent = 0.0;
        
        std::chrono::steady_clock::time_point snapshot_time;
    };
    
    /**
     * @brief Get performance statistics
     */
    PerformanceStats get_performance_stats() const;
    
    /**
     * @brief Reset all statistics
     */
    void reset_statistics();
    
    /**
     * @brief Enable/disable performance monitoring
     * @param enabled Enable monitoring
     */
    void set_monitoring_enabled(bool enabled);
    
    /**
     * @brief Get component handles for advanced operations
     */
    std::shared_ptr<PitchFeedHandler> get_feed_handler() const { return feed_handler_; }
    std::shared_ptr<OrderBookManager> get_orderbook_manager() const { return orderbook_manager_; }
    std::shared_ptr<Level2Publisher> get_publisher() const { return publisher_; }
    std::shared_ptr<SymbolManager> get_symbol_manager() const { return symbol_manager_; }
    std::shared_ptr<LatencyTracker> get_latency_tracker() const { return latency_tracker_; }
    
private:
    std::shared_ptr<ConfigManager> config_manager_;
    
    // Core components
    std::shared_ptr<SymbolManager> symbol_manager_;
    std::shared_ptr<LatencyTracker> latency_tracker_;
    std::shared_ptr<PitchFeedHandler> feed_handler_;
    std::shared_ptr<OrderBookManager> orderbook_manager_;
    std::shared_ptr<Level2Publisher> publisher_;
    
    // State management
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    
    // System monitoring
    mutable std::mutex status_mutex_;
    SystemStatus system_status_;
    std::thread monitoring_thread_;
    std::atomic<bool> monitoring_enabled_{true};
    
    // Error handling
    std::atomic<uint64_t> error_count_{0};
    std::chrono::steady_clock::time_point last_error_time_;
    
    /**
     * @brief Initialize all components
     */
    bool initialize_components();
    
    /**
     * @brief Setup component interconnections
     */
    bool setup_component_connections();
    
    /**
     * @brief Order book event handler (from feed handler)
     * @param event Order book event
     */
    void handle_orderbook_event(const OrderBookEvent& event);
    
    /**
     * @brief Level 2 snapshot handler (from order book manager)
     * @param snapshot Level 2 snapshot
     */
    void handle_level2_snapshot(const Level2Snapshot& snapshot);
    
    /**
     * @brief Level 2 delta handler (from order book manager)
     * @param delta Level 2 delta
     */
    void handle_level2_delta(const Level2Delta& delta);
    
    /**
     * @brief Published message handler (from publisher)
     * @param message Published message
     */
    void handle_published_message(const PublishedMessage& message);
    
    /**
     * @brief System monitoring thread function
     */
    void monitoring_thread_func();
    
    /**
     * @brief Update system status
     */
    void update_system_status();
    
    /**
     * @brief Check system health
     */
    SystemHealth assess_system_health() const;
    
    /**
     * @brief Handle system errors
     * @param component Component name
     * @param error_message Error message
     */
    void handle_error(const std::string& component, const std::string& error_message);
    
    /**
     * @brief Calculate memory usage
     */
    size_t calculate_memory_usage() const;
    
    /**
     * @brief Calculate CPU usage
     */
    double calculate_cpu_usage() const;
    
    /**
     * @brief Cleanup all resources
     */
    void cleanup();
    
    /**
     * @brief Validate configuration
     */
    bool validate_configuration() const;
    
    /**
     * @brief Log system startup information
     */
    void log_startup_info() const;
    
    /**
     * @brief Log system shutdown information
     */
    void log_shutdown_info() const;
};

} // namespace md_l2