#include "orderbook/SymbolManager.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>

namespace md_l2 {

SymbolManager::SymbolManager() {
    SPDLOG_DEBUG("SymbolManager initialized");
}

bool SymbolManager::add_symbol(const std::string& symbol, const std::string& exchange) {
    std::unique_lock lock(mutex_);
    
    if (symbols_.find(symbol) != symbols_.end()) {
        SPDLOG_DEBUG("Symbol {} already exists", symbol);
        return false;
    }
    
    auto symbol_info = create_default_symbol_info(symbol, exchange);
    symbols_.emplace(symbol, symbol_info);
    active_symbols_.insert(symbol);
    
    SPDLOG_INFO("Added symbol: {} (exchange: {})", symbol, exchange);
    return true;
}

bool SymbolManager::add_symbol(const SymbolInfo& symbol_info) {
    std::unique_lock lock(mutex_);
    
    if (symbols_.find(symbol_info.symbol) != symbols_.end()) {
        SPDLOG_DEBUG("Symbol {} already exists", symbol_info.symbol);
        return false;
    }
    
    symbols_.emplace(symbol_info.symbol, symbol_info);
    if (symbol_info.is_active) {
        active_symbols_.insert(symbol_info.symbol);
    }
    
    SPDLOG_INFO("Added symbol: {} with custom configuration", symbol_info.symbol);
    return true;
}

bool SymbolManager::remove_symbol(const std::string& symbol) {
    std::unique_lock lock(mutex_);
    
    auto it = symbols_.find(symbol);
    if (it == symbols_.end()) {
        SPDLOG_DEBUG("Symbol {} not found for removal", symbol);
        return false;
    }
    
    symbols_.erase(it);
    active_symbols_.erase(symbol);
    
    SPDLOG_INFO("Removed symbol: {}", symbol);
    return true;
}

bool SymbolManager::has_symbol(const std::string& symbol) const {
    std::shared_lock lock(mutex_);
    total_lookups_.fetch_add(1, std::memory_order_relaxed);
    
    bool found = symbols_.find(symbol) != symbols_.end();
    if (!found) {
        failed_lookups_.fetch_add(1, std::memory_order_relaxed);
    }
    
    return found;
}

const SymbolInfo* SymbolManager::get_symbol_info(const std::string& symbol) const {
    std::shared_lock lock(mutex_);
    total_lookups_.fetch_add(1, std::memory_order_relaxed);
    
    auto it = symbols_.find(symbol);
    if (it == symbols_.end()) {
        failed_lookups_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    
    return &it->second;
}

std::vector<std::string> SymbolManager::get_active_symbols() const {
    std::shared_lock lock(mutex_);
    
    std::vector<std::string> active_list;
    active_list.reserve(active_symbols_.size());
    
    for (const auto& symbol : active_symbols_) {
        const auto& symbol_info = symbols_.at(symbol);
        if (symbol_info.is_active) {
            active_list.push_back(symbol);
        }
    }
    
    return active_list;
}

size_t SymbolManager::get_symbol_count() const {
    std::shared_lock lock(mutex_);
    return symbols_.size();
}

bool SymbolManager::set_symbol_active(const std::string& symbol, bool active) {
    std::unique_lock lock(mutex_);
    
    auto it = symbols_.find(symbol);
    if (it == symbols_.end()) {
        SPDLOG_DEBUG("Symbol {} not found for activation change", symbol);
        return false;
    }
    
    it->second.is_active = active;
    
    if (active) {
        active_symbols_.insert(symbol);
        SPDLOG_INFO("Activated symbol: {}", symbol);
    } else {
        active_symbols_.erase(symbol);
        SPDLOG_INFO("Deactivated symbol: {}", symbol);
    }
    
    return true;
}

bool SymbolManager::validate_price(const std::string& symbol, double price) const {
    std::shared_lock lock(mutex_);
    
    auto it = symbols_.find(symbol);
    if (it == symbols_.end()) {
        return false;
    }
    
    const auto& info = it->second;
    return price >= info.min_price && price <= info.max_price && price > 0.0;
}

bool SymbolManager::validate_quantity(const std::string& symbol, int quantity) const {
    std::shared_lock lock(mutex_);
    
    auto it = symbols_.find(symbol);
    if (it == symbols_.end()) {
        return false;
    }
    
    const auto& info = it->second;
    return quantity >= info.min_quantity && quantity <= info.max_quantity;
}

double SymbolManager::round_to_tick(const std::string& symbol, double price) const {
    std::shared_lock lock(mutex_);
    
    auto it = symbols_.find(symbol);
    if (it == symbols_.end()) {
        return price;  // No rounding if symbol not found
    }
    
    const auto& info = it->second;
    if (info.tick_size <= 0.0) {
        return price;
    }
    
    // Round to nearest tick
    return std::round(price / info.tick_size) * info.tick_size;
}

void SymbolManager::load_symbols(const std::vector<std::string>& symbols) {
    std::unique_lock lock(mutex_);
    
    for (const auto& symbol : symbols) {
        if (symbols_.find(symbol) == symbols_.end()) {
            auto symbol_info = create_default_symbol_info(symbol);
            symbols_.emplace(symbol, symbol_info);
            active_symbols_.insert(symbol);
        }
    }
    
    SPDLOG_INFO("Loaded {} symbols", symbols.size());
}

void SymbolManager::clear() {
    std::unique_lock lock(mutex_);
    
    size_t count = symbols_.size();
    symbols_.clear();
    active_symbols_.clear();
    
    // Reset statistics
    total_lookups_.store(0, std::memory_order_relaxed);
    failed_lookups_.store(0, std::memory_order_relaxed);
    
    SPDLOG_INFO("Cleared {} symbols", count);
}

SymbolManager::Statistics SymbolManager::get_statistics() const {
    std::shared_lock lock(mutex_);
    
    Statistics stats;
    stats.total_symbols = symbols_.size();
    stats.active_symbols = 0;
    stats.inactive_symbols = 0;
    
    for (const auto& [symbol, info] : symbols_) {
        if (info.is_active) {
            stats.active_symbols++;
        } else {
            stats.inactive_symbols++;
        }
    }
    
    return stats;
}

SymbolInfo SymbolManager::create_default_symbol_info(const std::string& symbol, 
                                                   const std::string& exchange) const {
    SymbolInfo info;
    info.symbol = symbol;
    info.exchange = exchange.empty() ? "UNKNOWN" : exchange;
    info.currency = "USD";  // Default currency
    
    // Set default properties based on symbol patterns
    if (symbol.find("BTC") != std::string::npos || symbol.find("ETH") != std::string::npos) {
        // Cryptocurrency
        info.tick_size = 0.01;
        info.min_price = 0.01;
        info.max_price = 1000000.0;
        info.min_quantity = 1;
        info.max_quantity = 1000000;
        info.price_scale = 8;  // Higher precision for crypto
        info.price_multiplier = 100000000;  // 10^8
    } else if (symbol.find("FX") != std::string::npos || symbol.length() == 6) {
        // Forex pair (e.g., EURUSD)
        info.tick_size = 0.0001;  // 1 pip
        info.min_price = 0.0001;
        info.max_price = 10.0;
        info.min_quantity = 1000;  // Standard lot in forex
        info.max_quantity = 100000000;
        info.price_scale = 5;
        info.price_multiplier = 100000;  // 10^5
    } else {
        // Equity/default
        info.tick_size = 0.01;  // 1 cent
        info.min_price = 0.01;
        info.max_price = 10000.0;
        info.min_quantity = 1;
        info.max_quantity = 10000000;
        info.price_scale = 7;
        info.price_multiplier = 10000000;  // 10^7
    }
    
    info.is_active = true;
    
    SPDLOG_DEBUG("Created default symbol info for {}: tick_size={}, price_scale={}", 
                symbol, info.tick_size, info.price_scale);
    
    return info;
}

} // namespace md_l2