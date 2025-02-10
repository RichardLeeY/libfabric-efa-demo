CXX       = g++
CXXFLAGS  = -Wall -Werror -std=c++17 -march=native -O2 -g -Ibuild/libfabric/include -I/usr/local/cuda/include
LDFLAGS   = -Lbuild/libfabric/lib -L/usr/local/cuda/lib64
LDLIBS    = -lfabric -lpthread -lcudart -lcuda
BINARIES  = build/4_hello \
			build/5_reverse \
			build/6_write \
			build/7_queue \
			build/8_topo \
			build/9_multinet \
			build/10_warmup \
			build/11_multithread \
			build/12_pin \
			build/13_shard \
			build/14_batch \
			build/15_lazy

export LD_LIBRARY_PATH := $(PWD)/build/libfabric/lib:$(LD_LIBRARY_PATH)

.PHONY: all clean

all: $(BINARIES)

clean:
	rm -rf $(BINARIES)

build/%: src/%.cpp build/libfabric/lib/libfabric.so
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)
