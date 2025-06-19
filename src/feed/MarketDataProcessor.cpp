#include "feed/MarketDataProcessor.hpp"
#include <spdlog/spdlog.h>
#include <thread>
#include <fstream>

namespace md_l2 {

MarketDataProcessor::MarketDataProcessor(std::shared_ptr<ConfigManager> config_manager)
    : config_manager_(config_manager) {
    
    system_status_.start_time = std::chrono::steady_clock::now();
    SPDLOG_INFO("MarketDataProcessor created");
}

MarketDataProcessor::~MarketDataProcessor() {
    shutdown();
    cleanup();
}

bool MarketDataProcessor::initialize() {
    if (initialized_.load()) {
        SPDLOG_WARN("MarketDataProcessor already initialized");
        return true;
    }
    
    try {
        SPDLOG_INFO("Initializing MarketDataProcessor...");
        
        // Validate configuration
        if (!validate_configuration()) {
            SPDLOG_ERROR("Configuration validation failed");
            return false;
        }
        
        // Initialize components
        if (!initialize_components()) {
            SPDLOG_ERROR("Component initialization failed");
            return false;
        }
        
        // Setup component connections
        if (!setup_component_connections()) {
            SPDLOG_ERROR("Component connection setup failed");
            return false;
        }
        
        initialized_.store(true);
        system_status_.health = SystemHealth::HEALTHY;
        system_status_.status_message = "Initialized successfully";
        
        SPDLOG_INFO("MarketDataProcessor initialized successfully");
        log_startup_info();
        
        return true;
        
    } catch (const std::exception& ex) {
        SPDLOG_ERROR("MarketDataProcessor initialization failed: {}", ex.what());
        system_status_.health = SystemHealth::FAILED;
        system_status_.status_message = "Initialization failed: " + std::string(ex.what());
        return false;
    }
}

bool MarketDataProcessor::start() {
    if (!initialized_.load()) {
        SPDLOG_ERROR("MarketDataProcessor not initialized");
        return false;
    }
    
    if (running_.load()) {
        SPDLOG_WARN("MarketDataProcessor already running");
        return true;
    }
    
    try {
        SPDLOG_INFO("Starting MarketDataProcessor...");
        
        // Start publisher first
        if (publisher_ && !publisher_->start(
            [this](const PublishedMessage& msg) { handle_published_message(msg); })) {
            SPDLOG_ERROR("Failed to start Level2Publisher");
            return false;
        }
        system_status_.publisher_running = true;
        
        // Start order book manager
        if (orderbook_manager_) {
            orderbook_manager_->start_publishing(
                [this](const Level2Snapshot& snapshot) { handle_level2_snapshot(snapshot); },
                [this](const Level2Delta& delta) { handle_level2_delta(delta); }
            );
        }
        system_status_.order_book_manager_running = true;
        
        // Start feed handler last
        if (feed_handler_ && !feed_handler_->start(
            [this](const OrderBookEvent& event) { handle_orderbook_event(event); })) {
            SPDLOG_ERROR("Failed to start PitchFeedHandler");
            return false;
        }
        system_status_.feed_handler_running = true;
        
        running_.store(true);
        
        // Start monitoring thread
        if (monitoring_enabled_.load()) {
            monitoring_thread_ = std::thread(&MarketDataProcessor::monitoring_thread_func, this);
        }
        
        system_status_.health = SystemHealth::HEALTHY;
        system_status_.status_message = "Running normally";
        
        SPDLOG_INFO("MarketDataProcessor started successfully");
        return true;
        
    } catch (const std::exception& ex) {
        SPDLOG_ERROR("MarketDataProcessor start failed: {}", ex.what());
        system_status_.health = SystemHealth::FAILED;
        system_status_.status_message = "Start failed: " + std::string(ex.what());
        error_count_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

void MarketDataProcessor::stop() {
    if (!running_.load()) {
        return;
    }
    
    SPDLOG_INFO("Stopping MarketDataProcessor...");
    running_.store(false);
    
    // Stop monitoring thread first
    if (monitoring_thread_.joinable()) {
        monitoring_thread_.join();
    }
    
    // Stop components in reverse order
    if (feed_handler_) {
        feed_handler_->stop();
        system_status_.feed_handler_running = false;
    }
    
    if (orderbook_manager_) {
        orderbook_manager_->stop_publishing();
        system_status_.order_book_manager_running = false;
    }
    
    if (publisher_) {
        publisher_->stop();
        system_status_.publisher_running = false;
    }
    
    system_status_.health = SystemHealth::HEALTHY;  // Stopped is healthy
    system_status_.status_message = "Stopped";
    
    SPDLOG_INFO("MarketDataProcessor stopped");
}

bool MarketDataProcessor::shutdown(int timeout_seconds) {
    if (!running_.load()) {
        return true;
    }
    
    SPDLOG_INFO("Initiating graceful shutdown with {}s timeout...", timeout_seconds);
    
    auto start_time = std::chrono::steady_clock::now();
    auto timeout_time = start_time + std::chrono::seconds(timeout_seconds);
    
    // Request stop
    stop();
    
    // Flush publisher if available
    if (publisher_) {
        SPDLOG_INFO("Flushing publisher...");
        auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            timeout_time - std::chrono::steady_clock::now()).count();
        
        if (remaining_ms > 0) {
            publisher_->flush(static_cast<int>(remaining_ms));
        }
    }
    
    // Check if shutdown completed within timeout
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time).count();
    
    bool success = elapsed < timeout_seconds;
    
    if (success) {
        SPDLOG_INFO("Graceful shutdown completed in {}s", elapsed);
    } else {
        SPDLOG_WARN("Graceful shutdown timeout after {}s", elapsed);
    }
    
    log_shutdown_info();
    return success;
}

bool MarketDataProcessor::add_symbol(const std::string& symbol, const std::string& exchange) {
    if (!symbol_manager_) {
        SPDLOG_ERROR("Symbol manager not available");
        return false;
    }
    
    bool added = symbol_manager_->add_symbol(symbol, exchange);
    if (added && orderbook_manager_) {
        orderbook_manager_->add_symbol(symbol);
        SPDLOG_INFO("Added symbol {} to processing", symbol);
    }
    
    return added;
}

bool MarketDataProcessor::remove_symbol(const std::string& symbol) {
    bool removed = false;
    
    if (orderbook_manager_) {
        removed = orderbook_manager_->remove_symbol(symbol);
    }
    
    if (symbol_manager_) {
        symbol_manager_->remove_symbol(symbol);
    }
    
    if (removed) {
        SPDLOG_INFO("Removed symbol {} from processing", symbol);
    }
    
    return removed;
}

std::vector<std::string> MarketDataProcessor::get_active_symbols() const {
    if (symbol_manager_) {
        return symbol_manager_->get_active_symbols();
    }
    return {};
}

std::pair<double, double> MarketDataProcessor::get_best_bid_ask(const std::string& symbol) const {
    if (orderbook_manager_) {
        return orderbook_manager_->get_best_bid_ask(symbol);
    }
    return {0.0, 0.0};
}

std::unique_ptr<Level2Snapshot> MarketDataProcessor::get_level2_snapshot(const std::string& symbol) const {
    if (orderbook_manager_) {
        return orderbook_manager_->get_snapshot(symbol);
    }
    return nullptr;
}

MarketDataProcessor::PerformanceStats MarketDataProcessor::get_performance_stats() const {
    PerformanceStats perf_stats;
    perf_stats.snapshot_time = std::chrono::steady_clock::now();
    
    // Get component statistics
    if (feed_handler_) {
        perf_stats.feed_stats = feed_handler_->get_statistics();
    }
    
    if (orderbook_manager_) {
        perf_stats.orderbook_stats = orderbook_manager_->get_statistics();
    }
    
    if (publisher_) {
        perf_stats.publisher_stats = publisher_->get_statistics();
    }
    
    // Calculate system-wide metrics
    perf_stats.total_messages_per_second = perf_stats.feed_stats.messages_per_second;
    perf_stats.avg_processing_latency_us = perf_stats.orderbook_stats.avg_processing_latency_us;
    perf_stats.max_processing_latency_us = perf_stats.orderbook_stats.max_processing_latency_us;
    
    // Estimate memory usage
    perf_stats.total_memory_usage_mb = calculate_memory_usage();
    
    // Calculate CPU usage
    perf_stats.cpu_usage_percent = calculate_cpu_usage();
    
    return perf_stats;
}

void MarketDataProcessor::reset_statistics() {
    if (feed_handler_) {
        feed_handler_->reset_statistics();
    }
    
    if (orderbook_manager_) {
        orderbook_manager_->reset_statistics();
    }
    
    if (publisher_) {
        publisher_->reset_statistics();
    }
    
    if (latency_tracker_) {
        latency_tracker_->reset_all();
    }
    
    error_count_.store(0, std::memory_order_relaxed);
    
    SPDLOG_INFO("All system statistics reset");
}

void MarketDataProcessor::set_monitoring_enabled(bool enabled) {
    monitoring_enabled_.store(enabled, std::memory_order_relaxed);
    SPDLOG_INFO("System monitoring {}", enabled ? "enabled" : "disabled");
}

bool MarketDataProcessor::initialize_components() {
    const auto& config = config_manager_->get_config();
    
    // Initialize latency tracker
    if (config.monitoring.latency_tracking.enabled) {
        LatencyTracker::Config latency_config;
        latency_config.percentiles = config.monitoring.latency_tracking.percentiles;
        latency_config.enable_detailed_tracking = true;
        
        latency_tracker_ = std::make_shared<LatencyTracker>(latency_config);
        SPDLOG_INFO("LatencyTracker initialized");
    }
    
    // Initialize symbol manager
    symbol_manager_ = std::make_shared<SymbolManager>();
    if (!config.orderbook.symbols.empty()) {
        symbol_manager_->load_symbols(config.orderbook.symbols);
    }
    SPDLOG_INFO("SymbolManager initialized with {} symbols", 
               symbol_manager_->get_symbol_count());
    
    // Initialize feed handler
    PitchFeedHandler::Config feed_config;
    feed_config.source_type = PitchFeedHandler::SourceType::UDP_MULTICAST;
    feed_config.multicast_group = config.feed.multicast_group;
    feed_config.multicast_port = config.feed.multicast_port;
    feed_config.interface = config.feed.interface;
    feed_config.buffer_size = config.feed.buffer_size;
    feed_config.unit_id = config.feed.pitch.unit_id;
    feed_config.gap_threshold = config.feed.pitch.sequence_number_gap_threshold;
    feed_config.heartbeat_interval_ms = config.feed.pitch.heartbeat_interval_ms;
    
    feed_handler_ = std::make_shared<PitchFeedHandler>(
        feed_config, symbol_manager_, latency_tracker_);
    SPDLOG_INFO("PitchFeedHandler initialized");
    
    // Initialize order book manager
    OrderBookManager::Config orderbook_config;
    orderbook_config.max_price_levels = config.orderbook.max_price_levels;
    orderbook_config.snapshot_interval_ms = config.orderbook.snapshot_interval_ms;
    orderbook_config.delta_interval_ms = config.orderbook.delta_interval_ms;
    orderbook_config.enable_trade_tracking = config.orderbook.keep_trade_history;
    orderbook_config.max_trade_history = config.orderbook.max_trade_history;
    orderbook_config.initial_book_capacity = config.orderbook.initial_capacity;
    orderbook_config.enable_price_validation = config.risk.price_validation.min_price > 0;
    orderbook_config.enable_quantity_validation = config.risk.quantity_validation.min_quantity > 0;
    orderbook_config.max_price_deviation_percent = config.risk.max_price_deviation_percent;
    
    orderbook_manager_ = std::make_shared<OrderBookManager>(
        orderbook_config, symbol_manager_, latency_tracker_);
    
    // Initialize order books for configured symbols
    if (!config.orderbook.symbols.empty()) {
        orderbook_manager_->initialize_symbols(config.orderbook.symbols);
    }
    SPDLOG_INFO("OrderBookManager initialized");
    
    // Initialize publisher
    if (config.publishing.enabled) {
        Level2Publisher::Config publisher_config;
        
        // Set enabled channels
        if (config.publishing.kafka.enabled) {
            publisher_config.enabled_channels.push_back(PublicationChannel::KAFKA);
        }
        if (config.publishing.shared_memory.enabled) {
            publisher_config.enabled_channels.push_back(PublicationChannel::SHARED_MEMORY);
        }
        
        // Set data formats
        for (const auto& format_str : config.publishing.formats) {
            if (format_str == "flatbuffers") {
                publisher_config.enabled_formats.push_back(DataFormat::FLATBUFFERS);
            } else if (format_str == "json") {
                publisher_config.enabled_formats.push_back(DataFormat::JSON);
            }
        }
        
        // Kafka configuration
        publisher_config.kafka.topic_prefix = config.publishing.kafka.topic_prefix;
        publisher_config.kafka.partition_strategy = config.publishing.kafka.partition_strategy;
        
        // Performance settings
        publisher_config.enable_batching = true;
        publisher_config.max_batch_size = config.kafka_cluster.batch_num_messages;
        publisher_config.max_batch_delay_ms = config.kafka_cluster.linger_ms;
        publisher_config.publisher_thread_count = 2;
        publisher_config.max_queue_size = config.kafka_cluster.queue_buffering_max_messages / 10;
        
        publisher_ = std::make_shared<Level2Publisher>(publisher_config, latency_tracker_);
        SPDLOG_INFO("Level2Publisher initialized");
    }
    
    return true;
}

bool MarketDataProcessor::setup_component_connections() {
    // Component connections are set up through callbacks in the start() method
    // This method could be used for additional connection setup if needed
    return true;
}

void MarketDataProcessor::handle_orderbook_event(const OrderBookEvent& event) {
    if (latency_tracker_) {
        LATENCY_TRACK_SCOPE(latency_tracker_.get(), "md_processor_event_handling");
    }
    
    try {
        if (orderbook_manager_) {
            orderbook_manager_->process_event(event);
        }
    } catch (const std::exception& ex) {
        handle_error("OrderBookManager", "Event processing failed: " + std::string(ex.what()));
    }
}

void MarketDataProcessor::handle_level2_snapshot(const Level2Snapshot& snapshot) {
    if (latency_tracker_) {
        LATENCY_TRACK_SCOPE(latency_tracker_.get(), "md_processor_snapshot_handling");
    }
    
    try {
        if (publisher_) {
            publisher_->publish_snapshot(snapshot);
        }
    } catch (const std::exception& ex) {
        handle_error("Level2Publisher", "Snapshot publishing failed: " + std::string(ex.what()));
    }
}

void MarketDataProcessor::handle_level2_delta(const Level2Delta& delta) {
    if (latency_tracker_) {
        LATENCY_TRACK_SCOPE(latency_tracker_.get(), "md_processor_delta_handling");
    }
    
    try {
        if (publisher_) {
            publisher_->publish_delta(delta);
        }
    } catch (const std::exception& ex) {
        handle_error("Level2Publisher", "Delta publishing failed: " + std::string(ex.what()));
    }
}

void MarketDataProcessor::handle_published_message(const PublishedMessage& message) {
    // This is called after successful publishing
    // Could be used for additional post-processing or metrics
    SPDLOG_TRACE("Published message to {}: {} bytes", 
                static_cast<int>(message.channel), message.data.size());
}

void MarketDataProcessor::monitoring_thread_func() {
    SPDLOG_INFO("System monitoring thread started");
    
    const auto& config = config_manager_->get_config();
    auto interval = std::chrono::milliseconds(config.monitoring.stats_interval_ms);
    
    while (running_.load()) {
        std::this_thread::sleep_for(interval);
        
        if (!running_.load()) {
            break;
        }
        
        try {
            update_system_status();
            
            // Check for memory alerts
            if (config.monitoring.memory_monitoring.enabled) {
                size_t memory_mb = calculate_memory_usage();
                if (memory_mb > static_cast<size_t>(config.monitoring.memory_monitoring.alert_threshold_mb)) {
                    SPDLOG_WARN("High memory usage detected: {} MB", memory_mb);
                }
            }
            
        } catch (const std::exception& ex) {
            SPDLOG_ERROR("Error in monitoring thread: {}", ex.what());
        }
    }
    
    SPDLOG_INFO("System monitoring thread stopped");
}

void MarketDataProcessor::update_system_status() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    
    // Update component status
    system_status_.feed_handler_running = feed_handler_ && feed_handler_->is_running();
    system_status_.order_book_manager_running = orderbook_manager_ != nullptr;
    system_status_.publisher_running = publisher_ && publisher_->is_running();
    
