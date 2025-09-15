CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2

all: server client tester   # <-- add tester here

server: server.cpp
	$(CXX) $(CXXFLAGS) -pthread server.cpp -lsqlite3 -o server

client: client.cpp
	$(CXX) $(CXXFLAGS) client.cpp -o client

tester: tester.cpp
	$(CXX) $(CXXFLAGS) tester.cpp -o tester

clean:
	rm -f server client tester
