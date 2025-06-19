#include "orderbook/OrderBookManager.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace md_l2 {

OrderBookManager::OrderBookManager(const Config& config,
                                   std::shared_ptr<SymbolManager> symbol_manager,
                                   std::shared_ptr<LatencyTracker> latency_tracker)
    : config_(config), symbol_manager_(symbol_manager), latency_tracker_(latency_tracker) {
    
    start_time_ = std::chrono::steady_clock::now();
    SPDLOG_INFO("OrderBookManager initialized with {} max price levels", config_.max_price_levels);
}

OrderBookManager::~OrderBookManager() {
    stop_publishing();
    clear_all();
}

size_t OrderBookManager::initialize_symbols(const std::vector<std::string>& symbols) {
    size_t created_count = 0;
    
    for (const auto& symbol : symbols) {
        if (add_symbol(symbol)) {
            created_count++;
        }
    }
    
    SPDLOG_INFO("Initialized {} order books for {} symbols", created_count, symbols.size());
    return created_count;
}

bool OrderBookManager::add_symbol(const std::string& symbol) {
    std::unique_lock lock(books_mutex_);
    
    if (order_books_.find(symbol) != order_books_.end()) {
        SPDLOG_DEBUG("Order book for symbol {} already exists", symbol);
        return false;
    }
    
    // Verify symbol exists in symbol manager
    if (symbol_manager_ && !symbol_manager_->has_symbol(symbol)) {
        SPDLOG_WARN("Symbol {} not found in symbol manager", symbol);
        return false;
    }
    
    auto order_book = std::make_unique<LockFreeOrderBook>();
    order_books_[symbol] = std::move(order_book);
    
    SPDLOG_INFO("Created order book for symbol: {}", symbol);
    return true;
}

bool OrderBookManager::remove_symbol(const std::string& symbol) {
    std::unique_lock lock(books_mutex_);
    
    auto it = order_books_.find(symbol);
    if (it == order_books_.end()) {
        SPDLOG_DEBUG("Order book for symbol {} not found for removal", symbol);
        return false;
    }
    
    order_books_.erase(it);
    SPDLOG_INFO("Removed order book for symbol: {}", symbol);
    return true;
}

void OrderBookManager::process_event(const OrderBookEvent& event) {
    if (latency_tracker_) {
        LATENCY_TRACK_SCOPE(latency_tracker_.get(), "orderbook_event_processing");
    }
    
    // Validate event
    if (!validate_event(event)) {
        return;
    }
    
    // Process based on event type
    switch (event.type) {
        case OrderBookEventType::ADD_ORDER:
            process_add_order(event);
            break;
        case OrderBookEventType::MODIFY_ORDER:
            process_modify_order(event);
            break;
        case OrderBookEventType::DELETE_ORDER:
            process_delete_order(event);
            break;
        case OrderBookEventType::TRADE:
            process_trade(event);
            break;
        case OrderBookEventType::TRADING_STATUS_CHANGE:
            process_trading_status(event);
            break;
        case OrderBookEventType::UNIT_CLEAR:
            // Clear all order books
            clear_all();
            break;
    }
    
    update_statistics(event);
}

void OrderBookManager::process_add_order(const OrderBookEvent& event) {
    auto* book = get_or_create_book(event.symbol);
    if (!book) {
        SPDLOG_ERROR("Failed to get order book for symbol: {}", event.symbol);
        return;
    }
    
    OrderSide side = (event.side == 'B') ? eS_Buy : eS_Sell;
    
    // Scale price to integer if symbol manager available
    uint64_t scaled_price = static_cast<uint64_t>(event.price * 10000000);  // Default 7 decimal places
    if (symbol_manager_) {
        auto symbol_info = symbol_manager_->get_symbol_info(event.symbol);
        if (symbol_info) {
            scaled_price = symbol_info->scale_price(event.price);
        }
    }
    
    auto* order = book->addOrder(event.order_id, scaled_price, event.quantity, side);
    if (!order) {
        SPDLOG_DEBUG("Failed to add order {} for symbol {}", event.order_id, event.symbol);
        return;
    }
    
    // Generate delta if callback is set
    if (delta_callback_) {
        auto delta = create_delta(event);
        if (delta) {
            delta_callback_(*delta);
        }
    }
    
    SPDLOG_TRACE("Added order {} for symbol {} at price {} qty {}", 
                event.order_id, event.symbol, event.price, event.quantity);
}

