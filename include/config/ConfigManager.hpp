#pragma once

#include <string>
#include <vector>
#include <memory>
#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>

namespace md_l2 {

/**
 * @brief Configuration structure for the entire market data system
 */
struct Config {
    // Global settings
    struct Global {
        std::string log_level = "INFO";
        std::string log_path = "/var/log/market_data_l2";
        int log_max_size_mb = 100;
        int log_max_files = 50;
    } global;
    
    // Feed configuration
    struct Feed {
        std::string source_type = "PITCH";
        std::string multicast_group = "224.0.1.100";
        int multicast_port = 15000;
        std::string interface = "eth0";
        int buffer_size = 65536;
        
        struct Pitch {
            int unit_id = 1;
            int sequence_number_gap_threshold = 10;
            int heartbeat_interval_ms = 1000;
        } pitch;
    } feed;
    
    // Order book settings
    struct OrderBook {
        std::vector<std::string> symbols;
        int initial_capacity = 10000;
        int max_price_levels = 1000;
        int snapshot_interval_ms = 1000;
        int delta_interval_ms = 50;
        bool keep_trade_history = true;
        int max_trade_history = 10000;
    } orderbook;
    
    // Publishing configuration
    struct Publishing {
        bool enabled = true;
        std::vector<std::string> formats{"flatbuffers"};
        
        struct Kafka {
            bool enabled = true;
            std::string topic_prefix = "md_l2";
            std::string partition_strategy = "symbol_hash";
        } kafka;
        
        struct SharedMemory {
            bool enabled = false;
            std::string path = "/dev/shm/market_data_l2";
            int size_mb = 256;
        } shared_memory;
    } publishing;
    
    // Kafka cluster configuration
    struct KafkaCluster {
        std::string bootstrap_servers = "localhost:9092";
        std::string compression = "lz4";
        std::string acks = "1";
        int queue_buffering_max_messages = 1000000;
        int batch_num_messages = 10000;
        int linger_ms = 5;
        std::string group_id = "market_data_l2_gap_recovery";
        int session_timeout_ms = 6000;
        std::string auto_offset_reset = "earliest";
        bool enable_auto_commit = true;
        std::vector<std::string> topics;
    } kafka_cluster;
    
    // Performance monitoring
    struct Monitoring {
        bool enabled = true;
        int stats_interval_ms = 5000;
        
        struct LatencyTracking {
            bool enabled = true;
            std::vector<double> percentiles{50, 90, 95, 99, 99.9};
        } latency_tracking;
        
        struct MemoryMonitoring {
            bool enabled = true;
            int alert_threshold_mb = 1024;
        } memory_monitoring;
    } monitoring;
    
    // Risk management
    struct Risk {
        int max_messages_per_second = 100000;
        double max_price_deviation_percent = 10.0;
        
        struct PriceValidation {
            double min_price = 0.0001;
            double max_price = 1000000.0;
        } price_validation;
        
        struct QuantityValidation {
            int min_quantity = 1;
            int max_quantity = 10000000;
        } quantity_validation;
    } risk;
};

/**
 * @brief Configuration manager for the market data system
 * 
 * Singleton pattern to manage system-wide configuration.
 * Loads configuration from YAML files and provides access
 * to configuration parameters throughout the application.
 */
class ConfigManager {
public:
    /**
     * @brief Get the singleton instance
     */
    static ConfigManager& instance();
    
    /**
     * @brief Load configuration from YAML file
     * @param config_path Path to the configuration file
     * @throws std::runtime_error if configuration cannot be loaded
     */
    void load_config(const std::string& config_path);
    
    /**
     * @brief Get the current configuration
     * @return Reference to the configuration structure
     */
    const Config& get_config() const { return config_; }
    
    /**
     * @brief Setup logging based on configuration
     */
    void setup_logging();
    
    /**
     * @brief Validate configuration parameters
     * @throws std::runtime_error if configuration is invalid
     */
    void validate_config() const;
    
    /**
     * @brief Get specific configuration section
     */
    const Config::Global& global() const { return config_.global; }
    const Config::Feed& feed() const { return config_.feed; }
    const Config::OrderBook& orderbook() const { return config_.orderbook; }
    const Config::Publishing& publishing() const { return config_.publishing; }
    const Config::KafkaCluster& kafka_cluster() const { return config_.kafka_cluster; }
    const Config::Monitoring& monitoring() const { return config_.monitoring; }
    const Config::Risk& risk() const { return config_.risk; }
    
private:
    ConfigManager() = default;
    ~ConfigManager() = default;
    
    // Non-copyable
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;
    
    /**
     * @brief Parse YAML node into configuration structure
     */
    void parse_global(const YAML::Node& node);
    void parse_feed(const YAML::Node& node);
    void parse_orderbook(const YAML::Node& node);
    void parse_publishing(const YAML::Node& node);
    void parse_kafka_cluster(const YAML::Node& node);
    void parse_monitoring(const YAML::Node& node);
    void parse_risk(const YAML::Node& node);
    
    /**
     * @brief Helper function to safely extract values from YAML
     */
    template<typename T>
    T safe_get(const YAML::Node& node, const std::string& key, const T& default_value) const;
    
    Config config_;
    bool loaded_ = false;
};

} // namespace md_l2