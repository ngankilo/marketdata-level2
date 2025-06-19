#pragma once

#include <memory>
#include <functional>
#include <atomic>
#include <thread>
#include <chrono>
#include <queue>
#include <mutex>

// Include PITCH message types (assuming they're available from original project)
#include "pitch/message.h"
#include "pitch/message_factory.h"
#include "pitch/seq_unit_header.h"
#include "pitch/add_order.h"
#include "pitch/modify_order.h"
#include "pitch/delete_order.h"
#include "pitch/order_executed.h"
#include "pitch/trade.h"
#include "pitch/trading_status.h"

#include "orderbook/SymbolManager.hpp"
#include "utils/LatencyTracker.hpp"

namespace md_l2 {

/**
 * @brief Order book event types
 */
enum class OrderBookEventType {
    ADD_ORDER,
    MODIFY_ORDER,
    DELETE_ORDER,
    TRADE,
    TRADING_STATUS_CHANGE,
    UNIT_CLEAR
};

/**
 * @brief Order book event structure
 */
struct OrderBookEvent {
    OrderBookEventType type;
    std::string symbol;
    uint64_t timestamp;
    uint64_t sequence_number;
    
    // Order fields
    uint64_t order_id = 0;
    double price = 0.0;
    uint32_t quantity = 0;
    char side = ' ';  // 'B' for buy, 'S' for sell
    
    // Trade fields
    uint64_t execution_id = 0;
    uint64_t trade_price_scaled = 0;  // Scaled integer price
    uint32_t trade_quantity = 0;
    
    // Trading status
    char trading_status = ' ';
    
    // Execution details
    bool is_aggressive_order = false;
    std::string participant_id;
    
    // Timing information
    std::chrono::steady_clock::time_point receive_time;
    std::chrono::steady_clock::time_point process_time;
    
    OrderBookEvent() : receive_time(std::chrono::steady_clock::now()) {}
};

/**
 * @brief Callback function type for order book events
 */
using OrderBookEventCallback = std::function<void(const OrderBookEvent&)>;

/**
 * @brief Gap detection and recovery handler
 */
class GapHandler {
public:
    GapHandler(int unit_id, int gap_threshold);
    
    /**
     * @brief Check for sequence number gaps
     * @param sequence_number Current sequence number
     * @return true if gap detected
     */
    bool check_gap(uint32_t sequence_number);
    
    /**
     * @brief Reset sequence tracking
     */
    void reset();
    
    /**
     * @brief Get gap statistics
     */
    struct GapStats {
        uint32_t total_gaps = 0;
        uint32_t total_messages_lost = 0;
        uint32_t last_gap_size = 0;
        std::chrono::steady_clock::time_point last_gap_time;
    };
    
    const GapStats& get_stats() const { return stats_; }
    
private:
    int unit_id_;
    int gap_threshold_;
    uint32_t expected_sequence_ = 0;
    bool initialized_ = false;
    GapStats stats_;
};

/**
 * @brief PITCH feed handler for processing market data messages
 * 
 * Handles incoming PITCH messages, parses them, validates sequence numbers,
 * detects gaps, and converts them to normalized order book events.
 * Supports both UDP multicast and file replay modes.
 */
class PitchFeedHandler {
public:
    /**
     * @brief Feed source type
     */
    enum class SourceType {
        UDP_MULTICAST,
        FILE_REPLAY,
        TCP_STREAM
    };
    
    /**
     * @brief Configuration for the feed handler
     */
    struct Config {
        SourceType source_type = SourceType::UDP_MULTICAST;
        std::string multicast_group = "224.0.1.100";
        int multicast_port = 15000;
        std::string interface = "eth0";
        int buffer_size = 65536;
        int unit_id = 1;
        int gap_threshold = 10;
        int heartbeat_interval_ms = 1000;
        
        // File replay settings
        std::string replay_file_path;
        double replay_speed_multiplier = 1.0;
        
        // TCP settings
        std::string tcp_host;
        int tcp_port = 0;
    };
    
    /**
     * @brief Constructor
     * @param config Feed handler configuration
     * @param symbol_manager Symbol manager for validation
     * @param latency_tracker Latency tracking (optional)
     */
    PitchFeedHandler(const Config& config,
                     std::shared_ptr<SymbolManager> symbol_manager,
                     std::shared_ptr<LatencyTracker> latency_tracker = nullptr);
    
