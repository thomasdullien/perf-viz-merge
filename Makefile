CXX      := g++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -march=native
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
        src/ftrc_reader.cpp \
        src/merge_engine.cpp \
        src/perfetto_writer.cpp

# libftrc (C library from pyfasttrace)
LIBFTRC_OBJ := src/libftrc.o

src/libftrc.o: src/libftrc.c src/libftrc.h
	$(CC) -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -c -o $@ $<

OBJS := $(SRCS:.cpp=.o)
DEPS := $(OBJS:.o=.d)
TARGET := perf-viz-merge

# Static build support
# Downloads simdjson and fmt sources, compiles everything into a single static binary.
STATIC_TARGET := perf-viz-merge-static
VENDOR_DIR    := vendor

# Static libs available on the system
STATIC_LIBS := /usr/lib/x86_64-linux-gnu/libtraceevent.a \
               /usr/lib/x86_64-linux-gnu/libelf.a \
               /usr/lib/x86_64-linux-gnu/libdw.a \
               /usr/lib/x86_64-linux-gnu/libz.a \
               /usr/lib/x86_64-linux-gnu/liblzma.a \
               /usr/lib/x86_64-linux-gnu/libbz2.a \
               /usr/lib/x86_64-linux-gnu/libzstd.a

STATIC_CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -march=native \
                   -I$(VENDOR_DIR)/simdjson -I$(VENDOR_DIR)/fmt/include \
                   -I/usr/include/traceevent \
                   -DSIMDJSON_THREADS_ENABLED=1 -DFMT_HEADER_ONLY=1
STATIC_LDFLAGS  := -static -lpthread

STATIC_OBJS := $(SRCS:src/%.cpp=build-static/%.o) build-static/simdjson.o

.PHONY: all clean test static

all: $(TARGET)

$(TARGET): $(OBJS) $(LIBFTRC_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# --- Static target ---
static: $(STATIC_TARGET)

$(VENDOR_DIR)/simdjson/simdjson.h $(VENDOR_DIR)/simdjson/simdjson.cpp:
	@mkdir -p $(VENDOR_DIR)/simdjson
	@echo "Downloading simdjson amalgamated source..."
	curl -sL https://raw.githubusercontent.com/simdjson/simdjson/master/singleheader/simdjson.h \
		-o $(VENDOR_DIR)/simdjson/simdjson.h
	curl -sL https://raw.githubusercontent.com/simdjson/simdjson/master/singleheader/simdjson.cpp \
		-o $(VENDOR_DIR)/simdjson/simdjson.cpp

$(VENDOR_DIR)/fmt/include/fmt/format.h:
	@mkdir -p $(VENDOR_DIR)
	@echo "Downloading fmt..."
	@if [ ! -d $(VENDOR_DIR)/fmt ]; then \
		git clone --depth 1 --branch 10.2.1 https://github.com/fmtlib/fmt.git $(VENDOR_DIR)/fmt; \
	fi

build-static/%.o: src/%.cpp $(VENDOR_DIR)/simdjson/simdjson.h $(VENDOR_DIR)/fmt/include/fmt/format.h
	@mkdir -p build-static
	$(CXX) $(STATIC_CXXFLAGS) -MMD -MP -c -o $@ $<

build-static/simdjson.o: $(VENDOR_DIR)/simdjson/simdjson.cpp $(VENDOR_DIR)/simdjson/simdjson.h
	@mkdir -p build-static
	$(CXX) $(STATIC_CXXFLAGS) -MMD -MP -c -o $@ $<

$(STATIC_TARGET): $(STATIC_OBJS)
	$(CXX) $(STATIC_CXXFLAGS) -o $@ $^ $(STATIC_LIBS) $(STATIC_LDFLAGS)
	@echo "Static binary built: $(STATIC_TARGET) ($$(du -h $(STATIC_TARGET) | cut -f1))"
	@echo "Verify: ldd $(STATIC_TARGET) should say 'not a dynamic executable'"

clean:
	rm -f $(OBJS) $(DEPS) $(LIBFTRC_OBJ) $(TARGET) $(STATIC_TARGET)
	rm -rf test/output build-static

distclean: clean
	rm -rf $(VENDOR_DIR)

test: $(TARGET)
	./test/verify.sh --synthetic

-include $(DEPS)
-include $(wildcard build-static/*.d)
