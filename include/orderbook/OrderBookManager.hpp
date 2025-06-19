#pragma once

#include <unordered_map>
#include <memory>
#include <shared_mutex>
#include <atomic>
#include <vector>
#include <string>

// Use the existing LockFreeOrderBook from the original project
#include "LockFreeOrderBook.hpp"
#include "feed/PitchFeedHandler.hpp"
#include "orderbook/SymbolManager.hpp"
#include "utils/LatencyTracker.hpp"

namespace md_l2 {

/**
 * @brief Level 2 market data snapshot for a symbol
 */
struct Level2Snapshot {
    std::string symbol;
    uint64_t timestamp;
    uint64_t sequence_number;
    
    // Price levels (price -> quantity)
    std::vector<std::pair<double, uint32_t>> buy_levels;   // Sorted high to low
    std::vector<std::pair<double, uint32_t>> sell_levels;  // Sorted low to high
    
    // Trade information
    double last_trade_price = 0.0;
    uint32_t last_trade_quantity = 0;
    uint64_t last_trade_time = 0;
    
    // Book statistics
    uint32_t total_buy_orders = 0;
    uint32_t total_sell_orders = 0;
    uint64_t total_buy_volume = 0;
    uint64_t total_sell_volume = 0;
    
    // Spread information
    double best_bid = 0.0;
    double best_ask = 0.0;
    double spread = 0.0;
    double mid_price = 0.0;
    
    Level2Snapshot(const std::string& sym) : symbol(sym) {
        timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
};

/**
 * @brief Delta update for incremental market data
 */
struct Level2Delta {
    std::string symbol;
    uint64_t timestamp;
    uint64_t sequence_number;
    
    enum class ChangeType {
        ADD_ORDER,
        MODIFY_ORDER,
        DELETE_ORDER,
        TRADE
    };
    
    ChangeType change_type;
    uint64_t order_id = 0;
    double price = 0.0;
    uint32_t quantity = 0;
    char side = ' ';  // 'B' or 'S'
    
    // For trades
    uint64_t execution_id = 0;
    bool is_aggressive = false;
    
    Level2Delta(const std::string& sym, ChangeType type) 
        : symbol(sym), change_type(type) {
        timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
};

/**
 * @brief Callback types for market data events
 */
using Level2SnapshotCallback = std::function<void(const Level2Snapshot&)>;
using Level2DeltaCallback = std::function<void(const Level2Delta&)>;

/**
 * @brief Order book manager handling multiple symbols
 * 
 * Manages order books for multiple trading symbols, processes order book events
 * from the feed handler, and generates Level 2 market data snapshots and deltas.
 * Thread-safe and designed for high-performance market data processing.
 */
class OrderBookManager {
public:
    /**
     * @brief Configuration for order book manager
     */
    struct Config {
        int max_price_levels = 1000;
        int snapshot_interval_ms = 1000;
        int delta_interval_ms = 50;
        bool enable_trade_tracking = true;
        int max_trade_history = 10000;
        
        // Performance settings
        int initial_book_capacity = 10000;
        bool enable_statistics = true;
        
        // Validation settings
        bool enable_price_validation = true;
        bool enable_quantity_validation = true;
        double max_price_deviation_percent = 10.0;
    };
    
    /**
     * @brief Constructor
     * @param config Configuration
     * @param symbol_manager Symbol manager for validation
     * @param latency_tracker Latency tracking (optional)
     */
    OrderBookManager(const Config& config,
                     std::shared_ptr<SymbolManager> symbol_manager,
                     std::shared_ptr<LatencyTracker> latency_tracker = nullptr);
    
    ~OrderBookManager();
    
    // Non-copyable
    OrderBookManager(const OrderBookManager&) = delete;
    OrderBookManager& operator=(const OrderBookManager&) = delete;
    
    /**
     * @brief Initialize order books for symbols
     * @param symbols Vector of symbol names
     * @return Number of order books created
     */
    size_t initialize_symbols(const std::vector<std::string>& symbols);
    
    /**
     * @brief Add order book for a new symbol
     * @param symbol Symbol name
     * @return true if order book was created
     */
    bool add_symbol(const std::string& symbol);
    
    /**
     * @brief Remove order book for a symbol
     * @param symbol Symbol name
     * @return true if order book was removed
     */
    bool remove_symbol(const std::string& symbol);
    
    /**
     * @brief Process order book event from feed handler
     * @param event Order book event
     */
    void process_event(const OrderBookEvent& event);
    