    // Calculate performance metrics
    auto perf_stats = get_performance_stats();
    system_status_.total_throughput_msg_per_sec = perf_stats.total_messages_per_second;
    system_status_.avg_end_to_end_latency_us = perf_stats.avg_processing_latency_us;
    system_status_.max_end_to_end_latency_us = perf_stats.max_processing_latency_us;
    
    // Update memory usage
    system_status_.estimated_memory_usage_mb = calculate_memory_usage();
    
    // Update error counts
    system_status_.total_errors = error_count_.load(std::memory_order_relaxed);
    if (feed_handler_) {
        system_status_.feed_errors = feed_handler_->get_statistics().total_parsing_errors;
    }
    if (orderbook_manager_) {
        system_status_.processing_errors = 0;  // Would need to track these separately
    }
    
    // Update uptime
    auto now = std::chrono::steady_clock::now();
    system_status_.uptime_seconds = std::chrono::duration_cast<std::chrono::duration<double>>(
        now - system_status_.start_time);
    
    // Assess system health
    system_status_.health = assess_system_health();
    
    system_status_.last_update_time = now;
}

SystemHealth MarketDataProcessor::assess_system_health() const {
    // Check if all components are running
    if (running_.load()) {
        if (!system_status_.feed_handler_running ||
            !system_status_.order_book_manager_running ||
            !system_status_.publisher_running) {
            return SystemHealth::CRITICAL;
        }
    }
    
    // Check error rates
    uint64_t total_errors = error_count_.load(std::memory_order_relaxed);
    if (total_errors > 1000) {  // Configurable threshold
        return SystemHealth::DEGRADED;
    }
    
    // Check memory usage
    if (system_status_.estimated_memory_usage_mb > 2048) {  // 2GB threshold
        return SystemHealth::DEGRADED;
    }
    
    // Check latency
    if (system_status_.avg_end_to_end_latency_us > 10000) {  // 10ms threshold
        return SystemHealth::DEGRADED;
    }
    
    return SystemHealth::HEALTHY;
}

