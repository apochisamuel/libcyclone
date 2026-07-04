CC       ?= gcc
CXX      ?= g++
CFLAGS   ?= -Wall -Wextra -Wpedantic -std=c11 -O2
CXXFLAGS ?= -Wall -Wextra -std=c++17 -O2
AR       ?= ar

SRC_DIR  = src
FUZZ_DIR = fuzz
BUILD    = build

SRCS = $(SRC_DIR)/context.c \
       $(SRC_DIR)/reader.c \
       $(SRC_DIR)/header.c \
       $(SRC_DIR)/strings.c \
       $(SRC_DIR)/types.c \
       $(SRC_DIR)/nodes.c \
       $(SRC_DIR)/ports.c \
       $(SRC_DIR)/edges.c \
       $(SRC_DIR)/params.c \
       $(SRC_DIR)/schedule.c \
       $(SRC_DIR)/groups.c \
       $(SRC_DIR)/cache.c \
       $(SRC_DIR)/routing.c \
       $(SRC_DIR)/validate.c \
       $(SRC_DIR)/resolve.c \
       $(SRC_DIR)/query.c \
       $(SRC_DIR)/format.c

OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(SRCS))

LIB = $(BUILD)/libcyclone.a

FUZZERS = $(BUILD)/parse_fuzzer \
          $(BUILD)/edge_fuzzer \
          $(BUILD)/resolve_fuzzer

.PHONY: all clean fuzzers

all: $(LIB)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c $< -o $@

$(LIB): $(OBJS)
	$(AR) rcs $@ $(OBJS)

fuzzers: $(FUZZERS)

$(BUILD)/parse_fuzzer: $(FUZZ_DIR)/parse_fuzzer.cc $(LIB)
	$(CXX) $(CXXFLAGS) $(LIB_FUZZING_ENGINE) -I$(SRC_DIR) $< $(LIB) -o $@

$(BUILD)/edge_fuzzer: $(FUZZ_DIR)/edge_fuzzer.cc $(LIB)
	$(CXX) $(CXXFLAGS) $(LIB_FUZZING_ENGINE) -I$(SRC_DIR) $< $(LIB) -o $@

$(BUILD)/resolve_fuzzer: $(FUZZ_DIR)/resolve_fuzzer.cc $(LIB)
	$(CXX) $(CXXFLAGS) $(LIB_FUZZING_ENGINE) -I$(SRC_DIR) $< $(LIB) -o $@

clean:
	rm -rf $(BUILD)
