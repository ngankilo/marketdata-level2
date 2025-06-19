# Multi-stage Dockerfile for Market Data Level 2 Processor

# Build stage
FROM ubuntu:22.04 AS builder

# Set environment variables
ENV DEBIAN_FRONTEND=noninteractive
ENV CMAKE_BUILD_TYPE=Release

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    wget \
    && rm -rf /var/lib/apt/lists/*

# Install libraries
RUN apt-get update && apt-get install -y \
    libflatbuffers-dev \
    libspdlog-dev \
    libyaml-cpp-dev \
    librdkafka-dev \
    libboost-system-dev \
    libboost-filesystem-dev \
    libgtest-dev \
    libgmock-dev \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy source code
COPY . .

# Create build directory and build
RUN mkdir -p build && cd build && \
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local && \
    make -j$(nproc) && \
    make install

# Runtime stage
FROM ubuntu:22.04 AS runtime

# Set environment variables
ENV DEBIAN_FRONTEND=noninteractive

# Install runtime dependencies only
RUN apt-get update && apt-get install -y \
    libflatbuffers2 \
    libspdlog1 \
    libyaml-cpp0.7 \
    librdkafka1 \
    libboost-system1.74.0 \
    libboost-filesystem1.74.0 \
    && rm -rf /var/lib/apt/lists/*

# Create application user
RUN useradd -r -s /bin/false -m -d /var/lib/market-data-l2 market-data-l2

# Create directories
RUN mkdir -p /etc/market_data_l2 \
             /var/log/market_data_l2 \
             /var/lib/market-data-l2 && \
    chown -R market-data-l2:market-data-l2 /var/log/market_data_l2 \
                                           /var/lib/market-data-l2

# Copy binary from builder stage
COPY --from=builder /usr/local/bin/market_data_l2 /usr/local/bin/
COPY --from=builder /usr/local/etc/market_data_l2/market_data.yaml /etc/market_data_l2/

# Copy configuration template
COPY config/market_data.yaml /etc/market_data_l2/market_data.yaml.template

# Create startup script
RUN cat > /usr/local/bin/start-market-data.sh << 'EOF'
#!/bin/bash
set -e

# Configuration file path
CONFIG_FILE=${CONFIG_FILE:-/etc/market_data_l2/market_data.yaml}

# Environment variable overrides
if [ ! -z "$KAFKA_BOOTSTRAP_SERVERS" ]; then
    sed -i "s/bootstrap_servers:.*/bootstrap_servers: \"$KAFKA_BOOTSTRAP_SERVERS\"/" $CONFIG_FILE
fi

if [ ! -z "$MULTICAST_GROUP" ]; then
    sed -i "s/multicast_group:.*/multicast_group: \"$MULTICAST_GROUP\"/" $CONFIG_FILE
fi

if [ ! -z "$MULTICAST_PORT" ]; then
    sed -i "s/multicast_port:.*/multicast_port: $MULTICAST_PORT/" $CONFIG_FILE
fi

if [ ! -z "$LOG_LEVEL" ]; then
    sed -i "s/log_level:.*/log_level: \"$LOG_LEVEL\"/" $CONFIG_FILE
fi

# Print configuration for debugging
echo "=== Configuration ==="
grep -E "(bootstrap_servers|multicast_group|multicast_port|log_level)" $CONFIG_FILE || true
echo "===================="

# Start the application
exec /usr/local/bin/market_data_l2 -c $CONFIG_FILE "$@"
EOF

RUN chmod +x /usr/local/bin/start-market-data.sh

# Switch to application user
USER market-data-l2

# Set working directory
WORKDIR /var/lib/market-data-l2

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD pgrep -f market_data_l2 > /dev/null || exit 1

# Expose monitoring port (if implemented)
EXPOSE 9090

# Default command
ENTRYPOINT ["/usr/local/bin/start-market-data.sh"]
CMD []

# Labels
LABEL org.opencontainers.image.title="Market Data Level 2 Processor"
LABEL org.opencontainers.image.description="High-performance market data processing system"
LABEL org.opencontainers.image.version="1.0.0"
LABEL org.opencontainers.image.vendor="Equix Technologies Pty Ltd"
LABEL org.opencontainers.image.source="https://github.com/equix-tech/market-data-l2"