    /**
     * @brief Start periodic snapshot and delta publishing
     * @param snapshot_callback Callback for snapshots
     * @param delta_callback Callback for deltas
     */
    void start_publishing(Level2SnapshotCallback snapshot_callback,
                         Level2DeltaCallback delta_callback);
    
    /**
     * @brief Stop publishing
     */
    void stop_publishing();
    
    /**
     * @brief Get current Level 2 snapshot for a symbol
     * @param symbol Symbol name
     * @return Snapshot or nullptr if symbol not found
     */
    std::unique_ptr<Level2Snapshot> get_snapshot(const std::string& symbol) const;
    
    /**
     * @brief Get best bid/ask for a symbol
     * @param symbol Symbol name
     * @return Pair of (bid, ask) prices, (0,0) if symbol not found
     */
    std::pair<double, double> get_best_bid_ask(const std::string& symbol) const;
    
    /**
     * @brief Get order book statistics
     */
    struct Statistics {
        size_t total_symbols = 0;
        size_t active_symbols = 0;
        
        // Event processing
        uint64_t total_events_processed = 0;
        uint64_t total_add_orders = 0;
        uint64_t total_modify_orders = 0;
        uint64_t total_delete_orders = 0;
        uint64_t total_trades = 0;
        
        // Performance metrics
        double avg_processing_latency_us = 0.0;
        double max_processing_latency_us = 0.0;
        double events_per_second = 0.0;
        
        // Memory usage
        size_t total_orders_in_memory = 0;
        size_t estimated_memory_usage_mb = 0;
        
        // Publishing stats
        uint64_t snapshots_published = 0;
        uint64_t deltas_published = 0;
        
        std::chrono::steady_clock::time_point last_update_time;
    };
    
    const Statistics& get_statistics() const { return stats_; }
    
    /**
     * @brief Reset statistics
     */
    void reset_statistics();
    
    /**
     * @brief Get list of active symbols
     */
    std::vector<std::string> get_active_symbols() const;
    
    /**
     * @brief Clear all order books
     */
    void clear_all();
    
private:
    Config config_;
    std::shared_ptr<SymbolManager> symbol_manager_;
    std::shared_ptr<LatencyTracker> latency_tracker_;
    
    // Order books per symbol
    mutable std::shared_mutex books_mutex_;
    std::unordered_map<std::string, std::unique_ptr<LockFreeOrderBook>> order_books_;
    
    // Publishing threads
    std::atomic<bool> publishing_active_{false};
    std::thread snapshot_thread_;
    std::thread delta_thread_;
    
    // Callbacks
    Level2SnapshotCallback snapshot_callback_;
    Level2DeltaCallback delta_callback_;
    
    // Statistics
    mutable std::mutex stats_mutex_;
    Statistics stats_;
    std::chrono::steady_clock::time_point start_time_;
    
    /**
     * @brief Get or create order book for symbol
     * @param symbol Symbol name
     * @return Pointer to order book, nullptr if symbol invalid
     */
    LockFreeOrderBook* get_or_create_book(const std::string& symbol);
    
    /**
     * @brief Process different event types
     */
    void process_add_order(const OrderBookEvent& event);
    void process_modify_order(const OrderBookEvent& event);
    void process_delete_order(const OrderBookEvent& event);
    void process_trade(const OrderBookEvent& event);
    void process_trading_status(const OrderBookEvent& event);
    
    /**
     * @brief Convert order book to Level 2 snapshot
     * @param symbol Symbol name
     * @param book_data Order book data
     * @return Level 2 snapshot
     */
    std::unique_ptr<Level2Snapshot> create_snapshot(
        const std::string& symbol,
        const std::shared_ptr<const LockFreeOrderBookData>& book_data) const;
    
    /**
     * @brief Create delta from order book event
     * @param event Order book event
     * @return Level 2 delta
     */
    std::unique_ptr<Level2Delta> create_delta(const OrderBookEvent& event) const;
    
    /**
     * @brief Snapshot publishing thread function
     */
    void snapshot_thread_func();
    
    /**
     * @brief Delta publishing thread function
     */
    void delta_thread_func();
    
    /**
     * @brief Validate order book event
     * @param event Event to validate
     * @return true if valid
     */
    bool validate_event(const OrderBookEvent& event) const;
    
    /**
     * @brief Update statistics
     */
    void update_statistics(const OrderBookEvent& event);
    
    /**
     * @brief Calculate spread and mid price
     */
    void calculate_spread_info(Level2Snapshot& snapshot) const;
    
    /**
     * @brief Calculate book statistics
     */
    void calculate_book_stats(Level2Snapshot& snapshot,
                             const std::shared_ptr<const LockFreeOrderBookData>& book_data) const;
};

} // namespace md_l2