void MarketDataProcessor::handle_error(const std::string& component, const std::string& error_message) {
    error_count_.fetch_add(1, std::memory_order_relaxed);
    last_error_time_ = std::chrono::steady_clock::now();
    
    SPDLOG_ERROR("[{}] {}", component, error_message);
    
    // Update system status
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        system_status_.status_message = "Error in " + component + ": " + error_message;
        
        if (component == "PitchFeedHandler") {
            system_status_.feed_errors++;
        } else if (component == "OrderBookManager") {
            system_status_.processing_errors++;
        } else if (component == "Level2Publisher") {
            system_status_.publishing_errors++;
        }
    }
}

size_t MarketDataProcessor::calculate_memory_usage() const {
    // Rough estimation of memory usage
    size_t total_mb = 0;
    
    // Base system components
    total_mb += 50;  // Base system
    
    // Symbol manager
    if (symbol_manager_) {
        total_mb += symbol_manager_->get_symbol_count() * 1;  // ~1KB per symbol
    }
    
    // Order books
    if (orderbook_manager_) {
        auto symbols = orderbook_manager_->get_active_symbols();
        total_mb += symbols.size() * 10;  // ~10MB per order book (rough estimate)
    }
    
    // Latency tracker
    if (latency_tracker_) {
        auto sys_stats = latency_tracker_->get_system_stats();
        total_mb += sys_stats.memory_usage_bytes / (1024 * 1024);
    }
    
    return total_mb;
}

