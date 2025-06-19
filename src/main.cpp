#include <iostream>
#include <memory>
#include <csignal>
#include <chrono>
#include <thread>
#include <filesystem>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "config/ConfigManager.hpp"
#include "feed/MarketDataProcessor.hpp"

namespace {
    std::shared_ptr<md_l2::MarketDataProcessor> g_processor;
    std::atomic<bool> g_shutdown_requested{false};
}

/**
 * @brief Signal handler for graceful shutdown
 */
void signal_handler(int signal) {
    SPDLOG_INFO("Received signal {}, initiating graceful shutdown...", signal);
    g_shutdown_requested = true;
    
    if (g_processor) {
        g_processor->stop();
    }
}

/**
 * @brief Setup logging based on configuration
 */
void setup_logging(const md_l2::Config& config) {
    try {
        // Ensure log directory exists
        std::filesystem::create_directories(config.global.log_path);
        
        // Create rotating file sink
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            config.global.log_path + "/market_data_l2.log",
            config.global.log_max_size_mb * 1024 * 1024,
            config.global.log_max_files
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
        if (config.global.log_level == "TRACE") log_level = spdlog::level::trace;
        else if (config.global.log_level == "DEBUG") log_level = spdlog::level::debug;
        else if (config.global.log_level == "INFO") log_level = spdlog::level::info;
        else if (config.global.log_level == "WARN") log_level = spdlog::level::warn;
        else if (config.global.log_level == "ERROR") log_level = spdlog::level::err;
        else if (config.global.log_level == "CRITICAL") log_level = spdlog::level::critical;
        
        logger->set_level(log_level);
        
        // Set as default logger
        spdlog::set_default_logger(logger);
        spdlog::flush_every(std::chrono::seconds(1));
        
        SPDLOG_INFO("Logging initialized - Level: {}, Path: {}", 
                   config.global.log_level, config.global.log_path);
                   
    } catch (const std::exception& ex) {
        std::cerr << "Failed to setup logging: " << ex.what() << std::endl;
        throw;
    }
}

/**
 * @brief Print system information
 */
void print_system_info() {
    SPDLOG_INFO("=== Market Data Level 2 Processor ===");
    SPDLOG_INFO("Version: 1.0.0");
    SPDLOG_INFO("Build Date: {}", __DATE__);
    SPDLOG_INFO("Build Time: {}", __TIME__);
    
    // System information
    SPDLOG_INFO("Hardware Concurrency: {} threads", std::thread::hardware_concurrency());
    
    // Current working directory
    try {
        auto cwd = std::filesystem::current_path();
        SPDLOG_INFO("Working Directory: {}", cwd.string());
    } catch (const std::exception& ex) {
        SPDLOG_WARN("Could not determine working directory: {}", ex.what());
    }
    
    SPDLOG_INFO("=====================================");
}

/**
 * @brief Print usage information
 */
void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTIONS]\n"
              << "\nOptions:\n"
              << "  -h, --help           Show this help message\n"
              << "  -c, --config FILE    Configuration file path (default: config/market_data.yaml)\n"
              << "  -v, --version        Show version information\n"
              << "  -t, --test-config    Test configuration and exit\n"
              << "  --symbols SYMBOLS    Comma-separated list of symbols to process\n"
              << "\nExamples:\n"
              << "  " << program_name << " -c /etc/market_data/config.yaml\n"
              << "  " << program_name << " --symbols AAPL,GOOGL,MSFT\n"
              << "  " << program_name << " --test-config\n"
              << std::endl;
}

/**
 * @brief Test configuration file
 */
bool test_configuration(const std::string& config_path) {
    try {
        SPDLOG_INFO("Testing configuration file: {}", config_path);
        
        auto config_manager = std::make_shared<md_l2::ConfigManager>();
        config_manager->load_config(config_path);
        config_manager->validate_config();
        
        SPDLOG_INFO("Configuration test PASSED");
        
        // Print some key configuration values
        const auto& config = config_manager->get_config();
        SPDLOG_INFO("Feed source: {}", config.feed.source_type);
        SPDLOG_INFO("Multicast: {}:{}", config.feed.multicast_group, config.feed.multicast_port);
        SPDLOG_INFO("Kafka bootstrap: {}", config.kafka_cluster.bootstrap_servers);
        SPDLOG_INFO("Publishing enabled: {}", config.publishing.enabled);
        
        return true;
    } catch (const std::exception& ex) {
        SPDLOG_ERROR("Configuration test FAILED: {}", ex.what());
        return false;
    }
}

