# Continuous fuzzing container for Donner.
#
# Long-lived container that clones the repo from GitHub, builds fuzzer
# targets from the latest main, and runs continuous fuzzing on a loop.
# Persistent state (corpus, crash ledger, run history) lives in a volume.
#
# Build:
#   docker build -t donner-fuzz -f tools/fuzzing/Dockerfile .
#
# Run:
#   docker compose -f tools/fuzzing/docker-compose.yml up -d

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# System dependencies for Bazel + clang + fuzzing
RUN apt-get update && apt-get install -y --no-install-recommends \
    bash \
    ca-certificates \
    clang \
    curl \
    git \
    libc++-dev \
    libc++abi-dev \
    libstdc++-14-dev \
    lld \
    python3 \
    python3-minimal \
    && rm -rf /var/lib/apt/lists/*

# Install Bazelisk (auto-downloads the right Bazel version)
RUN curl -fsSL -o /usr/local/bin/bazel \
    "https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-$(dpkg --print-architecture)" \
    && chmod +x /usr/local/bin/bazel

# Install gh CLI for crash reporting
RUN curl -fsSL https://cli.github.com/packages/githubcli-archive-keyring.gpg \
    | dd of=/usr/share/keyrings/githubcli-archive-keyring.gpg \
    && echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" \
    > /etc/apt/sources.list.d/github-cli.list \
    && apt-get update && apt-get install -y --no-install-recommends gh \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user for fuzzing
RUN useradd -m -s /bin/bash fuzzer
USER fuzzer
WORKDIR /home/fuzzer

# Persistent state volume: corpus, crash ledger, run history, bazel cache
VOLUME /home/fuzzer/.donner-fuzz
VOLUME /home/fuzzer/.cache/bazel

# The repo is cloned at first start by the entrypoint, then kept up to
# date with git fetch + reset to origin/main before each fuzzing run.
# Bazel builds happen inside run_continuous_fuzz.py on each iteration.
COPY --chown=fuzzer:fuzzer tools/fuzzing/entrypoint.sh /home/fuzzer/entrypoint.sh

ENTRYPOINT ["/home/fuzzer/entrypoint.sh"]
