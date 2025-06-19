#pragma once

#include <memory>
#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>

#include <flatbuffers/flatbuffers.h>
#include "generated/orderbook_generated.h"  // Generated from orderbook.fbs
#include "orderbook/OrderBookManager.hpp"
#include "utils/LatencyTracker.hpp"

namespace md_l2 {

/**
 * @brief Publication channels for Level 2 data
 */
enum class PublicationChannel {
    KAFKA,
    SHARED_MEMORY,
    FILE,
    NETWORK_MULTICAST
};

/**
 * @brief Data format for publishing
 */
enum class DataFormat {
    FLATBUFFERS,
    JSON,
    BINARY_PROPRIETARY
};

/**
 * @brief Published message wrapper
 */
struct PublishedMessage {
    PublicationChannel channel;
    DataFormat format;
    std::string topic;
    std::vector<uint8_t> data;
    uint64_t timestamp;
    uint64_t sequence_number;
    
    PublishedMessage() {
        timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
};

/**
 * @brief Callback for published messages
 */
using PublishedMessageCallback = std::function<void(const PublishedMessage&)>;

/**
 * @brief Kafka-specific publishing configuration
 */
struct KafkaPublishConfig {
    std::string topic_prefix = "md_l2";
    std::string partition_strategy = "symbol_hash";  // symbol_hash, round_robin
    int default_partition = 0;
    bool enable_compression = true;
    int batch_size = 1000;
    int linger_ms = 5;
};

/**
 * @brief Shared memory publishing configuration
 */
struct SharedMemoryConfig {
    std::string memory_path = "/dev/shm/market_data_l2";
    size_t memory_size_mb = 256;
    bool use_ring_buffer = true;
    int max_readers = 10;
};

/**
 * @brief Level 2 data publisher using FlatBuffers
 * 
 * Converts Level 2 snapshots and deltas to FlatBuffers format and publishes
 * them through various channels (Kafka, shared memory, files, etc.).
 * Supports batching, compression, and multiple output formats.
 */
class Level2Publisher {
public:
    /**
     * @brief Publisher configuration
     */
    struct Config {
        std::vector<PublicationChannel> enabled_channels;
        std::vector<DataFormat> enabled_formats{DataFormat::FLATBUFFERS};
        
        // Batching settings
        bool enable_batching = true;
        int max_batch_size = 1000;
        int max_batch_delay_ms = 10;
        
        // Performance settings
        int publisher_thread_count = 2;
        size_t max_queue_size = 100000;
        
        // Channel-specific configs
        KafkaPublishConfig kafka;
        SharedMemoryConfig shared_memory;
        
        // Output file settings (for file channel)
        std::string output_directory = "/var/log/market_data_l2";
        bool rotate_files = true;
        int max_file_size_mb = 100;
        
        // Network multicast settings
        std::string multicast_group = "224.0.2.100";
        int multicast_port = 25000;
        std::string multicast_interface = "eth0";
    };
    
    /**
     * @brief Constructor
     * @param config Publisher configuration
     * @param latency_tracker Latency tracking (optional)
     */
    Level2Publisher(const Config& config,
                    std::shared_ptr<LatencyTracker> latency_tracker = nullptr);
    
    ~Level2Publisher();
    
    // Non-copyable
    Level2Publisher(const Level2Publisher&) = delete;
    Level2Publisher& operator=(const Level2Publisher&) = delete;
    
    /**
     * @brief Start the publisher
     * @param message_callback Callback for published messages (optional)
     * @return true if started successfully
     */
    bool start(PublishedMessageCallback message_callback = nullptr);
    
    /**
     * @brief Stop the publisher
     */
    void stop();
    
    /**
     * @brief Check if publisher is running
     */
    bool is_running() const { return running_.load(); }
    
    /**
     * @brief Publish Level 2 snapshot
     * @param snapshot Level 2 snapshot data
     */
    void publish_snapshot(const Level2Snapshot& snapshot);
    
    /**
     * @brief Publish Level 2 delta
     * @param delta Level 2 delta data
     */
    void publish_delta(const Level2Delta& delta);
    
    /**
     * @brief Publish batch of snapshots
     * @param snapshots Vector of snapshots
     */
    void publish_snapshot_batch(const std::vector<Level2Snapshot>& snapshots);
    
    /**
     * @brief Publish batch of deltas
     * @param deltas Vector of deltas
     */
    void publish_delta_batch(const std::vector<Level2Delta>& deltas);
    
    /**
     * @brief Get publisher statistics
     */
    struct Statistics {
        uint64_t total_snapshots_published = 0;
        uint64_t total_deltas_published = 0;
        uint64_t total_messages_published = 0;
        uint64_t total_bytes_published = 0;
        uint64_t total_batches_published = 0;
        
        // Channel-specific stats
        uint64_t kafka_messages = 0;
        uint64_t shared_memory_messages = 0;
        uint64_t file_messages = 0;
        uint64_t multicast_messages = 0;
        
        // Performance metrics
        double messages_per_second = 0.0;
        double avg_serialization_latency_us = 0.0;
        double avg_publishing_latency_us = 0.0;
        double max_publishing_latency_us = 0.0;
        
