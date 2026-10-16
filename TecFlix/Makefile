# Linux / macOS / MSYS2:  make && ./tecflix [port]
tecflix: src/main.cpp
	$(CXX) -std=c++17 -O2 -Wall -Wextra -o $@ $< -pthread $(if $(findstring Windows,$(OS)),-lws2_32,)
