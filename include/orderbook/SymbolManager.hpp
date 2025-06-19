#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
#include <atomic>
#include <vector>

namespace md_l2 {

/**
 * @brief Symbol information structure
 */
struct SymbolInfo {
    std::string symbol;
    std::string exchange;
    std::string currency;
    double tick_size = 0.01;
    double min_price = 0.0001;
    double max_price = 1000000.0;
    int min_quantity = 1;
    int max_quantity = 10000000;
    bool is_active = true;
    
    // Price scaling for integer representation
    int price_scale = 7;  // 7 decimal places
    uint64_t price_multiplier = 10000000;  // 10^7
    
    // Convert double price to scaled integer
    uint64_t scale_price(double price) const {
        return static_cast<uint64_t>(price * price_multiplier + 0.5);
    }
    
    // Convert scaled integer to double price
    double unscale_price(uint64_t scaled_price) const {
        return static_cast<double>(scaled_price) / price_multiplier;
    }
};

/**
 * @brief Symbol manager for handling symbol metadata and validation
 * 
 * Thread-safe management of trading symbols, their properties,
 * and validation rules. Supports dynamic symbol addition/removal
 * and efficient lookups.
 */
class SymbolManager {
public:
    SymbolManager();
    ~SymbolManager() = default;
    
    // Non-copyable but movable
    SymbolManager(const SymbolManager&) = delete;
    SymbolManager& operator=(const SymbolManager&) = delete;
    SymbolManager(SymbolManager&&) = default;
    SymbolManager& operator=(SymbolManager&&) = default;
    
    /**
     * @brief Add a new symbol with default properties
     * @param symbol Symbol name (e.g., "AAPL", "BTCUSDT")
     * @param exchange Exchange name (e.g., "NASDAQ", "BINANCE")
     * @return true if symbol was added, false if already exists
     */
    bool add_symbol(const std::string& symbol, const std::string& exchange = "");
    
    /**
     * @brief Add a symbol with custom properties
     * @param symbol_info Complete symbol information
     * @return true if symbol was added, false if already exists
     */
    bool add_symbol(const SymbolInfo& symbol_info);
    
    /**
     * @brief Remove a symbol
     * @param symbol Symbol to remove
     * @return true if symbol was removed, false if not found
     */
    bool remove_symbol(const std::string& symbol);
    
    /**
     * @brief Check if symbol exists
     * @param symbol Symbol to check
     * @return true if symbol exists
     */
    bool has_symbol(const std::string& symbol) const;
    
    /**
     * @brief Get symbol information
     * @param symbol Symbol name
     * @return Pointer to symbol info, nullptr if not found
     */
    const SymbolInfo* get_symbol_info(const std::string& symbol) const;
    
    /**
     * @brief Get all active symbols
     * @return Vector of active symbol names
     */
    std::vector<std::string> get_active_symbols() const;
    
    /**
     * @brief Get symbol count
     * @return Number of symbols managed
     */
    size_t get_symbol_count() const;
    
    /**
     * @brief Activate/deactivate a symbol
     * @param symbol Symbol name
     * @param active New active status
     * @return true if status was changed
     */
    bool set_symbol_active(const std::string& symbol, bool active);
    
    /**
     * @brief Validate price for a symbol
     * @param symbol Symbol name
     * @param price Price to validate
     * @return true if price is valid
     */
    bool validate_price(const std::string& symbol, double price) const;
    
    /**
     * @brief Validate quantity for a symbol
     * @param symbol Symbol name
     * @param quantity Quantity to validate
     * @return true if quantity is valid
     */
    bool validate_quantity(const std::string& symbol, int quantity) const;
    
    /**
     * @brief Round price to nearest tick for a symbol
     * @param symbol Symbol name
     * @param price Price to round
     * @return Rounded price
     */
    double round_to_tick(const std::string& symbol, double price) const;
    
    /**
     * @brief Load symbols from configuration
     * @param symbols Vector of symbol names to add
     */
    void load_symbols(const std::vector<std::string>& symbols);
    
    /**
     * @brief Clear all symbols
     */
    void clear();
    
    /**
     * @brief Get statistics
     */
    struct Statistics {
        size_t total_symbols = 0;
        size_t active_symbols = 0;
        size_t inactive_symbols = 0;
    };
    
    Statistics get_statistics() const;
    
private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, SymbolInfo> symbols_;
    std::unordered_set<std::string> active_symbols_;
    
    // Statistics
    mutable std::atomic<size_t> total_lookups_{0};
    mutable std::atomic<size_t> failed_lookups_{0};
    
    /**
     * @brief Create default symbol info
     */
    SymbolInfo create_default_symbol_info(const std::string& symbol, 
                                         const std::string& exchange = "") const;
};

} // namespace md_l2