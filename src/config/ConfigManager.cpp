#include "config/ConfigManager.hpp"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <filesystem>
#include <stdexcept>

namespace md_l2 {

ConfigManager& ConfigManager::instance() {
    static ConfigManager instance;
    return instance;
}

void ConfigManager::load_config(const std::string& config_path) {
    try {
        SPDLOG_INFO("Loading configuration from: {}", config_path);
        
        if (!std::filesystem::exists(config_path)) {
            throw std::runtime_error("Configuration file does not exist: " + config_path);
        }
        
        YAML::Node config = YAML::LoadFile(config_path);
        
        // Parse each section
        if (config["global"]) {
            parse_global(config["global"]);
        }
        
        if (config["feed"]) {
            parse_feed(config["feed"]);
        }
        
        if (config["orderbook"]) {
            parse_orderbook(config["orderbook"]);
        }
        
        if (config["publishing"]) {
            parse_publishing(config["publishing"]);
        }
        
        if (config["kafka_cluster"]) {
            parse_kafka_cluster(config["kafka_cluster"]);
        }
        
        if (config["monitoring"]) {
            parse_monitoring(config["monitoring"]);
        }
        
        if (config["risk"]) {
            parse_risk(config["risk"]);
        }
        
        loaded_ = true;
        SPDLOG_INFO("Configuration loaded successfully");
        
    } catch (const YAML::Exception& ex) {
        throw std::runtime_error("YAML parsing error: " + std::string(ex.what()));
    } catch (const std::exception& ex) {
        throw std::runtime_error("Configuration loading error: " + std::string(ex.what()));
    }
}

void ConfigManager::parse_global(const YAML::Node& node) {
    config_.global.log_level = safe_get<std::string>(node, "log_level", "INFO");
    config_.global.log_path = safe_get<std::string>(node, "log_path", "/var/log/market_data_l2");
    config_.global.log_max_size_mb = safe_get<int>(node, "log_max_size_mb", 100);
    config_.global.log_max_files = safe_get<int>(node, "log_max_files", 50);
}

void ConfigManager::parse_feed(const YAML::Node& node) {
    config_.feed.source_type = safe_get<std::string>(node, "source_type", "PITCH");
    config_.feed.multicast_group = safe_get<std::string>(node, "multicast_group", "224.0.1.100");
    config_.feed.multicast_port = safe_get<int>(node, "multicast_port", 15000);
    config_.feed.interface = safe_get<std::string>(node, "interface", "eth0");
    config_.feed.buffer_size = safe_get<int>(node, "buffer_size", 65536);
    
    if (node["pitch"]) {
        auto pitch = node["pitch"];
        config_.feed.pitch.unit_id = safe_get<int>(pitch, "unit_id", 1);
        config_.feed.pitch.sequence_number_gap_threshold = safe_get<int>(pitch, "sequence_number_gap_threshold", 10);
        config_.feed.pitch.heartbeat_interval_ms = safe_get<int>(pitch, "heartbeat_interval_ms", 1000);
    }
}

void ConfigManager::parse_orderbook(const YAML::Node& node) {
    if (node["symbols"] && node["symbols"].IsSequence()) {
        for (const auto& symbol : node["symbols"]) {
            config_.orderbook.symbols.push_back(symbol.as<std::string>());
        }
    }
    
    config_.orderbook.initial_capacity = safe_get<int>(node, "initial_capacity", 10000);
    config_.orderbook.max_price_levels = safe_get<int>(node, "max_price_levels", 1000);
    config_.orderbook.snapshot_interval_ms = safe_get<int>(node, "snapshot_interval_ms", 1000);
    config_.orderbook.delta_interval_ms = safe_get<int>(node, "delta_interval_ms", 50);
    config_.orderbook.keep_trade_history = safe_get<bool>(node, "keep_trade_history", true);
    config_.orderbook.max_trade_history = safe_get<int>(node, "max_trade_history", 10000);
}

void ConfigManager::parse_publishing(const YAML::Node& node) {
    config_.publishing.enabled = safe_get<bool>(node, "enabled", true);
    
    if (node["formats"] && node["formats"].IsSequence()) {
        config_.publishing.formats.clear();
        for (const auto& format : node["formats"]) {
            config_.publishing.formats.push_back(format.as<std::string>());
        }
    }
    
    if (node["kafka"]) {
        auto kafka = node["kafka"];
        config_.publishing.kafka.enabled = safe_get<bool>(kafka, "enabled", true);
        config_.publishing.kafka.topic_prefix = safe_get<std::string>(kafka, "topic_prefix", "md_l2");
        config_.publishing.kafka.partition_strategy = safe_get<std::string>(kafka, "partition_strategy", "symbol_hash");
    }
    
    if (node["shared_memory"]) {
        auto shm = node["shared_memory"];
        config_.publishing.shared_memory.enabled = safe_get<bool>(shm, "enabled", false);
        config_.publishing.shared_memory.path = safe_get<std::string>(shm, "path", "/dev/shm/market_data_l2");
        config_.publishing.shared_memory.size_mb = safe_get<int>(shm, "size_mb", 256);
    }
}

void ConfigManager::parse_kafka_cluster(const YAML::Node& node) {
    config_.kafka_cluster.bootstrap_servers = safe_get<std::string>(node, "bootstrap_servers", "localhost:9092");
    config_.kafka_cluster.compression = safe_get<std::string>(node, "compression", "lz4");
    config_.kafka_cluster.acks = safe_get<std::string>(node, "acks", "1");
    config_.kafka_cluster.queue_buffering_max_messages = safe_get<int>(node, "queue_buffering_max_messages", 1000000);
    config_.kafka_cluster.batch_num_messages = safe_get<int>(node, "batch_num_messages", 10000);
    config_.kafka_cluster.linger_ms = safe_get<int>(node, "linger_ms", 5);
    config_.kafka_cluster.group_id = safe_get<std::string>(node, "group_id", "market_data_l2_gap_recovery");
    config_.kafka_cluster.session_timeout_ms = safe_get<int>(node, "session_timeout_ms", 6000);
    config_.kafka_cluster.auto_offset_reset = safe_get<std::string>(node, "auto_offset_reset", "earliest");
    config_.kafka_cluster.enable_auto_commit = safe_get<bool>(node, "enable_auto_commit", true);
    
    if (node["topics"] && node["topics"].IsSequence()) {
        config_.kafka_cluster.topics.clear();
        for (const auto& topic : node["topics"]) {
            config_.kafka_cluster.topics.push_back(topic.as<std::string>());
        }
    }
}

void ConfigManager::parse_monitoring(const YAML::Node& node) {
    config_.monitoring.enabled = safe_get<bool>(node, "enabled", true);
    config_.monitoring.stats_interval_ms = safe_get<int>(node, "stats_interval_ms", 5000);
    
    if (node["latency_tracking"]) {
        auto latency = node["latency_tracking"];
        config_.monitoring.latency_tracking.enabled = safe_get<bool>(latency, "enabled", true);
        
        if (latency["percentiles"] && latency["percentiles"].IsSequence()) {
            config_.monitoring.latency_tracking.percentiles.clear();
            for (const auto& percentile : latency["percentiles"]) {
                config_.monitoring.latency_tracking.percentiles.push_back(percentile.as<double>());
            }
        }
    }
    
    if (node["memory_monitoring"]) {
        auto memory = node["memory_monitoring"];
        config_.monitoring.memory_monitoring.enabled = safe_get<bool>(memory, "enabled", true);
        config_.monitoring.memory_monitoring.alert_threshold_mb = safe_get<int>(memory, "alert_threshold_mb", 1024);
    }
}

void ConfigManager::parse_risk(const YAML::Node& node) {
    config_.risk.max_messages_per_second = safe_get<int>(node, "max_messages_per_second", 100000);
    config_.risk.max_price_deviation_percent = safe_get<double>(node, "max_price_deviation_percent", 10.0);
    
    if (node["price_validation"]) {
        auto price = node["price_validation"];
        config_.risk.price_validation.min_price = safe_get<double>(price, "min_price", 0.0001);
        config_.risk.price_validation.max_price = safe_get<double>(price, "max_price", 1000000.0);
    }
    
    if (node["quantity_validation"]) {
        auto quantity = node["quantity_validation"];
        config_.risk.quantity_validation.min_quantity = safe_get<int>(quantity, "min_quantity", 1);
        config_.risk.quantity_validation.max_quantity = safe_get<int>(quantity, "max_quantity", 10000000);
    }
}

void ConfigManager::setup_logging() {
    try {
        // Ensure log directory exists
        std::filesystem::create_directories(config_.global.log_path);
        
        // Create rotating file sink
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            config_.global.log_path + "/market_data_l2.log",
            config_.global.log_max_size_mb * 1024 * 1024,
            config_.global.log_max_files
        );
        
        // Create console sink
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        
        // Create multi-sink logger
        auto logger = std::make_shared<spdlog::logger>("market_data_l2",
            spdlog::sinks_init_list{file_sink, console_sink});
        
        // Set log pattern
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%f] [%n] [%l] [%t] %v");
        