double MarketDataProcessor::calculate_cpu_usage() const {
    // TODO: Implement actual CPU usage calculation
    // This would require system-specific code to read /proc/stat or similar
    return 0.0;
}

void MarketDataProcessor::cleanup() {
    // Components are automatically cleaned up by their destructors
    SPDLOG_DEBUG("MarketDataProcessor cleanup completed");
}

bool MarketDataProcessor::validate_configuration() const {
    try {
        config_manager_->validate_config();
        return true;
    } catch (const std::exception& ex) {
        SPDLOG_ERROR("Configuration validation failed: {}", ex.what());
        return false;
    }
}

void MarketDataProcessor::log_startup_info() const {
    const auto& config = config_manager_->get_config();
    
    SPDLOG_INFO("=== Market Data Processor Startup ===");
    SPDLOG_INFO("Feed Source: {}", config.feed.source_type);
    SPDLOG_INFO("Multicast: {}:{}", config.feed.multicast_group, config.feed.multicast_port);
    SPDLOG_INFO("Unit ID: {}", config.feed.pitch.unit_id);
    SPDLOG_INFO("Symbols: {}", config.orderbook.symbols.size());
    SPDLOG_INFO("Publishing Enabled: {}", config.publishing.enabled);
    SPDLOG_INFO("Kafka Bootstrap: {}", config.kafka_cluster.bootstrap_servers);
    SPDLOG_INFO("=====================================");
}

void MarketDataProcessor::log_shutdown_info() const {
    auto final_stats = get_performance_stats();
    
    SPDLOG_INFO("=== Market Data Processor Shutdown ===");
    SPDLOG_INFO("Total Runtime: {:.1f} seconds", system_status_.uptime_seconds.count());
    SPDLOG_INFO("Messages Processed: {}", final_stats.feed_stats.total_messages_processed);
    SPDLOG_INFO("Events Processed: {}", final_stats.orderbook_stats.total_events_processed);
    SPDLOG_INFO("Messages Published: {}", final_stats.publisher_stats.total_messages_published);
    SPDLOG_INFO("Total Errors: {}", error_count_.load());
    SPDLOG_INFO("======================================");
}

} // namespace md_l2