void OrderBookManager::process_modify_order(const OrderBookEvent& event) {
    // For modify orders, we need to find the symbol since PITCH modify messages don't include it
    // This would require maintaining an order ID to symbol mapping
    // For now, we'll skip if symbol is empty
    if (event.symbol.empty()) {
        SPDLOG_DEBUG("Modify order {} skipped - no symbol information", event.order_id);
        return;
    }
    
    auto* book = get_or_create_book(event.symbol);
    if (!book) {
        return;
    }
    
    // Scale price
    uint64_t scaled_price = static_cast<uint64_t>(event.price * 10000000);
    if (symbol_manager_) {
        auto symbol_info = symbol_manager_->get_symbol_info(event.symbol);
        if (symbol_info) {
            scaled_price = symbol_info->scale_price(event.price);
        }
    }
    
    bool success = book->modifyOrder(event.order_id, scaled_price, event.quantity);
    if (!success) {
        SPDLOG_DEBUG("Failed to modify order {} for symbol {}", event.order_id, event.symbol);
        return;
    }
    
    // Generate delta
    if (delta_callback_) {
        auto delta = create_delta(event);
        if (delta) {
            delta_callback_(*delta);
        }
    }
    
    SPDLOG_TRACE("Modified order {} for symbol {}", event.order_id, event.symbol);
}

void OrderBookManager::process_delete_order(const OrderBookEvent& event) {
    // Similar to modify, delete orders may not have symbol information
    if (event.symbol.empty()) {
        SPDLOG_DEBUG("Delete order {} skipped - no symbol information", event.order_id);
        return;
    }
    
    auto* book = get_or_create_book(event.symbol);
    if (!book) {
        return;
    }
    
    bool success = book->removeOrder(event.order_id);
    if (!success) {
        SPDLOG_DEBUG("Failed to delete order {} for symbol {}", event.order_id, event.symbol);
        return;
    }
    
    // Generate delta
    if (delta_callback_) {
        auto delta = create_delta(event);
        if (delta) {
            delta_callback_(*delta);
        }
    }
    
    SPDLOG_TRACE("Deleted order {} for symbol {}", event.order_id, event.symbol);
}

void OrderBookManager::process_trade(const OrderBookEvent& event) {
    auto* book = get_or_create_book(event.symbol);
    if (!book) {
        return;
    }
    
    // Scale trade price
    uint64_t scaled_price = event.trade_price_scaled;
    if (scaled_price == 0 && symbol_manager_) {
        auto symbol_info = symbol_manager_->get_symbol_info(event.symbol);
        if (symbol_info) {
            scaled_price = symbol_info->scale_price(event.price);
        }
    }
    
    LockFreeOrderBook::TradeMessage trade_msg;
    trade_msg.trade_price_ = scaled_price;
    trade_msg.trade_qty_ = event.trade_quantity;
    
    book->handleTrade(trade_msg);
    
    // Generate delta
    if (delta_callback_) {
        auto delta = create_delta(event);
        if (delta) {
            delta_callback_(*delta);
        }
    }
    
    SPDLOG_TRACE("Processed trade for symbol {} price {} qty {}", 
                event.symbol, event.price, event.trade_quantity);
}

void OrderBookManager::process_trading_status(const OrderBookEvent& event) {
    // Trading status changes don't directly affect the order book structure
    // but could be used to pause/resume processing or generate notifications
    
    SPDLOG_INFO("Trading status change for symbol {}: {}", 
               event.symbol, event.trading_status);
    
    // Generate delta for status change
    if (delta_callback_) {
        auto delta = create_delta(event);
        if (delta) {
            delta_callback_(*delta);
        }
    }
}

