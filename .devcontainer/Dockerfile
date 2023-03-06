ARG VARIANT=jammy
FROM mcr.microsoft.com/vscode/devcontainers/base:${VARIANT}

ARG USERNAME=vscode
ARG TARGETPLATFORM
ARG TARGETARCH

RUN case "${TARGETPLATFORM}" in \
    linux/amd64) break;; \
    linux/arm64) break;; \
    *) echo "unsupported platform '${TARGETPLATFORM}'"; exit 1;; \
    esac

#
# Install base dependencies
#
RUN apt-get update \
    && export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y install --no-install-recommends software-properties-common libncurses5

#
# Install LLVM
#
RUN mkdir -p /tmp/llvm \
    && cd /tmp/llvm \
    && wget https://apt.llvm.org/llvm.sh \
    && chmod +x ./llvm.sh \
    && ./llvm.sh 13 \
    && rm -rf /tmp/llvm

# Install libc++
RUN apt-get update \
    && export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y install --no-install-recommends libc++-13-dev libc++abi-13-dev

# Update path so this clang is first.
ENV PATH=/usr/lib/llvm-13/bin:${PATH}

#
# Install java
#
RUN export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y install --no-install-recommends openjdk-11-jdk

#
# Install dependencies for coverage: lcov
#
RUN export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y install --no-install-recommends lcov

#
# Install bazelisk
#
RUN wget https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-$TARGETARCH -O /usr/local/bin/bazel \
    && chmod +x /usr/local/bin/bazel

#
# Install buildifier
#
RUN wget https://github.com/bazelbuild/buildtools/releases/latest/download/buildifier-linux-$TARGETARCH -O /usr/local/bin/buildifier \
    && chmod +x /usr/local/bin/buildifier

#
# Set up command history volume
# See https://code.visualstudio.com/remote/advancedcontainers/persist-bash-history
#
RUN mkdir /commandhistory \
    && chown -R $USERNAME /commandhistory
