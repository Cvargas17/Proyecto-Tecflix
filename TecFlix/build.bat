@echo off
rem Builds TecFlix with MinGW-w64 g++ (MSYS2). Output: tecflix.exe
g++ -std=c++17 -O2 -Wall -Wextra -static -o tecflix.exe src\main.cpp -lws2_32 -pthread