void OrderBookManager::start_publishing(Level2SnapshotCallback snapshot_callback,
                                       Level2DeltaCallback delta_callback) {
    if (publishing_active_.load()) {
        SPDLOG_WARN("Publishing is already active");
        return;
    }
    
    snapshot_callback_ = snapshot_callback;
    delta_callback_ = delta_callback;
    
    publishing_active_.store(true);
    
    // Start snapshot publishing thread
    snapshot_thread_ = std::thread(&OrderBookManager::snapshot_thread_func, this);
    
    SPDLOG_INFO("Order book publishing started - Snapshot interval: {}ms, Delta interval: {}ms",
               config_.snapshot_interval_ms, config_.delta_interval_ms);
}

void OrderBookManager::stop_publishing() {
    if (!publishing_active_.load()) {
        return;
    }
    
    SPDLOG_INFO("Stopping order book publishing...");
    publishing_active_.store(false);
    
    if (snapshot_thread_.joinable()) {
        snapshot_thread_.join();
    }
    
    if (delta_thread_.joinable()) {
        delta_thread_.join();
    }
    
    SPDLOG_INFO("Order book publishing stopped");
}

std::unique_ptr<Level2Snapshot> OrderBookManager::get_snapshot(const std::string& symbol) const {
    std::shared_lock lock(books_mutex_);
    
    auto it = order_books_.find(symbol);
    if (it == order_books_.end()) {
        return nullptr;
    }
    
    auto book_data = it->second->snapshot();
    return create_snapshot(symbol, book_data);
}

std::pair<double, double> OrderBookManager::get_best_bid_ask(const std::string& symbol) const {
    auto snapshot = get_snapshot(symbol);
    if (!snapshot) {
        return {0.0, 0.0};
    }
    
    return {snapshot->best_bid, snapshot->best_ask};
}

std::vector<std::string> OrderBookManager::get_active_symbols() const {
    std::shared_lock lock(books_mutex_);
    
    std::vector<std::string> symbols;
    symbols.reserve(order_books_.size());
    
    for (const auto& [symbol, book] : order_books_) {
        symbols.push_back(symbol);
    }
    
    return symbols;
}

void OrderBookManager::clear_all() {
    std::unique_lock lock(books_mutex_);
    
    size_t count = order_books_.size();
    
    for (auto& [symbol, book] : order_books_) {
        book->clear();
    }
    
    order_books_.clear();
    
    SPDLOG_INFO("Cleared all {} order books", count);
}

void OrderBookManager::reset_statistics() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = Statistics{};
    start_time_ = std::chrono::steady_clock::now();
    
    SPDLOG_INFO("OrderBookManager statistics reset");
}

LockFreeOrderBook* OrderBookManager::get_or_create_book(const std::string& symbol) {
    if (symbol.empty()) {
        return nullptr;
    }
    
    // Try with read lock first
    {
        std::shared_lock lock(books_mutex_);
        auto it = order_books_.find(symbol);
        if (it != order_books_.end()) {
            return it->second.get();
        }
    }
    
    // Create new book with write lock
    std::unique_lock lock(books_mutex_);
    
    // Check again after acquiring write lock
    auto it = order_books_.find(symbol);
    if (it != order_books_.end()) {
        return it->second.get();
    }
    
    // Verify symbol if symbol manager is available
    if (symbol_manager_ && !symbol_manager_->has_symbol(symbol)) {
        SPDLOG_DEBUG("Cannot create order book for unknown symbol: {}", symbol);
        return nullptr;
    }
    
    auto order_book = std::make_unique<LockFreeOrderBook>();
    auto* book_ptr = order_book.get();
    order_books_[symbol] = std::move(order_book);
    
    SPDLOG_INFO("Created order book for new symbol: {}", symbol);
    return book_ptr;
}

