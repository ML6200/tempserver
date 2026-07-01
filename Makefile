CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread

# Version string + build number (git commit count, falls back to 0 outside a repo).
# Kept separate from CXXFLAGS so overriding CXXFLAGS on the command line doesn't
# drop them.
VERSION      := 1.0.0
BUILD_NUMBER := $(shell git rev-list --count HEAD 2>/dev/null || echo 0)
VERSION_FLAGS := -DAPP_VERSION='"$(VERSION)"' -DBUILD_NUMBER='"$(BUILD_NUMBER)"'

all: dht_server

dht_server: dht_server.cpp config.h
	$(CXX) $(CXXFLAGS) $(VERSION_FLAGS) dht_server.cpp -o dht_server

clean:
	rm -f dht_server

.PHONY: all clean
