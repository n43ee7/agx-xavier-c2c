CXX      ?= g++
CXXFLAGS ?= -O2 -g -Wall -Wextra -Wno-missing-field-initializers
CXXFLAGS += -std=c++17 -pthread
LDFLAGS  += -pthread

all: c2c-memd c2c-rmem

c2c-memd: c2c-memd.cpp rmem_proto.hpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

c2c-rmem: c2c-rmem.cpp rmem_proto.hpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f c2c-memd c2c-rmem

.PHONY: all clean
