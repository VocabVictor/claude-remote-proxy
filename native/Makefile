CXX ?= c++
CXXFLAGS ?= -O3 -DNDEBUG -std=c++17 -Wall -Wextra -Wpedantic

all: claude-remote-proxy-native

claude-remote-proxy-native: proxy.cpp http_connect.hpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f claude-remote-proxy-native

.PHONY: all clean