std::unique_ptr<Level2Snapshot> OrderBookManager::create_snapshot(
    const std::string& symbol,
    const std::shared_ptr<const LockFreeOrderBookData>& book_data) const {
    
    auto snapshot = std::make_unique<Level2Snapshot>(symbol);
    snapshot->sequence_number = book_data->cdc_seq;
    snapshot->last_trade_price = static_cast<double>(book_data->recent_trade_price) / 10000000.0;
    snapshot->last_trade_quantity = book_data->recent_trade_qty;
    
    // Unscale prices if symbol manager is available
    double price_divisor = 10000000.0;  // Default
    if (symbol_manager_) {
        auto symbol_info = symbol_manager_->get_symbol_info(symbol);
        if (symbol_info) {
            price_divisor = static_cast<double>(symbol_info->price_multiplier);
        }
    }
    
    // Convert buy side (reverse order for best-to-worst)
    for (auto it = book_data->buy_side.rbegin(); it != book_data->buy_side.rend(); ++it) {
        if (it->second && it->second->getTotalQty() > 0) {
            double price = static_cast<double>(it->first) / price_divisor;
            uint32_t quantity = it->second->getTotalQty();
            snapshot->buy_levels.emplace_back(price, quantity);
            snapshot->total_buy_volume += quantity;
            
            // Count orders
            for (const Order* order = it->second->getHead(); order; order = order->next_) {
                snapshot->total_buy_orders++;
            }
        }
        
        if (snapshot->buy_levels.size() >= static_cast<size_t>(config_.max_price_levels)) {
            break;
        }
    }
    
    // Convert sell side (natural order for best-to-worst)
    for (const auto& [price, order_list] : book_data->sell_side) {
        if (order_list && order_list->getTotalQty() > 0) {
            double converted_price = static_cast<double>(price) / price_divisor;
            uint32_t quantity = order_list->getTotalQty();
            snapshot->sell_levels.emplace_back(converted_price, quantity);
            snapshot->total_sell_volume += quantity;
            
            // Count orders
            for (const Order* order = order_list->getHead(); order; order = order->next_) {
                snapshot->total_sell_orders++;
            }
        }
        
        if (snapshot->sell_levels.size() >= static_cast<size_t>(config_.max_price_levels)) {
            break;
        }
    }
    
    // Calculate spread and mid price
    calculate_spread_info(*snapshot);
    
    return snapshot;
}

std::unique_ptr<Level2Delta> OrderBookManager::create_delta(const OrderBookEvent& event) const {
    Level2Delta::ChangeType change_type;
    
    switch (event.type) {
        case OrderBookEventType::ADD_ORDER:
            change_type = Level2Delta::ChangeType::ADD_ORDER;
            break;
        case OrderBookEventType::MODIFY_ORDER:
            change_type = Level2Delta::ChangeType::MODIFY_ORDER;
            break;
        case OrderBookEventType::DELETE_ORDER:
            change_type = Level2Delta::ChangeType::DELETE_ORDER;
            break;
        case OrderBookEventType::TRADE:
            change_type = Level2Delta::ChangeType::TRADE;
            break;
        default:
            return nullptr;
    }
    
    auto delta = std::make_unique<Level2Delta>(event.symbol, change_type);
    delta->sequence_number = event.sequence_number;
    delta->order_id = event.order_id;
    delta->price = event.price;
    delta->quantity = event.quantity;
    delta->side = event.side;
    delta->execution_id = event.execution_id;
    delta->is_aggressive = event.is_aggressive_order;
    
    return delta;
}

void OrderBookManager::snapshot_thread_func() {
    SPDLOG_INFO("Snapshot publishing thread started");
    
    auto next_snapshot_time = std::chrono::steady_clock::now();
    
    while (publishing_active_.load()) {
        next_snapshot_time += std::chrono::milliseconds(config_.snapshot_interval_ms);
        std::this_thread::sleep_until(next_snapshot_time);
        
        if (!publishing_active_.load()) {
            break;
        }
        
        // Get all symbols and publish snapshots
        auto symbols = get_active_symbols();
        
        for (const auto& symbol : symbols) {
            try {
                auto snapshot = get_snapshot(symbol);
                if (snapshot && snapshot_callback_) {
                    snapshot_callback_(*snapshot);
                    
                    std::lock_guard<std::mutex> lock(stats_mutex_);
                    stats_.snapshots_published++;
                }
            } catch (const std::exception& ex) {
                SPDLOG_ERROR("Error publishing snapshot for symbol {}: {}", symbol, ex.what());
            }
        }
    }
    
    SPDLOG_INFO("Snapshot publishing thread stopped");
}

void OrderBookManager::delta_thread_func() {
    SPDLOG_INFO("Delta publishing thread started");
    
    // Note: Delta publishing is event-driven through the delta_callback_
    // This thread would handle batched delta publishing if needed
    
    SPDLOG_INFO("Delta publishing thread stopped");
}

