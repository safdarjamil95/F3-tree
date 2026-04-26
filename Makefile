.PHONY: all clean smoke bench hash evaluator-sweep evaluator-report restart-pmdk

CXX ?= g++
CXXFLAGS ?= -std=c++11 -O2 -g -Wall -Wextra -pedantic
CPPFLAGS ?= -Iinclude
LDFLAGS ?=
LDLIBS ?= -lpthread -lrt -lm

# PMDK backend opt-in: make PMDK=1
#
# Enables the libpmemobj-backed persistent store. When F3_PMDK is NOT
# defined the codebase builds as DRAM-only. When F3_PMDK is defined
# AND RuntimeConfig::persistent_path is non-empty, B+-tree state lives
# in a libpmemobj pool at that path and survives process restart; when
# the path is empty the PMDK backend falls back to DRAM behaviour.
ifeq ($(PMDK),1)
CPPFLAGS += -DF3_PMDK $(shell pkg-config --cflags libpmemobj++ libpmemobj libpmem 2>/dev/null)
LDLIBS   += $(shell pkg-config --libs libpmemobj++ libpmemobj libpmem 2>/dev/null)
endif

BIN_DIR := bin
# src/buffered_hash_index.cpp is intentionally NOT part of COMMON_SOURCES.
# It is a standalone benchmark harness not used by the canonical FutureBTree
# correctness path. Only the hash_benchmark target links it in explicitly below.
COMMON_SOURCES := src/f3tree.cpp src/workload.cpp

LIB_OBJECTS := $(BIN_DIR)/f3tree.o $(BIN_DIR)/workload.o

all: $(BIN_DIR)/baseline_benchmark $(BIN_DIR)/future_benchmark $(BIN_DIR)/mixed_benchmark $(BIN_DIR)/hash_benchmark $(BIN_DIR)/smoke_test $(BIN_DIR)/restart_test

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/baseline_benchmark: bench/baseline_benchmark.cpp $(COMMON_SOURCES) | $(BIN_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ bench/baseline_benchmark.cpp $(COMMON_SOURCES) $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/future_benchmark: bench/future_benchmark.cpp $(COMMON_SOURCES) | $(BIN_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ bench/future_benchmark.cpp $(COMMON_SOURCES) $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/mixed_benchmark: bench/mixed_benchmark.cpp $(COMMON_SOURCES) | $(BIN_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ bench/mixed_benchmark.cpp $(COMMON_SOURCES) $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/hash_benchmark: bench/hash_benchmark.cpp src/buffered_hash_index.cpp | $(BIN_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ bench/hash_benchmark.cpp src/buffered_hash_index.cpp $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/smoke_test: tests/smoke.cpp $(COMMON_SOURCES) | $(BIN_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ tests/smoke.cpp $(COMMON_SOURCES) $(LDFLAGS) $(LDLIBS)

$(BIN_DIR)/restart_test: tests/restart_test.cpp $(COMMON_SOURCES) | $(BIN_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ tests/restart_test.cpp $(COMMON_SOURCES) $(LDFLAGS) $(LDLIBS)

# Static library targets — built separately for DRAM and PMDK configurations
# so a `make lib` and `make lib PMDK=1` can coexist in bin/ without
# invalidating each other via timestamp races.
ifeq ($(PMDK),1)
LIB_NAME := libf3tree_pmdk.a
else
LIB_NAME := libf3tree.a
endif

LIB_BACKEND_OBJ := $(BIN_DIR)/$(LIB_NAME:.a=)_backend.o
LIB_WORKLOAD_OBJ := $(BIN_DIR)/$(LIB_NAME:.a=)_workload.o

$(LIB_BACKEND_OBJ): src/f3tree.cpp | $(BIN_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(LIB_WORKLOAD_OBJ): src/workload.cpp | $(BIN_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BIN_DIR)/$(LIB_NAME): $(LIB_BACKEND_OBJ) $(LIB_WORKLOAD_OBJ) | $(BIN_DIR)
	ar rcs $@ $^

lib: $(BIN_DIR)/$(LIB_NAME)

$(BIN_DIR)/f3tree.o: src/f3tree.cpp | $(BIN_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BIN_DIR)/workload.o: src/workload.cpp | $(BIN_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

smoke: $(BIN_DIR)/smoke_test $(BIN_DIR)/restart_test
	./$(BIN_DIR)/smoke_test
	./$(BIN_DIR)/restart_test

bench: $(BIN_DIR)/baseline_benchmark $(BIN_DIR)/future_benchmark $(BIN_DIR)/mixed_benchmark

hash: $(BIN_DIR)/hash_benchmark

clean:
	rm -rf $(BIN_DIR)

# Cross-process restart test. Requires make PMDK=1.
# Writes the pool under /dev/shm so the tmpfs-backed file survives
# between the write and verify phases.
restart-pmdk: $(BIN_DIR)/restart_test
	./scripts/run_restart_test.sh

evaluator-sweep: $(BIN_DIR)/future_benchmark $(BIN_DIR)/mixed_benchmark
	./scripts/run_evaluator_sweep.sh

evaluator-report:
	./scripts/summarize_evaluator_sweep.py evaluator_sweep_results.csv --output-dir evaluator_report