    ~PitchFeedHandler();
    
    // Non-copyable
    PitchFeedHandler(const PitchFeedHandler&) = delete;
    PitchFeedHandler& operator=(const PitchFeedHandler&) = delete;
    
    /**
     * @brief Start the feed handler
     * @param event_callback Callback for order book events
     * @return true if started successfully
     */
    bool start(OrderBookEventCallback event_callback);
    
    /**
     * @brief Stop the feed handler
     */
    void stop();
    
    /**
     * @brief Check if feed handler is running
     */
    bool is_running() const { return running_.load(); }
    
    /**
     * @brief Get feed statistics
     */
    struct Statistics {
        uint64_t total_messages_received = 0;
        uint64_t total_messages_processed = 0;
        uint64_t total_messages_dropped = 0;
        uint64_t total_bytes_received = 0;
        uint64_t total_parsing_errors = 0;
        uint64_t total_validation_errors = 0;
        
        // Message type counts
        uint64_t add_order_count = 0;
        uint64_t modify_order_count = 0;
        uint64_t delete_order_count = 0;
        uint64_t trade_count = 0;
        uint64_t status_count = 0;
        
        // Performance metrics
        double messages_per_second = 0.0;
        double avg_processing_latency_us = 0.0;
        double max_processing_latency_us = 0.0;
        
        // Gap information
        GapHandler::GapStats gap_stats;
        
        // Last update time
        std::chrono::steady_clock::time_point last_update_time;
    };
    
    const Statistics& get_statistics() const { return stats_; }
    
    /**
     * @brief Reset statistics
     */
    void reset_statistics();
    
private:
    Config config_;
    std::shared_ptr<SymbolManager> symbol_manager_;
    std::shared_ptr<LatencyTracker> latency_tracker_;
    OrderBookEventCallback event_callback_;
    
    // Threading
    std::atomic<bool> running_{false};
    std::thread receiver_thread_;
    std::thread processor_thread_;
    
    // Message queue for decoupling receive and process
    std::queue<std::vector<uint8_t>> message_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    static constexpr size_t MAX_QUEUE_SIZE = 10000;
    
    // Gap detection
    std::unique_ptr<GapHandler> gap_handler_;
    
    // Statistics
    mutable std::mutex stats_mutex_;
    Statistics stats_;
    std::chrono::steady_clock::time_point start_time_;
    
    // Socket for UDP multicast
    int socket_fd_ = -1;
    
    /**
     * @brief Main receiver thread function
     */
    void receiver_thread_func();
    
    /**
     * @brief Main processor thread function
     */
    void processor_thread_func();
    
    /**
     * @brief Setup UDP multicast socket
     */
    bool setup_udp_socket();
    
    /**
     * @brief Process a single message buffer
     * @param buffer Message buffer
     * @return Number of events generated
     */
    int process_message_buffer(const std::vector<uint8_t>& buffer);
    
    /**
     * @brief Convert PITCH message to order book event
     * @param message PITCH message
     * @param sequence_number Sequence number from header
     * @return Order book event (null if not convertible)
     */
    std::unique_ptr<OrderBookEvent> convert_message_to_event(
        const std::shared_ptr<CboePitch::Message>& message,
        uint32_t sequence_number);
    
    /**
     * @brief Handle different PITCH message types
     */
    std::unique_ptr<OrderBookEvent> handle_add_order(
        const CboePitch::AddOrder& msg, uint32_t sequence_number);
    
    std::unique_ptr<OrderBookEvent> handle_modify_order(
        const CboePitch::ModifyOrder& msg, uint32_t sequence_number);
    
    std::unique_ptr<OrderBookEvent> handle_delete_order(
        const CboePitch::DeleteOrder& msg, uint32_t sequence_number);
    
    std::unique_ptr<OrderBookEvent> handle_trade(
        const CboePitch::Trade& msg, uint32_t sequence_number);
    
    std::unique_ptr<OrderBookEvent> handle_trading_status(
        const CboePitch::TradingStatus& msg, uint32_t sequence_number);
    
    /**
     * @brief Validate message against symbol manager
     */
    bool validate_message(const OrderBookEvent& event);
    
    /**
     * @brief Update statistics
     */
    void update_statistics(const OrderBookEvent& event);
    
    /**
     * @brief Cleanup resources
     */
    void cleanup();
};

} // namespace md_l2