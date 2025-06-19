#include "feed/PitchFeedHandler.hpp"
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

namespace md_l2 {

// Gap Handler Implementation
GapHandler::GapHandler(int unit_id, int gap_threshold)
    : unit_id_(unit_id), gap_threshold_(gap_threshold) {
    SPDLOG_DEBUG("GapHandler initialized for unit {} with threshold {}", unit_id_, gap_threshold_);
}

bool GapHandler::check_gap(uint32_t sequence_number) {
    if (!initialized_) {
        expected_sequence_ = sequence_number + 1;
        initialized_ = true;
        return false;
    }
    
    if (sequence_number == expected_sequence_) {
        expected_sequence_++;
        return false;  // No gap
    }
    
    if (sequence_number > expected_sequence_) {
        // Gap detected
        uint32_t gap_size = sequence_number - expected_sequence_;
        
        if (gap_size >= static_cast<uint32_t>(gap_threshold_)) {
            stats_.total_gaps++;
            stats_.total_messages_lost += gap_size;
            stats_.last_gap_size = gap_size;
            stats_.last_gap_time = std::chrono::steady_clock::now();
            
            SPDLOG_WARN("Gap detected in unit {}: expected {}, received {}, gap size: {}", 
                       unit_id_, expected_sequence_, sequence_number, gap_size);
            
            expected_sequence_ = sequence_number + 1;
            return true;
        }
    }
    
    // Handle out-of-order or duplicate messages
    if (sequence_number < expected_sequence_) {
        SPDLOG_DEBUG("Out-of-order message in unit {}: received {}, expected {}", 
                    unit_id_, sequence_number, expected_sequence_);
    }
    
    expected_sequence_ = sequence_number + 1;
    return false;
}

void GapHandler::reset() {
    expected_sequence_ = 0;
    initialized_ = false;
    stats_ = GapStats{};
    SPDLOG_INFO("GapHandler reset for unit {}", unit_id_);
}

// PitchFeedHandler Implementation
PitchFeedHandler::PitchFeedHandler(const Config& config,
                                   std::shared_ptr<SymbolManager> symbol_manager,
                                   std::shared_ptr<LatencyTracker> latency_tracker)
    : config_(config), symbol_manager_(symbol_manager), latency_tracker_(latency_tracker) {
    
    gap_handler_ = std::make_unique<GapHandler>(config_.unit_id, config_.gap_threshold);
    start_time_ = std::chrono::steady_clock::now();
    
    SPDLOG_INFO("PitchFeedHandler initialized - Source: {}, Unit: {}", 
               static_cast<int>(config_.source_type), config_.unit_id);
}

PitchFeedHandler::~PitchFeedHandler() {
    stop();
    cleanup();
}

bool PitchFeedHandler::start(OrderBookEventCallback event_callback) {
    if (running_.load()) {
        SPDLOG_WARN("PitchFeedHandler is already running");
        return false;
    }
    
    event_callback_ = event_callback;
    
    if (!event_callback_) {
        SPDLOG_ERROR("Event callback is required");
        return false;
    }
    
    // Setup source-specific resources
    switch (config_.source_type) {
        case SourceType::UDP_MULTICAST:
            if (!setup_udp_socket()) {
                SPDLOG_ERROR("Failed to setup UDP multicast socket");
                return false;
            }
            break;
        case SourceType::FILE_REPLAY:
            // TODO: Implement file replay setup
            SPDLOG_ERROR("File replay not yet implemented");
            return false;
        case SourceType::TCP_STREAM:
            // TODO: Implement TCP stream setup
            SPDLOG_ERROR("TCP stream not yet implemented");
            return false;
    }
    
    running_.store(true);
    
    // Start threads
    receiver_thread_ = std::thread(&PitchFeedHandler::receiver_thread_func, this);
    processor_thread_ = std::thread(&PitchFeedHandler::processor_thread_func, this);
    
    SPDLOG_INFO("PitchFeedHandler started successfully");
    return true;
}

void PitchFeedHandler::stop() {
    if (!running_.load()) {
        return;
    }
    
    SPDLOG_INFO("Stopping PitchFeedHandler...");
    running_.store(false);
    
    // Wake up processor thread
    queue_cv_.notify_all();
    
    // Join threads
    if (receiver_thread_.joinable()) {
        receiver_thread_.join();
    }
    
    if (processor_thread_.joinable()) {
        processor_thread_.join();
    }
    
    SPDLOG_INFO("PitchFeedHandler stopped");
}

bool PitchFeedHandler::setup_udp_socket() {
    // Create UDP socket
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        SPDLOG_ERROR("Failed to create UDP socket: {}", strerror(errno));
        return false;
    }
    
