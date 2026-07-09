# kvstore — single Makefile, no CMake (project constraint).
#
# Targets:
#   make        -> build/kvstore
#   make test   -> build & run every tests/test_*.cpp as its own binary
#   make clean

CXX      := g++
# -pthread: the integration test drives the server with threaded clients
# (the server itself stays single-threaded).
CXXFLAGS := -std=c++17 -Wall -Wextra -Werror -g -O2 -pthread
# ?= so the Docker dev loop can compile into its own dir (build-linux),
# keeping Mac and Linux object files from trampling each other.
BUILD    ?= build

# server.cpp holds main(), so it's kept out of LIB_SRCS — test binaries link
# LIB_OBJS plus their own main.
SRCS     := $(wildcard src/*.cpp)
LIB_SRCS := $(filter-out src/server.cpp, $(SRCS))
LIB_OBJS := $(patsubst src/%.cpp, $(BUILD)/%.o, $(LIB_SRCS))

TEST_SRCS := $(wildcard tests/test_*.cpp)
TEST_BINS := $(patsubst tests/%.cpp, $(BUILD)/%, $(TEST_SRCS))

all: $(BUILD)/kvstore

$(BUILD)/kvstore: $(BUILD)/server.o $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/test_%: tests/test_%.cpp $(LIB_OBJS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LIB_OBJS)

# The load generator is a standalone CLIENT (bench/loadgen.cpp): no server
# sources, its own main, threads allowed (it's a client, not the server).
$(BUILD)/loadgen: bench/loadgen.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $<

bench: $(BUILD)/loadgen

# Tests need the server binary too: the event-loop test forks a real one
# (finding it via KVSTORE_BIN, since the build dir varies by platform).
test: all $(TEST_BINS)
	@for t in $(TEST_BINS); do KVSTORE_BIN=$(BUILD)/kvstore ./$$t || exit 1; done
	@echo "all tests passed"

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD) build-linux

# ---- Docker workflows ----
# Ship it: build the production image (compiles + runs tests inside).
docker-image:
	docker build -t kvstore .

# Run the shipped image, port mapped to the host.
docker-run:
	docker run --rm -p 6379:6379 kvstore

# Dev loop (Phase 3+, epoll needs Linux): compile & test THIS working tree
# inside a Linux container. Objects go to build-linux/, separate from the
# Mac's build/.
docker-test:
	docker run --rm -v "$(CURDIR)":/work -w /work gcc:14 make BUILD=build-linux test

# Benchmark inside one Linux container: build server + loadgen, run the
# server in the background, fire several workloads at it, tear down with the
# container. Numbers depend on the host (a Mac running Docker is slower than
# bare Linux) — treated as relative, honest figures, not lab records.
docker-bench:
	docker run --rm -v "$(CURDIR)":/work -w /work gcc:14 sh -c '\
	  make BUILD=build-linux all bench >/dev/null; \
	  { ./build-linux/kvstore --port 6400 >/dev/null 2>&1 & } && \
	  server=$$! && sleep 1 && \
	  echo "=== SET, no pipeline ===" && \
	  ./build-linux/loadgen --port 6400 --test set --requests 200000 --clients 50 --pipeline 1; \
	  echo "=== SET, pipeline 16 ===" && \
	  ./build-linux/loadgen --port 6400 --test set --requests 1000000 --clients 50 --pipeline 16; \
	  echo "=== GET, pipeline 16 ===" && \
	  ./build-linux/loadgen --port 6400 --test get --requests 1000000 --clients 50 --pipeline 16; \
	  kill $$server'

# Auto-generated header dependencies (so editing config.h rebuilds config.o).
-include $(BUILD)/*.d

.PHONY: all test bench clean docker-image docker-run docker-test docker-bench
