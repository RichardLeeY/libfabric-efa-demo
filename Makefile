CXX       = g++
CXXFLAGS  = -Wall -Werror -std=c++17 -march=native -O2 -g -Ibuild/libfabric/include -I/usr/local/cuda/include
LDFLAGS   = -Lbuild/libfabric/lib -L/usr/local/cuda/lib64
LDLIBS    = -lfabric -lpthread
BINARIES  = build/16_write_cpu

export LD_LIBRARY_PATH := $(PWD)/build/libfabric/lib:$(LD_LIBRARY_PATH)

.PHONY: all clean

all: $(BINARIES)

clean:
        rm -rf $(BINARIES)

build/%: src/%.cpp build/libfabric/lib/libfabric.so
        $(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)