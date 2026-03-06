CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -march=native
LDFLAGS  :=

# libtraceevent
CXXFLAGS += $(shell pkg-config --cflags libtraceevent 2>/dev/null)
LDFLAGS  += $(shell pkg-config --libs libtraceevent 2>/dev/null || echo "-ltraceevent")

# simdjson
CXXFLAGS += $(shell pkg-config --cflags simdjson 2>/dev/null)
LDFLAGS  += $(shell pkg-config --libs simdjson 2>/dev/null || echo "-lsimdjson")

# fmt
LDFLAGS  += -lfmt

# libelf / libdw (for future symbol resolution)
LDFLAGS  += -lelf -ldw

SRCS := src/main.cpp \
        src/perf_data_reader.cpp \
        src/viz_json_reader.cpp \
        src/merge_engine.cpp \
        src/trace_writer.cpp

OBJS := $(SRCS:.cpp=.o)
DEPS := $(OBJS:.o=.d)
TARGET := perf-viz-merge

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)
	rm -rf test/output

test: $(TARGET)
	./test/verify.sh --synthetic

-include $(DEPS)