    // Set socket options
    int reuse = 1;
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        SPDLOG_WARN("Failed to set SO_REUSEADDR: {}", strerror(errno));
    }
    
    // Set receive buffer size
    int buffer_size = config_.buffer_size;
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) < 0) {
        SPDLOG_WARN("Failed to set receive buffer size: {}", strerror(errno));
    }
    
    // Bind to multicast address
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.multicast_port);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(socket_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        SPDLOG_ERROR("Failed to bind socket: {}", strerror(errno));
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }
    
    // Join multicast group
    struct ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(config_.multicast_group.c_str());
    mreq.imr_interface.s_addr = INADDR_ANY;
    
    if (setsockopt(socket_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        SPDLOG_ERROR("Failed to join multicast group {}: {}", 
                    config_.multicast_group, strerror(errno));
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }
    
    SPDLOG_INFO("UDP multicast socket setup complete - Group: {}:{}", 
               config_.multicast_group, config_.multicast_port);
    
    return true;
}

void PitchFeedHandler::receiver_thread_func() {
    SPDLOG_INFO("Receiver thread started");
    
    std::vector<uint8_t> buffer(config_.buffer_size);
    struct pollfd pfd{};
    pfd.fd = socket_fd_;
    pfd.events = POLLIN;
    
    while (running_.load()) {
        // Poll with timeout
        int poll_result = poll(&pfd, 1, 100);  // 100ms timeout
        
        if (poll_result < 0) {
            if (errno != EINTR) {
                SPDLOG_ERROR("Poll error: {}", strerror(errno));
            }
            continue;
        }
        
        if (poll_result == 0) {
            // Timeout - continue loop
            continue;
        }
        
        if (!(pfd.revents & POLLIN)) {
            continue;
        }
        
        // Receive data
        ssize_t received = recv(socket_fd_, buffer.data(), buffer.size(), 0);
        
        if (received < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                SPDLOG_ERROR("Receive error: {}", strerror(errno));
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.total_messages_dropped++;
            }
            continue;
        }
        
        if (received == 0) {
            continue;
        }
        
        // Update statistics
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.total_messages_received++;
            stats_.total_bytes_received += received;
        }
        
        // Create message buffer and enqueue
        std::vector<uint8_t> message_buffer(buffer.begin(), buffer.begin() + received);
        
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            
            if (message_queue_.size() >= MAX_QUEUE_SIZE) {
                // Queue is full, drop oldest message
                message_queue_.pop();
                std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                stats_.total_messages_dropped++;
            }
            
            message_queue_.push(std::move(message_buffer));
        }
        
        queue_cv_.notify_one();
    }
    
    SPDLOG_INFO("Receiver thread stopped");
}

void PitchFeedHandler::processor_thread_func() {
    SPDLOG_INFO("Processor thread started");
    
    while (running_.load()) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        
        // Wait for messages or shutdown
        queue_cv_.wait(lock, [this] { 
            return !message_queue_.empty() || !running_.load(); 
        });
        
        if (!running_.load() && message_queue_.empty()) {
            break;
        }
        
        // Process all available messages
        while (!message_queue_.empty()) {
            auto message_buffer = std::move(message_queue_.front());
            message_queue_.pop();
            lock.unlock();
            
            // Process the message buffer
            try {
                int events_generated = process_message_buffer(message_buffer);
                
                std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                stats_.total_messages_processed++;
                
            } catch (const std::exception& ex) {
                SPDLOG_ERROR("Error processing message: {}", ex.what());
                std::lock_guard<std::mutex> stats_lock(stats_mutex_);
                stats_.total_parsing_errors++;
            }
            
            lock.lock();
        }
    }
    
    SPDLOG_INFO("Processor thread stopped");
}

int PitchFeedHandler::process_message_buffer(const std::vector<uint8_t>& buffer) {
    if (buffer.size() < 8) {
        SPDLOG_DEBUG("Message buffer too small: {} bytes", buffer.size());
        return 0;
    }
    
    try {
        // Parse PITCH messages from buffer
        auto messages = CboePitch::MessageFactory::parseMessages(buffer.data(), buffer.size());
        
        if (messages.empty()) {
            return 0;
        }
        
        // Extract sequence number from header
        auto header = CboePitch::MessageFactory::parseHeader(buffer.data(), buffer.size());
        uint32_t sequence_number = header.getSequence();
        
        // Check for gaps
        if (gap_handler_->check_gap(sequence_number)) {
            SPDLOG_WARN("Gap detected at sequence {}", sequence_number);
        }
        
        int events_generated = 0;
        
        // Process each message
        for (const auto& message : messages) {
            auto event = convert_message_to_event(message, sequence_number);
            
            if (event && validate_message(*event)) {
                update_statistics(*event);
                event_callback_(*event);
                events_generated++;
            }
        }
        
        return events_generated;
        
    } catch (const std::exception& ex) {
        SPDLOG_ERROR("Failed to process message buffer: {}", ex.what());
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.total_parsing_errors++;
        return 0;
    }
}

