# Spike-only Makefile. The Bloomberg clang-p2996 fork is non-hermetic and not
# wired into Bazel yet (see SPIKE_NOTES.md "Why a Makefile, not Bazel"), so
# this is the intended way to build the spike binary inside the
# `vsavkov/clang-p2996` Docker image.
#
# Usage (from inside the image, with the repo bind-mounted at /work):
#   cd /work/donner/editor/ipc/spike && make && ./teleport_spike

CXX ?= clang++
CXXFLAGS ?= -std=c++26 -freflection-latest -fexpansion-statements -O2 -Wall -Wextra

teleport_spike: teleport_spike.cc render_request.h teleport_codec.h
	$(CXX) $(CXXFLAGS) -o $@ teleport_spike.cc

.PHONY: clean asan
clean:
	rm -f teleport_spike

asan: CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -g
asan: clean teleport_spike