/**
 * @brief Monitor system performance
 */
void monitor_system_performance(std::shared_ptr<md_l2::MarketDataProcessor> processor) {
    SPDLOG_INFO("Starting performance monitoring thread");
    
    while (!g_shutdown_requested && processor->is_running()) {
        try {
            auto status = processor->get_system_status();
            auto perf_stats = processor->get_performance_stats();
            
            // Log key performance metrics every 30 seconds
            SPDLOG_INFO("=== Performance Summary ===");
            SPDLOG_INFO("System Health: {}", 
                       status.health == md_l2::SystemHealth::HEALTHY ? "HEALTHY" :
                       status.health == md_l2::SystemHealth::DEGRADED ? "DEGRADED" :
                       status.health == md_l2::SystemHealth::CRITICAL ? "CRITICAL" : "FAILED");
            
            SPDLOG_INFO("Total Throughput: {:.2f} msg/sec", status.total_throughput_msg_per_sec);
            SPDLOG_INFO("Avg E2E Latency: {:.2f} μs", status.avg_end_to_end_latency_us);
            SPDLOG_INFO("Max E2E Latency: {:.2f} μs", status.max_end_to_end_latency_us);
            SPDLOG_INFO("Memory Usage: {} MB", status.estimated_memory_usage_mb);
            SPDLOG_INFO("Active Symbols: {}", processor->get_active_symbols().size());
            SPDLOG_INFO("Total Errors: {}", status.total_errors);
            
            // Component-specific stats
            if (perf_stats.feed_stats.total_messages_received > 0) {
                SPDLOG_INFO("Feed: {:.2f} msg/sec, {} parsing errors", 
                           perf_stats.feed_stats.messages_per_second,
                           perf_stats.feed_stats.total_parsing_errors);
            }
            
            if (perf_stats.orderbook_stats.total_events_processed > 0) {
                SPDLOG_INFO("OrderBook: {} events processed, {:.2f} μs avg latency",
                           perf_stats.orderbook_stats.total_events_processed,
                           perf_stats.orderbook_stats.avg_processing_latency_us);
            }
            
            if (perf_stats.publisher_stats.total_messages_published > 0) {
                SPDLOG_INFO("Publisher: {} messages, {:.2f} MB published",
                           perf_stats.publisher_stats.total_messages_published,
                           perf_stats.publisher_stats.total_bytes_published / (1024.0 * 1024.0));
            }
            
            SPDLOG_INFO("========================");
            
        } catch (const std::exception& ex) {
            SPDLOG_ERROR("Error in performance monitoring: {}", ex.what());
        }
        
        // Wait 30 seconds before next update
        std::this_thread::sleep_for(std::chrono::seconds(30));
    }
    
    SPDLOG_INFO("Performance monitoring thread stopped");
}

/**
 * @brief Parse command line arguments
 */
struct CommandLineArgs {
    std::string config_path = "config/market_data.yaml";
    std::vector<std::string> symbols;
    bool test_config = false;
    bool show_help = false;
    bool show_version = false;
};

CommandLineArgs parse_arguments(int argc, char* argv[]) {
    CommandLineArgs args;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "-h" || arg == "--help") {
            args.show_help = true;
        } else if (arg == "-v" || arg == "--version") {
            args.show_version = true;
        } else if (arg == "-t" || arg == "--test-config") {
            args.test_config = true;
        } else if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            args.config_path = argv[++i];
        } else if (arg == "--symbols" && i + 1 < argc) {
            std::string symbols_str = argv[++i];
            // Parse comma-separated symbols
            std::stringstream ss(symbols_str);
            std::string symbol;
            while (std::getline(ss, symbol, ',')) {
                if (!symbol.empty()) {
                    args.symbols.push_back(symbol);
                }
            }
        }
    }
    
    return args;
}