std::unique_ptr<OrderBookEvent> PitchFeedHandler::convert_message_to_event(
    const std::shared_ptr<CboePitch::Message>& message,
    uint32_t sequence_number) {
    
    if (!message) {
        return nullptr;
    }
    
    auto message_type = message->getMessageType();
    
    switch (message_type) {
        case 0x37: // Add Order
            if (auto add_msg = std::dynamic_pointer_cast<CboePitch::AddOrder>(message)) {
                return handle_add_order(*add_msg, sequence_number);
            }
            break;
            
        case 0x3A: // Modify Order
            if (auto modify_msg = std::dynamic_pointer_cast<CboePitch::ModifyOrder>(message)) {
                return handle_modify_order(*modify_msg, sequence_number);
            }
            break;
            
        case 0x3C: // Delete Order
            if (auto delete_msg = std::dynamic_pointer_cast<CboePitch::DeleteOrder>(message)) {
                return handle_delete_order(*delete_msg, sequence_number);
            }
            break;
            
        case 0x3D: // Trade
            if (auto trade_msg = std::dynamic_pointer_cast<CboePitch::Trade>(message)) {
                return handle_trade(*trade_msg, sequence_number);
            }
            break;
            
        case 0x3B: // Trading Status
            if (auto status_msg = std::dynamic_pointer_cast<CboePitch::TradingStatus>(message)) {
                return handle_trading_status(*status_msg, sequence_number);
            }
            break;
            
        default:
            SPDLOG_DEBUG("Unhandled message type: 0x{:02X}", message_type);
            break;
    }
    
    return nullptr;
}

std::unique_ptr<OrderBookEvent> PitchFeedHandler::handle_add_order(
    const CboePitch::AddOrder& msg, uint32_t sequence_number) {
    
    auto event = std::make_unique<OrderBookEvent>();
    event->type = OrderBookEventType::ADD_ORDER;
    event->symbol = msg.getSymbol();
    event->timestamp = msg.getTimestamp();
    event->sequence_number = sequence_number;
    event->order_id = msg.getOrderId();
    event->price = msg.getPrice();
    event->quantity = msg.getQuantity();
    event->side = msg.getSide();
    event->participant_id = msg.getParticipantId();
    event->process_time = std::chrono::steady_clock::now();
    
    return event;
}

std::unique_ptr<OrderBookEvent> PitchFeedHandler::handle_modify_order(
    const CboePitch::ModifyOrder& msg, uint32_t sequence_number) {
    
    auto event = std::make_unique<OrderBookEvent>();
    event->type = OrderBookEventType::MODIFY_ORDER;
    event->symbol = ""; // ModifyOrder doesn't contain symbol, need lookup
    event->timestamp = msg.getTimestamp();
    event->sequence_number = sequence_number;
    event->order_id = msg.getOrderId();
    event->price = msg.getPrice();
    event->quantity = msg.getQuantity();
    event->process_time = std::chrono::steady_clock::now();
    
    return event;
}

std::unique_ptr<OrderBookEvent> PitchFeedHandler::handle_delete_order(
    const CboePitch::DeleteOrder& msg, uint32_t sequence_number) {
    
    auto event = std::make_unique<OrderBookEvent>();
    event->type = OrderBookEventType::DELETE_ORDER;
    event->symbol = ""; // DeleteOrder doesn't contain symbol, need lookup
    event->timestamp = msg.getTimestamp();
    event->sequence_number = sequence_number;
    event->order_id = msg.getOrderId();
    event->process_time = std::chrono::steady_clock::now();
    
    return event;
}