        // Queue status
        size_t current_queue_size = 0;
        size_t max_queue_size_reached = 0;
        uint64_t total_queue_overflows = 0;
        
        std::chrono::steady_clock::time_point last_update_time;
    };
    
    const Statistics& get_statistics() const { return stats_; }
    
    /**
     * @brief Reset statistics
     */
    void reset_statistics();
    
    /**
     * @brief Flush all pending messages
     * @param timeout_ms Maximum time to wait for flush
     * @return true if all messages were flushed
     */
    bool flush(int timeout_ms = 5000);
    
private:
    Config config_;
    std::shared_ptr<LatencyTracker> latency_tracker_;
    PublishedMessageCallback message_callback_;
    
    // Threading
    std::atomic<bool> running_{false};
    std::vector<std::thread> publisher_threads_;
    
    // Message queue
    struct QueuedMessage {
        enum Type { SNAPSHOT, DELTA, BATCH_SNAPSHOTS, BATCH_DELTAS };
        Type type;
        std::vector<Level2Snapshot> snapshots;
        std::vector<Level2Delta> deltas;
        uint64_t enqueue_time;
        
        QueuedMessage(Type t) : type(t) {
            enqueue_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }
    };
    
    std::queue<QueuedMessage> message_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::atomic<size_t> current_queue_size_{0};
    
    // Statistics
    mutable std::mutex stats_mutex_;
    Statistics stats_;
    std::chrono::steady_clock::time_point start_time_;
    
    // FlatBuffers builders (thread-local)
    thread_local static flatbuffers::FlatBufferBuilder fb_builder_;
    
    /**
     * @brief Publisher thread function
     * @param thread_id Thread identifier
     */
    void publisher_thread_func(int thread_id);
    
    /**
     * @brief Serialize Level 2 snapshot to FlatBuffers
     * @param snapshot Snapshot data
     * @return Serialized FlatBuffer
     */
    std::vector<uint8_t> serialize_snapshot_flatbuf(const Level2Snapshot& snapshot);
    
    /**
     * @brief Serialize Level 2 delta to FlatBuffers
     * @param delta Delta data
     * @return Serialized FlatBuffer
     */
    std::vector<uint8_t> serialize_delta_flatbuf(const Level2Delta& delta);
    
    /**
     * @brief Serialize batch of snapshots to FlatBuffers
     * @param snapshots Vector of snapshots
     * @return Serialized FlatBuffer
     */
    std::vector<uint8_t> serialize_snapshot_batch_flatbuf(const std::vector<Level2Snapshot>& snapshots);
    
    /**
     * @brief Serialize batch of deltas to FlatBuffers
     * @param deltas Vector of deltas
     * @return Serialized FlatBuffer
     */
    std::vector<uint8_t> serialize_delta_batch_flatbuf(const std::vector<Level2Delta>& deltas);
    
    /**
     * @brief Convert Level2Snapshot to FlatBuffers OrderBookSnapshot
     * @param builder FlatBuffers builder
     * @param snapshot Source snapshot
     * @return FlatBuffers offset
     */
    flatbuffers::Offset<md::OrderBookSnapshot> create_fb_snapshot(
        flatbuffers::FlatBufferBuilder& builder, const Level2Snapshot& snapshot);
    
    /**
     * @brief Convert Level2Delta to FlatBuffers FBBookDeltaEvent
     * @param builder FlatBuffers builder
     * @param delta Source delta
     * @return FlatBuffers offset
     */
    flatbuffers::Offset<md::FBBookDeltaEvent> create_fb_delta(
        flatbuffers::FlatBufferBuilder& builder, const Level2Delta& delta);
    
    /**
     * @brief Publish message to specific channel
     * @param message Published message
     * @param channel Target channel
     */
    void publish_to_channel(const PublishedMessage& message, PublicationChannel channel);
    
    /**
     * @brief Publish to Kafka
     * @param message Published message
     */
    void publish_to_kafka(const PublishedMessage& message);
    
    /**
     * @brief Publish to shared memory
     * @param message Published message
     */
    void publish_to_shared_memory(const PublishedMessage& message);
    
    /**
     * @brief Publish to file
     * @param message Published message
     */
    void publish_to_file(const PublishedMessage& message);
    
    /**
     * @brief Publish to network multicast
     * @param message Published message
     */
    void publish_to_multicast(const PublishedMessage& message);
    
    /**
     * @brief Enqueue message for publishing
     * @param message Queued message
     * @return true if enqueued successfully
     */
    bool enqueue_message(QueuedMessage&& message);
    
    /**
     * @brief Generate topic name for message
     * @param symbol Symbol name
     * @param message_type Message type ("snapshot" or "delta")
     * @return Topic name
     */
    std::string generate_topic_name(const std::string& symbol, const std::string& message_type);
    
    /**
     * @brief Calculate partition for symbol
     * @param symbol Symbol name
     * @return Partition number
     */
    int calculate_partition(const std::string& symbol);
    
    /**
     * @brief Update statistics
     */
    void update_statistics(const PublishedMessage& message);
    
    /**
     * @brief Cleanup resources
     */
    void cleanup();
};

} // namespace md_l2