/**
 * @brief Main function
 */
int main(int argc, char* argv[]) {
    try {
        // Parse command line arguments
        auto args = parse_arguments(argc, argv);
        
        if (args.show_help) {
            print_usage(argv[0]);
            return 0;
        }
        
        if (args.show_version) {
            std::cout << "Market Data Level 2 Processor v1.0.0" << std::endl;
            return 0;
        }
        
        // Initialize configuration manager
        auto config_manager = std::make_shared<md_l2::ConfigManager>();
        config_manager->load_config(args.config_path);
        
        // Setup logging early
        setup_logging(config_manager->get_config());
        
        // Print system information
        print_system_info();
        
        // Test configuration if requested
        if (args.test_config) {
            return test_configuration(args.config_path) ? 0 : 1;
        }
        
        // Validate configuration
        config_manager->validate_config();
        SPDLOG_INFO("Configuration loaded and validated successfully");
        
        // Setup signal handlers
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        
        // Create and initialize market data processor
        g_processor = std::make_shared<md_l2::MarketDataProcessor>(config_manager);
        
        if (!g_processor->initialize()) {
            SPDLOG_ERROR("Failed to initialize market data processor");
            return 1;
        }
        SPDLOG_INFO("Market data processor initialized successfully");
        
        // Add symbols from command line if specified
        if (!args.symbols.empty()) {
            SPDLOG_INFO("Adding symbols from command line: {}", 
                       fmt::join(args.symbols, ", "));
            for (const auto& symbol : args.symbols) {
                if (!g_processor->add_symbol(symbol)) {
                    SPDLOG_WARN("Failed to add symbol: {}", symbol);
                }
            }
        }
        
        // Start the processor
        if (!g_processor->start()) {
            SPDLOG_ERROR("Failed to start market data processor");
            return 1;
        }
        SPDLOG_INFO("Market data processor started successfully");
        
        // Start performance monitoring thread
        std::thread monitor_thread(monitor_system_performance, g_processor);
        
        // Main loop - wait for shutdown signal
        SPDLOG_INFO("Market Data Level 2 Processor is running. Press Ctrl+C to stop.");
        
        while (!g_shutdown_requested && g_processor->is_running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
            // Check system health
            auto health = g_processor->get_system_health();
            if (health == md_l2::SystemHealth::FAILED) {
                SPDLOG_CRITICAL("System health is FAILED, initiating emergency shutdown");
                break;
            }
        }
        
        // Graceful shutdown
        SPDLOG_INFO("Initiating graceful shutdown...");
        
        if (g_processor) {
            bool shutdown_success = g_processor->shutdown(30);  // 30 second timeout
            if (!shutdown_success) {
                SPDLOG_WARN("Graceful shutdown timed out, forcing shutdown");
            } else {
                SPDLOG_INFO("Graceful shutdown completed successfully");
            }
        }
        
        // Wait for monitoring thread to finish
        if (monitor_thread.joinable()) {
            monitor_thread.join();
        }
        
        // Final statistics
        if (g_processor) {
            auto final_stats = g_processor->get_performance_stats();
            SPDLOG_INFO("=== Final Statistics ===");
            SPDLOG_INFO("Total messages processed: {}", 
                       final_stats.feed_stats.total_messages_processed);
            SPDLOG_INFO("Total events processed: {}", 
                       final_stats.orderbook_stats.total_events_processed);
            SPDLOG_INFO("Total messages published: {}", 
                       final_stats.publisher_stats.total_messages_published);
            SPDLOG_INFO("========================");
        }
        
        SPDLOG_INFO("Market Data Level 2 Processor shutdown complete");
        return 0;
        
    } catch (const std::exception& ex) {
        if (spdlog::default_logger()) {
            SPDLOG_CRITICAL("Fatal error: {}", ex.what());
        } else {
            std::cerr << "Fatal error: " << ex.what() << std::endl;
        }
        return 1;
    } catch (...) {
        if (spdlog::default_logger()) {
            SPDLOG_CRITICAL("Unknown fatal error occurred");
        } else {
            std::cerr << "Unknown fatal error occurred" << std::endl;
        }
        return 1;
    }
}