std::unique_ptr<OrderBookEvent> PitchFeedHandler::handle_trade(
    const CboePitch::Trade& msg, uint32_t sequence_number) {
    
    auto event = std::make_unique<OrderBookEvent>();
    event->type = OrderBookEventType::TRADE;
    event->symbol = msg.getSymbol();
    event->timestamp = msg.getTimestamp();
    event->sequence_number = sequence_number;
    event->order_id = msg.getOrderId();
    event->execution_id = msg.getExecutionId();
    event->price = msg.getPrice();
    event->quantity = msg.getQuantity();
    event->trade_quantity = msg.getQuantity();
    event->participant_id = msg.getPid();
    event->process_time = std::chrono::steady_clock::now();
    
    // Scale price for integer representation
    if (symbol_manager_) {
        auto symbol_info = symbol_manager_->get_symbol_info(msg.getSymbol());
        if (symbol_info) {
            event->trade_price_scaled = symbol_info->scale_price(msg.getPrice());
        }
    }
    
    return event;
}

std::unique_ptr<OrderBookEvent> PitchFeedHandler::handle_trading_status(
    const CboePitch::TradingStatus& msg, uint32_t sequence_number) {
    
    auto event = std::make_unique<OrderBookEvent>();
    event->type = OrderBookEventType::TRADING_STATUS_CHANGE;
    event->symbol = msg.getSymbol();
    event->timestamp = msg.getTimestamp();
    event->sequence_number = sequence_number;
    event->trading_status = msg.getTradingStatus();
    event->process_time = std::chrono::steady_clock::now();
    
    return event;
}

bool PitchFeedHandler::validate_message(const OrderBookEvent& event) {
    if (!symbol_manager_) {
        return true;  // No validation if symbol manager not available
    }
    
    // Check if symbol exists and is active
    if (!event.symbol.empty() && !symbol_manager_->has_symbol(event.symbol)) {
        SPDLOG_DEBUG("Unknown symbol in event: {}", event.symbol);
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.total_validation_errors++;
        return false;
    }
    
    // Validate price and quantity ranges
    if (event.type == OrderBookEventType::ADD_ORDER || 
        event.type == OrderBookEventType::MODIFY_ORDER ||
        event.type == OrderBookEventType::TRADE) {
        
        if (!symbol_manager_->validate_price(event.symbol, event.price)) {
            SPDLOG_DEBUG("Invalid price {} for symbol {}", event.price, event.symbol);
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.total_validation_errors++;
            return false;
        }
        
        if (!symbol_manager_->validate_quantity(event.symbol, event.quantity)) {
            SPDLOG_DEBUG("Invalid quantity {} for symbol {}", event.quantity, event.symbol);
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.total_validation_errors++;
            return false;
        }
    }
    
    return true;
}

void PitchFeedHandler::update_statistics(const OrderBookEvent& event) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    switch (event.type) {
        case OrderBookEventType::ADD_ORDER:
            stats_.add_order_count++;
            break;
        case OrderBookEventType::MODIFY_ORDER:
            stats_.modify_order_count++;
            break;
        case OrderBookEventType::DELETE_ORDER:
            stats_.delete_order_count++;
            break;
        case OrderBookEventType::TRADE:
            stats_.trade_count++;
            break;
        case OrderBookEventType::TRADING_STATUS_CHANGE:
            stats_.status_count++;
            break;
    }
    
    // Calculate processing latency
    auto processing_latency = std::chrono::duration_cast<std::chrono::microseconds>(
        event.process_time - event.receive_time).count();
    
    if (processing_latency > 0) {
        stats_.max_processing_latency_us = std::max(
            stats_.max_processing_latency_us, static_cast<double>(processing_latency));
        
        // Update average (simple moving average)
        if (stats_.total_messages_processed > 0) {
            stats_.avg_processing_latency_us = 
                (stats_.avg_processing_latency_us * (stats_.total_messages_processed - 1) + 
                 processing_latency) / stats_.total_messages_processed;
        } else {
            stats_.avg_processing_latency_us = processing_latency;
        }
    }
    
    // Calculate messages per second
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
    if (duration > 0) {
        stats_.messages_per_second = static_cast<double>(stats_.total_messages_processed) / duration;
    }
    
    // Copy gap statistics
    stats_.gap_stats = gap_handler_->get_stats();
    
    stats_.last_update_time = now;
}

void PitchFeedHandler::reset_statistics() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = Statistics{};
    start_time_ = std::chrono::steady_clock::now();
    gap_handler_->reset();
    
    SPDLOG_INFO("PitchFeedHandler statistics reset");
}

void PitchFeedHandler::cleanup() {
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
    
    // Clear message queue
    std::lock_guard<std::mutex> lock(queue_mutex_);
    while (!message_queue_.empty()) {
        message_queue_.pop();
    }
}

} // namespace md_l2