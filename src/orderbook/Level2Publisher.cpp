#include "orderbook/Level2Publisher.hpp"
#include "KafkaPush.hpp"
#include <spdlog/spdlog.h>
#include <flatbuffers/flatbuffers.h>
#include <fstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <functional>

namespace md_l2 {

// Thread-local FlatBuffers builder
thread_local flatbuffers::FlatBufferBuilder Level2Publisher::fb_builder_;

Level2Publisher::Level2Publisher(const Config& config,
                                 std::shared_ptr<LatencyTracker> latency_tracker)
    : config_(config), latency_tracker_(latency_tracker) {
    
    start_time_ = std::chrono::steady_clock::now();
    SPDLOG_INFO("Level2Publisher initialized with {} channels", config_.enabled_channels.size());
}

Level2Publisher::~Level2Publisher() {
    stop();
    cleanup();
}

bool Level2Publisher::start(PublishedMessageCallback message_callback) {
    if (running_.load()) {
        SPDLOG_WARN("Level2Publisher is already running");
        return false;
    }
    
    message_callback_ = message_callback;
    
    // Validate configuration
    if (config_.enabled_channels.empty()) {
        SPDLOG_ERROR("No publication channels enabled");
        return false;
    }
    
    // Initialize channel-specific resources
    for (auto channel : config_.enabled_channels) {
        switch (channel) {
            case PublicationChannel::KAFKA:
                // Kafka initialization is handled by KafkaProducer singleton
                break;
            case PublicationChannel::SHARED_MEMORY:
                // TODO: Initialize shared memory
                break;
            case PublicationChannel::FILE:
                // Ensure output directory exists
                std::filesystem::create_directories(config_.output_directory);
                break;
            case PublicationChannel::NETWORK_MULTICAST:
                // TODO: Initialize multicast socket
                break;
        }
    }
    
    running_.store(true);
    
    // Start publisher threads
    for (int i = 0; i < config_.publisher_thread_count; ++i) {
        publisher_threads_.emplace_back(&Level2Publisher::publisher_thread_func, this, i);
    }
    
    SPDLOG_INFO("Level2Publisher started with {} threads", config_.publisher_thread_count);
    return true;
}

void Level2Publisher::stop() {
    if (!running_.load()) {
        return;
    }
    
    SPDLOG_INFO("Stopping Level2Publisher...");
    running_.store(false);
    
    // Wake up all publisher threads
    queue_cv_.notify_all();
    
    // Join all threads
    for (auto& thread : publisher_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    publisher_threads_.clear();
    
    SPDLOG_INFO("Level2Publisher stopped");
}

void Level2Publisher::publish_snapshot(const Level2Snapshot& snapshot) {
    if (!running_.load()) {
        return;
    }
    
    QueuedMessage message(QueuedMessage::SNAPSHOT);
    message.snapshots.push_back(snapshot);
    
    enqueue_message(std::move(message));
}

void Level2Publisher::publish_delta(const Level2Delta& delta) {
    if (!running_.load()) {
        return;
    }
    
    QueuedMessage message(QueuedMessage::DELTA);
    message.deltas.push_back(delta);
    
    enqueue_message(std::move(message));
}

void Level2Publisher::publish_snapshot_batch(const std::vector<Level2Snapshot>& snapshots) {
    if (!running_.load() || snapshots.empty()) {
        return;
    }
    
    if (config_.enable_batching) {
        QueuedMessage message(QueuedMessage::BATCH_SNAPSHOTS);
        message.snapshots = snapshots;
        enqueue_message(std::move(message));
    } else {
        // Publish individually
        for (const auto& snapshot : snapshots) {
            publish_snapshot(snapshot);
        }
    }
}

void Level2Publisher::publish_delta_batch(const std::vector<Level2Delta>& deltas) {
    if (!running_.load() || deltas.empty()) {
        return;
    }
    
    if (config_.enable_batching) {
        QueuedMessage message(QueuedMessage::BATCH_DELTAS);
        message.deltas = deltas;
        enqueue_message(std::move(message));
    } else {
        // Publish individually
        for (const auto& delta : deltas) {
            publish_delta(delta);
        }
    }
}

bool Level2Publisher::flush(int timeout_ms) {
    auto start_time = std::chrono::steady_clock::now();
    auto timeout_time = start_time + std::chrono::milliseconds(timeout_ms);
    
    // Wait for queue to empty
    while (current_queue_size_.load() > 0) {
        if (std::chrono::steady_clock::now() > timeout_time) {
            SPDLOG_WARN("Flush timeout after {}ms, {} messages remaining", 
                       timeout_ms, current_queue_size_.load());
            return false;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();
    
    SPDLOG_INFO("Flush completed in {}ms", elapsed);
    return true;
}

void Level2Publisher::reset_statistics() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = Statistics{};
    start_time_ = std::chrono::steady_clock::now();
    
    SPDLOG_INFO("Level2Publisher statistics reset");
}

void Level2Publisher::publisher_thread_func(int thread_id) {
    SPDLOG_INFO("Publisher thread {} started", thread_id);
    
    // Initialize thread-local FlatBuffers builder
    fb_builder_.Clear();
    fb_builder_.Reset();
    
    while (running_.load()) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        
        // Wait for messages or shutdown
        queue_cv_.wait(lock, [this] { 
            return !message_queue_.empty() || !running_.load(); 
        });
        
        if (!running_.load() && message_queue_.empty()) {
            break;
        }
        
        // Process available messages
        while (!message_queue_.empty()) {
            auto queued_message = std::move(message_queue_.front());
            message_queue_.pop();
            current_queue_size_.fetch_sub(1, std::memory_order_relaxed);
            lock.unlock();
            
            try {
                // Process the message based on type
                switch (queued_message.type) {
                    case QueuedMessage::SNAPSHOT:
                        if (!queued_message.snapshots.empty()) {
                            process_snapshot(queued_message.snapshots[0]);
                        }
                        break;
                        
                    case QueuedMessage::DELTA:
                        if (!queued_message.deltas.empty()) {
                            process_delta(queued_message.deltas[0]);
                        }
                        break;
                        
                    case QueuedMessage::BATCH_SNAPSHOTS:
                        process_snapshot_batch(queued_message.snapshots);
                        break;
                        
                    case QueuedMessage::BATCH_DELTAS:
                        process_delta_batch(queued_message.deltas);
                        break;
                }
                
            } catch (const std::exception& ex) {
                SPDLOG_ERROR("Error processing message in thread {}: {}", thread_id, ex.what());
            }
            
            lock.lock();
        }
    }
    
    SPDLOG_INFO("Publisher thread {} stopped", thread_id);
}

void Level2Publisher::process_snapshot(const Level2Snapshot& snapshot) {
    if (latency_tracker_) {
        LATENCY_TRACK_SCOPE(latency_tracker_.get(), "publisher_snapshot_serialization");
    }
    
    for (auto format : config_.enabled_formats) {
        std::vector<uint8_t> serialized_data;
        
        switch (format) {
            case DataFormat::FLATBUFFERS:
                serialized_data = serialize_snapshot_flatbuf(snapshot);
                break;
            case DataFormat::JSON:
                // TODO: Implement JSON serialization
                SPDLOG_DEBUG("JSON serialization not implemented");
                continue;
            case DataFormat::BINARY_PROPRIETARY:
                // TODO: Implement proprietary binary format
                SPDLOG_DEBUG("Binary proprietary serialization not implemented");
                continue;
        }
        
        if (serialized_data.empty()) {
            continue;
        }
        
        // Create published message
        PublishedMessage pub_message;
        pub_message.format = format;
        pub_message.topic = generate_topic_name(snapshot.symbol, "snapshot");
        pub_message.data = std::move(serialized_data);
        pub_message.sequence_number = snapshot.sequence_number;
        
        // Publish to all enabled channels
        for (auto channel : config_.enabled_channels) {
            pub_message.channel = channel;
            publish_to_channel(pub_message, channel);
        }
        
        update_statistics(pub_message);
        
        // Call user callback if set
        if (message_callback_) {
            message_callback_(pub_message);
        }
    }
}

void Level2Publisher::process_delta(const Level2Delta& delta) {
    if (latency_tracker_) {
        LATENCY_TRACK_SCOPE(latency_tracker_.get(), "publisher_delta_serialization");
    }
    
    for (auto format : config_.enabled_formats) {
        std::vector<uint8_t> serialized_data;
        
        switch (format) {
            case DataFormat::FLATBUFFERS:
                serialized_data = serialize_delta_flatbuf(delta);
                break;
            case DataFormat::JSON:
                // TODO: Implement JSON serialization
                continue;
            case DataFormat::BINARY_PROPRIETARY:
                // TODO: Implement proprietary binary format
                continue;
        }
        
        if (serialized_data.empty()) {
            continue;
        }
        
        // Create published message
        PublishedMessage pub_message;
        pub_message.format = format;
        pub_message.topic = generate_topic_name(delta.symbol, "delta");
        pub_message.data = std::move(serialized_data);
        pub_message.sequence_number = delta.sequence_number;
        
        // Publish to all enabled channels
        for (auto channel : config_.enabled_channels) {
            pub_message.channel = channel;
            publish_to_channel(pub_message, channel);
        }
        
        update_statistics(pub_message);
        
        // Call user callback if set
        if (message_callback_) {
            message_callback_(pub_message);
        }
    }
}

void Level2Publisher::process_snapshot_batch(const std::vector<Level2Snapshot>& snapshots) {
    if (snapshots.empty()) {
        return;
    }
    
    for (auto format : config_.enabled_formats) {
        std::vector<uint8_t> serialized_data;
        
        switch (format) {
            case DataFormat::FLATBUFFERS:
                serialized_data = serialize_snapshot_batch_flatbuf(snapshots);
                break;
            default:
                // For non-batch formats, process individually
                for (const auto& snapshot : snapshots) {
                    process_snapshot(snapshot);
                }
                continue;
        }
        
        if (serialized_data.empty()) {
            continue;
        }
        
        // Create published message for batch
        PublishedMessage pub_message;
        pub_message.format = format;
        pub_message.topic = generate_topic_name("", "snapshot_batch");
        pub_message.data = std::move(serialized_data);
        pub_message.sequence_number = snapshots.back().sequence_number;
        
        // Publish to all enabled channels
        for (auto channel : config_.enabled_channels) {
            pub_message.channel = channel;
            publish_to_channel(pub_message, channel);
        }
        
        update_statistics(pub_message);
        
        if (message_callback_) {
            message_callback_(pub_message);
        }
    }
}

void Level2Publisher::process_delta_batch(const std::vector<Level2Delta>& deltas) {
    if (deltas.empty()) {
        return;
    }
    
    for (auto format : config_.enabled_formats) {
        std::vector<uint8_t> serialized_data;
        
        switch (format) {
            case DataFormat::FLATBUFFERS:
                serialized_data = serialize_delta_batch_flatbuf(deltas);
                break;
            default:
                // For non-batch formats, process individually
                for (const auto& delta : deltas) {
                    process_delta(delta);
                }
                continue;
        }
        
        if (serialized_data.empty()) {
            continue;
        }
        
        // Create published message for batch
        PublishedMessage pub_message;
        pub_message.format = format;
        pub_message.topic = generate_topic_name("", "delta_batch");
        pub_message.data = std::move(serialized_data);
        pub_message.sequence_number = deltas.back().sequence_number;
        
        // Publish to all enabled channels
        for (auto channel : config_.enabled_channels) {
            pub_message.channel = channel;
            publish_to_channel(pub_message, channel);
        }
        
        update_statistics(pub_message);
        
        if (message_callback_) {
            message_callback_(pub_message);
        }
    }
}

std::vector<uint8_t> Level2Publisher::serialize_snapshot_flatbuf(const Level2Snapshot& snapshot) {
    fb_builder_.Clear();
    
    // Create symbol string
    auto symbol_str = fb_builder_.CreateString(snapshot.symbol);
    
    // Create buy side levels
    std::vector<flatbuffers::Offset<md::OrderMsgLevel>> buy_levels;
    for (const auto& [price, quantity] : snapshot.buy_levels) {
        // For snapshots, we create a single order representing the aggregated level
        std::vector<flatbuffers::Offset<md::OrderMsgOrder>> orders;
        auto order = md::CreateOrderMsgOrder(fb_builder_, 0, quantity, md::Side_Buy);
        orders.push_back(order);
        
        auto level = md::CreateOrderMsgLevel(fb_builder_, 
            static_cast<uint64_t>(price * 10000000), // Scale price
            fb_builder_.CreateVector(orders));
        buy_levels.push_back(level);
    }
    
    // Create sell side levels
    std::vector<flatbuffers::Offset<md::OrderMsgLevel>> sell_levels;
    for (const auto& [price, quantity] : snapshot.sell_levels) {
        std::vector<flatbuffers::Offset<md::OrderMsgOrder>> orders;
        auto order = md::CreateOrderMsgOrder(fb_builder_, 0, quantity, md::Side_Sell);
        orders.push_back(order);
        
        auto level = md::CreateOrderMsgLevel(fb_builder_,
            static_cast<uint64_t>(price * 10000000), // Scale price
            fb_builder_.CreateVector(orders));
        sell_levels.push_back(level);
    }
    
    // Create snapshot
    auto buy_vec = fb_builder_.CreateVector(buy_levels);
    auto sell_vec = fb_builder_.CreateVector(sell_levels);
    
    auto snapshot_fb = md::CreateOrderBookSnapshot(
        fb_builder_,
        symbol_str,
        snapshot.sequence_number,
        buy_vec,
        sell_vec,
        static_cast<uint64_t>(snapshot.last_trade_price * 10000000),
        snapshot.last_trade_quantity
    );
    
    // Create envelope
    auto envelope = md::CreateEnvelope(fb_builder_, 
        md::BookMsg_OrderBookSnapshot, 
        snapshot_fb.Union());
    
    fb_builder_.Finish(envelope);
    
    // Return serialized data
    return std::vector<uint8_t>(fb_builder_.GetBufferPointer(), 
                               fb_builder_.GetBufferPointer() + fb_builder_.GetSize());
}

std::vector<uint8_t> Level2Publisher::serialize_delta_flatbuf(const Level2Delta& delta) {
    fb_builder_.Clear();
    
    // Convert delta to FlatBuffers delta event
    md::Kind kind;
    switch (delta.change_type) {
        case Level2Delta::ChangeType::ADD_ORDER:
            kind = md::Kind_Add;
            break;
        case Level2Delta::ChangeType::MODIFY_ORDER:
            kind = md::Kind_Modify;
            break;
        case Level2Delta::ChangeType::DELETE_ORDER:
            kind = md::Kind_Remove;
            break;
        case Level2Delta::ChangeType::TRADE:
            kind = md::Kind_Trade;
            break;
    }
    
    md::Side side = (delta.side == 'B') ? md::Side_Buy : md::Side_Sell;
    
    auto delta_event = md::CreateFBBookDeltaEvent(
        fb_builder_,
        kind,
        delta.order_id,
        static_cast<uint64_t>(delta.price * 10000000), // Scale price
        delta.quantity,
        side,
        delta.sequence_number
    );
    
    // Create delta batch with single event
    auto symbol_str = fb_builder_.CreateString(delta.symbol);
    std::vector<flatbuffers::Offset<md::FBBookDeltaEvent>> events;
    events.push_back(delta_event);
    auto events_vec = fb_builder_.CreateVector(events);
    
    auto delta_batch = md::CreateDeltaBatch(
        fb_builder_,
        symbol_str,
        delta.sequence_number,
        delta.sequence_number,
        events_vec
    );
    
    // Create envelope
    auto envelope = md::CreateEnvelope(fb_builder_,
        md::BookMsg_DeltaBatch,
        delta_batch.Union());
    
    fb_builder_.Finish(envelope);
    
    return std::vector<uint8_t>(fb_builder_.GetBufferPointer(),
                               fb_builder_.GetBufferPointer() + fb_builder_.GetSize());
}

std::vector<uint8_t> Level2Publisher::serialize_snapshot_batch_flatbuf(const std::vector<Level2Snapshot>& snapshots) {
    // For now, just serialize the first snapshot
    // TODO: Implement proper batch serialization if needed
    if (!snapshots.empty()) {
        return serialize_snapshot_flatbuf(snapshots[0]);
    }
    return {};
}

std::vector<uint8_t> Level2Publisher::serialize_delta_batch_flatbuf(const std::vector<Level2Delta>& deltas) {
    if (deltas.empty()) {
        return {};
    }
    
    fb_builder_.Clear();
    
    // Create events vector
    std::vector<flatbuffers::Offset<md::FBBookDeltaEvent>> events;
    events.reserve(deltas.size());
    
    uint64_t seq_start = deltas.front().sequence_number;
    uint64_t seq_end = deltas.back().sequence_number;
    std::string symbol = deltas.front().symbol;  // Assume all deltas are for same symbol
    
    for (const auto& delta : deltas) {
        md::Kind kind;
        switch (delta.change_type) {
            case Level2Delta::ChangeType::ADD_ORDER:
                kind = md::Kind_Add;
                break;
            case Level2Delta::ChangeType::MODIFY_ORDER:
                kind = md::Kind_Modify;
                break;
            case Level2Delta::ChangeType::DELETE_ORDER:
                kind = md::Kind_Remove;
                break;
            case Level2Delta::ChangeType::TRADE:
                kind = md::Kind_Trade;
                break;
        }
        
        md::Side side = (delta.side == 'B') ? md::Side_Buy : md::Side_Sell;
        
        auto delta_event = md::CreateFBBookDeltaEvent(
            fb_builder_,
            kind,
            delta.order_id,
            static_cast<uint64_t>(delta.price * 10000000),
            delta.quantity,
            side,
            delta.sequence_number
        );
        
        events.push_back(delta_event);
    }
    
    // Create delta batch
    auto symbol_str = fb_builder_.CreateString(symbol);
    auto events_vec = fb_builder_.CreateVector(events);
    
    auto delta_batch = md::CreateDeltaBatch(
        fb_builder_,
        symbol_str,
        seq_start,
        seq_end,
        events_vec
    );
    
    // Create envelope
    auto envelope = md::CreateEnvelope(fb_builder_,
        md::BookMsg_DeltaBatch,
        delta_batch.Union());
    
    fb_builder_.Finish(envelope);
    
    return std::vector<uint8_t>(fb_builder_.GetBufferPointer(),
                               fb_builder_.GetBufferPointer() + fb_builder_.GetSize());
}

void Level2Publisher::publish_to_channel(const PublishedMessage& message, PublicationChannel channel) {
    try {
        switch (channel) {
            case PublicationChannel::KAFKA:
                publish_to_kafka(message);
                break;
            case PublicationChannel::SHARED_MEMORY:
                publish_to_shared_memory(message);
                break;
            case PublicationChannel::FILE:
                publish_to_file(message);
                break;
            case PublicationChannel::NETWORK_MULTICAST:
                publish_to_multicast(message);
                break;
        }
    } catch (const std::exception& ex) {
        SPDLOG_ERROR("Error publishing to channel {}: {}", static_cast<int>(channel), ex.what());
    }
}

void Level2Publisher::publish_to_kafka(const PublishedMessage& message) {
    try {
        int partition = calculate_partition(message.topic);
        KafkaPush(message.topic, partition, message.data.data(), message.data.size());
        
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.kafka_messages++;
    } catch (const std::exception& ex) {
        SPDLOG_ERROR("Kafka publish error: {}", ex.what());
    }
}

void Level2Publisher::publish_to_shared_memory(const PublishedMessage& message) {
    // TODO: Implement shared memory publishing
    SPDLOG_DEBUG("Shared memory publishing not implemented");
}

void Level2Publisher::publish_to_file(const PublishedMessage& message) {
    try {
        std::string filename = config_.output_directory + "/" + message.topic + ".bin";
        std::ofstream file(filename, std::ios::binary | std::ios::app);
        
        if (file.is_open()) {
            // Write message size followed by data
            uint32_t size = static_cast<uint32_t>(message.data.size());
            file.write(reinterpret_cast<const char*>(&size), sizeof(size));
            file.write(reinterpret_cast<const char*>(message.data.data()), message.data.size());
            file.close();
            
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.file_messages++;
        }
    } catch (const std::exception& ex) {
        SPDLOG_ERROR("File publish error: {}", ex.what());
    }
}

void Level2Publisher::publish_to_multicast(const PublishedMessage& message) {
    // TODO: Implement multicast publishing
    SPDLOG_DEBUG("Multicast publishing not implemented");
}

bool Level2Publisher::enqueue_message(QueuedMessage&& message) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    
    if (message_queue_.size() >= config_.max_queue_size) {
        // Queue is full, drop oldest message
        message_queue_.pop();
        current_queue_size_.fetch_sub(1, std::memory_order_relaxed);
        
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_.total_queue_overflows++;
        return false;
    }
    
    message_queue_.push(std::move(message));
    size_t new_size = current_queue_size_.fetch_add(1, std::memory_order_relaxed) + 1;
    
    // Update max queue size reached
    {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        stats_.max_queue_size_reached = std::max(stats_.max_queue_size_reached, new_size);
    }
    
    queue_cv_.notify_one();
    return true;
}

std::string Level2Publisher::generate_topic_name(const std::string& symbol, const std::string& message_type) {
    if (symbol.empty()) {
        return config_.kafka.topic_prefix + "_" + message_type;
    }
    return config_.kafka.topic_prefix + "_" + message_type + "_" + symbol;
}

int Level2Publisher::calculate_partition(const std::string& symbol) {
    if (config_.kafka.partition_strategy == "symbol_hash") {
        std::hash<std::string> hasher;
        return static_cast<int>(hasher(symbol) % 10);  // Assume 10 partitions
    } else {
        // Round robin or default
        static std::atomic<int> partition_counter{0};
        return partition_counter.fetch_add(1, std::memory_order_relaxed) % 10;
    }
}

void Level2Publisher::update_statistics(const PublishedMessage& message) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    stats_.total_messages_published++;
    stats_.total_bytes_published += message.data.size();
    
    if (message.topic.find("snapshot") != std::string::npos) {
        stats_.total_snapshots_published++;
    } else if (message.topic.find("delta") != std::string::npos) {
        stats_.total_deltas_published++;
    }
    
    // Update channel-specific stats
    switch (message.channel) {
        case PublicationChannel::KAFKA:
            stats_.kafka_messages++;
            break;
        case PublicationChannel::SHARED_MEMORY:
            stats_.shared_memory_messages++;
            break;
        case PublicationChannel::FILE:
            stats_.file_messages++;
            break;
        case PublicationChannel::NETWORK_MULTICAST:
            stats_.multicast_messages++;
            break;
    }
    
    // Calculate messages per second
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
    if (duration > 0) {
        stats_.messages_per_second = static_cast<double>(stats_.total_messages_published) / duration;
    }
    
    stats_.current_queue_size = current_queue_size_.load();
    stats_.last_update_time = now;
}

void Level2Publisher::cleanup() {
    // Clear message queue
    std::lock_guard<std::mutex> lock(queue_mutex_);
    while (!message_queue_.empty()) {
        message_queue_.pop();
    }
    current_queue_size_.store(0, std::memory_order_relaxed);
}

} // namespace md_l2