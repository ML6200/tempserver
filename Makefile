CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread

all: dht_server

dht_server: dht_server.cpp dashboard.cpp
	$(CXX) $(CXXFLAGS) dht_server.cpp dashboard.cpp -o dht_server

clean:
	rm -f dht_server

.PHONY: all clean
