CXX      ?= g++
CXXFLAGS ?= -O2 -g -Wall -Wextra -Wno-missing-field-initializers
CXXFLAGS += -std=c++17 -pthread -I../common
LDFLAGS  += -pthread

DAEMON_OBJS = c2cd.o link.o tun.o sysinfo.o sha256.o util.o
CTL_OBJS    = c2cctl.o sha256.o util.o

all: c2cd c2cctl

c2cd: $(DAEMON_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^

c2cctl: $(CTL_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^

%.o: %.cpp $(wildcard *.hpp) ../common/c2c_proto.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f *.o c2cd c2cctl

.PHONY: all clean
