cmake_minimum_required(VERSION 3.16)
project(MarketDataL2 VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Release build optimizations
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -DDEBUG")

# Find required packages
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# FlatBuffers
find_package(Flatbuffers REQUIRED)

# spdlog for logging
find_package(spdlog REQUIRED)

# yaml-cpp for configuration
find_package(yaml-cpp REQUIRED)

# rdkafka for Kafka integration
pkg_check_modules(RDKAFKA REQUIRED rdkafka++)

# Boost for some utilities
find_package(Boost REQUIRED COMPONENTS system filesystem)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/flatbuffers
    ${RDKAFKA_INCLUDE_DIRS}
)

# Generate FlatBuffers code
set(FB_SCHEMA ${CMAKE_SOURCE_DIR}/flatbuffers/orderbook.fbs)
set(FB_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/include/generated)
file(MAKE_DIRECTORY ${FB_OUTPUT_DIR})

add_custom_command(
    OUTPUT ${FB_OUTPUT_DIR}/orderbook_generated.h
    COMMAND flatc --cpp -o ${FB_OUTPUT_DIR} ${FB_SCHEMA}
    DEPENDS ${FB_SCHEMA}
    COMMENT "Generating FlatBuffers code"
)

# Source files
set(SOURCES
    src/main.cpp
    src/config/ConfigManager.cpp
    src/feed/MarketDataProcessor.cpp
    src/feed/PitchFeedHandler.cpp
    src/orderbook/OrderBookManager.cpp
    src/orderbook/Level2Publisher.cpp
    src/orderbook/SymbolManager.cpp
    src/utils/LatencyTracker.cpp
    src/utils/MessageQueue.cpp
    ${FB_OUTPUT_DIR}/orderbook_generated.h
)

# Main executable
add_executable(market_data_l2 ${SOURCES})

# Link libraries
target_link_libraries(market_data_l2
    PRIVATE
    Threads::Threads
    spdlog::spdlog
    yaml-cpp
    ${RDKAFKA_LIBRARIES}
    Boost::system
    Boost::filesystem
    flatbuffers::flatbuffers
)

# Compiler specific flags
target_compile_options(market_data_l2 PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    ${RDKAFKA_CFLAGS_OTHER}
)

# Install targets
install(TARGETS market_data_l2
    RUNTIME DESTINATION bin
)

install(FILES config/market_data.yaml
    DESTINATION etc/market_data_l2
)

# Tests (optional)
enable_testing()
add_subdirectory(tests EXCLUDE_FROM_ALL)