        // Set log level
        auto log_level = spdlog::level::info;
        if (config_.global.log_level == "TRACE") log_level = spdlog::level::trace;
        else if (config_.global.log_level == "DEBUG") log_level = spdlog::level::debug;
        else if (config_.global.log_level == "INFO") log_level = spdlog::level::info;
        else if (config_.global.log_level == "WARN") log_level = spdlog::level::warn;
        else if (config_.global.log_level == "ERROR") log_level = spdlog::level::err;
        else if (config_.global.log_level == "CRITICAL") log_level = spdlog::level::critical;
        
        logger->set_level(log_level);
        
        // Set as default logger
        spdlog::set_default_logger(logger);
        spdlog::flush_every(std::chrono::seconds(1));
        
        SPDLOG_INFO("Logging configured - Level: {}, Path: {}", 
                   config_.global.log_level, config_.global.log_path);
                   
    } catch (const std::exception& ex) {
        throw std::runtime_error("Failed to setup logging: " + std::string(ex.what()));
    }
}

void ConfigManager::validate_config() const {
    if (!loaded_) {
        throw std::runtime_error("Configuration not loaded");
    }
    
    // Validate global settings
    if (config_.global.log_max_size_mb <= 0) {
        throw std::runtime_error("log_max_size_mb must be positive");
    }
    
    if (config_.global.log_max_files <= 0) {
        throw std::runtime_error("log_max_files must be positive");
    }
    
    // Validate feed settings
    if (config_.feed.multicast_port <= 0 || config_.feed.multicast_port > 65535) {
        throw std::runtime_error("Invalid multicast port");
    }
    
    if (config_.feed.buffer_size <= 0) {
        throw std::runtime_error("buffer_size must be positive");
    }
    
    // Validate order book settings
    if (config_.orderbook.initial_capacity <= 0) {
        throw std::runtime_error("initial_capacity must be positive");
    }
    
    if (config_.orderbook.max_price_levels <= 0) {
        throw std::runtime_error("max_price_levels must be positive");
    }
    
    if (config_.orderbook.snapshot_interval_ms <= 0) {
        throw std::runtime_error("snapshot_interval_ms must be positive");
    }
    
    if (config_.orderbook.delta_interval_ms <= 0) {
        throw std::runtime_error("delta_interval_ms must be positive");
    }
    
    // Validate Kafka settings
    if (config_.kafka_cluster.bootstrap_servers.empty()) {
        throw std::runtime_error("bootstrap_servers cannot be empty");
    }
    
    if (config_.kafka_cluster.queue_buffering_max_messages <= 0) {
        throw std::runtime_error("queue_buffering_max_messages must be positive");
    }
    
    // Validate risk settings
    if (config_.risk.price_validation.min_price < 0) {
        throw std::runtime_error("min_price cannot be negative");
    }
    
    if (config_.risk.price_validation.max_price <= config_.risk.price_validation.min_price) {
        throw std::runtime_error("max_price must be greater than min_price");
    }
    
    if (config_.risk.quantity_validation.min_quantity <= 0) {
        throw std::runtime_error("min_quantity must be positive");
    }
    
    if (config_.risk.quantity_validation.max_quantity <= config_.risk.quantity_validation.min_quantity) {
        throw std::runtime_error("max_quantity must be greater than min_quantity");
    }
    
    SPDLOG_INFO("Configuration validation passed");
}

template<typename T>
T ConfigManager::safe_get(const YAML::Node& node, const std::string& key, const T& default_value) const {
    try {
        if (node[key]) {
            return node[key].as<T>();
        }
        return default_value;
    } catch (const YAML::Exception& ex) {
        SPDLOG_WARN("Failed to parse config key '{}', using default: {}", key, ex.what());
        return default_value;
    }
}

// Explicit template instantiations
template std::string ConfigManager::safe_get<std::string>(const YAML::Node&, const std::string&, const std::string&) const;
template int ConfigManager::safe_get<int>(const YAML::Node&, const std::string&, const int&) const;
template bool ConfigManager::safe_get<bool>(const YAML::Node&, const std::string&, const bool&) const;
template double ConfigManager::safe_get<double>(const YAML::Node&, const std::string&, const double&) const;

} // namespace md_l2