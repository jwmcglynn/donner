ARG VARIANT=hirsute
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
    && apt-get -y install --no-install-recommends software-properties-common

#
# Install LLVM
#
RUN mkdir -p /tmp/llvm \
    && cd /tmp/llvm \
    && wget https://apt.llvm.org/llvm.sh \
    && chmod +x ./llvm.sh \
    && ./llvm.sh 11 \
    && rm -rf /tmp/llvm

# Install libc++
RUN apt-get update \
    && export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y install --no-install-recommends libc++-11-dev libc++abi-11-dev

# Update path so this clang is first.
ENV PATH=/usr/lib/llvm-11/bin:${PATH}

#
# Install dependencies for coverage: gcov-7 (from gcc), lcov, java
#
RUN export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y install --no-install-recommends gcc-7 lcov openjdk-11-jdk

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