bool OrderBookManager::validate_event(const OrderBookEvent& event) const {
    // Basic validation
    if (event.symbol.empty() && 
        event.type != OrderBookEventType::UNIT_CLEAR &&
        event.type != OrderBookEventType::DELETE_ORDER &&
        event.type != OrderBookEventType::MODIFY_ORDER) {
        SPDLOG_DEBUG("Event validation failed: empty symbol");
        return false;
    }
    
    if (event.type == OrderBookEventType::ADD_ORDER ||
        event.type == OrderBookEventType::MODIFY_ORDER) {
        if (event.price <= 0.0 || event.quantity == 0) {
            SPDLOG_DEBUG("Event validation failed: invalid price or quantity");
            return false;
        }
        
        if (event.side != 'B' && event.side != 'S') {
            SPDLOG_DEBUG("Event validation failed: invalid side: {}", event.side);
            return false;
        }
    }
    
    // Symbol manager validation
    if (config_.enable_price_validation && symbol_manager_ && !event.symbol.empty()) {
        if (!symbol_manager_->validate_price(event.symbol, event.price)) {
            SPDLOG_DEBUG("Event validation failed: price out of range for symbol {}", event.symbol);
            return false;
        }
        
        if (!symbol_manager_->validate_quantity(event.symbol, event.quantity)) {
            SPDLOG_DEBUG("Event validation failed: quantity out of range for symbol {}", event.symbol);
            return false;
        }
    }
    
    return true;
}

void OrderBookManager::update_statistics(const OrderBookEvent& event) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    stats_.total_events_processed++;
    
    switch (event.type) {
        case OrderBookEventType::ADD_ORDER:
            stats_.total_add_orders++;
            break;
        case OrderBookEventType::MODIFY_ORDER:
            stats_.total_modify_orders++;
            break;
        case OrderBookEventType::DELETE_ORDER:
            stats_.total_delete_orders++;
            break;
        case OrderBookEventType::TRADE:
            stats_.total_trades++;
            break;
    }
    
    // Calculate processing latency
    auto processing_latency = std::chrono::duration_cast<std::chrono::microseconds>(
        event.process_time - event.receive_time).count();
    
    if (processing_latency > 0) {
        stats_.max_processing_latency_us = std::max(
            stats_.max_processing_latency_us, static_cast<double>(processing_latency));
        
        // Update average
        if (stats_.total_events_processed > 0) {
            stats_.avg_processing_latency_us = 
                (stats_.avg_processing_latency_us * (stats_.total_events_processed - 1) + 
                 processing_latency) / stats_.total_events_processed;
        }
    }
    
    // Calculate events per second
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
    if (duration > 0) {
        stats_.events_per_second = static_cast<double>(stats_.total_events_processed) / duration;
    }
    
    // Estimate memory usage
    stats_.total_symbols = order_books_.size();
    stats_.estimated_memory_usage_mb = (order_books_.size() * sizeof(LockFreeOrderBook) + 
                                       stats_.total_orders_in_memory * sizeof(Order)) / (1024 * 1024);
    
    stats_.last_update_time = now;
}

void OrderBookManager::calculate_spread_info(Level2Snapshot& snapshot) const {
    if (!snapshot.buy_levels.empty()) {
        snapshot.best_bid = snapshot.buy_levels.front().first;
    }
    
    if (!snapshot.sell_levels.empty()) {
        snapshot.best_ask = snapshot.sell_levels.front().first;
    }
    
    if (snapshot.best_bid > 0.0 && snapshot.best_ask > 0.0) {
        snapshot.spread = snapshot.best_ask - snapshot.best_bid;
        snapshot.mid_price = (snapshot.best_bid + snapshot.best_ask) / 2.0;
    }
}

void OrderBookManager::calculate_book_stats(Level2Snapshot& snapshot,
                                           const std::shared_ptr<const LockFreeOrderBookData>& book_data) const {
    // This function could be extended to calculate additional book statistics
    // such as volume-weighted average price, order count at each level, etc.
}

} // namespace md_l2