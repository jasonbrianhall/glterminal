#!/bin/bash

# Build script for FelixTerminalGUI on Fedora using g++

echo "Compiling FelixTerminalGUI..."

g++ -std=c++14 $(wx-config --cppflags) FelixTerminalGUI.cpp -o FelixTerminalGUI $(wx-config --libs)

if [ $? -eq 0 ]; then
    echo ""
    echo "Build successful!"
    echo "Run with: ./FelixTerminalGUI"
else
    echo "Build failed!"
    exit 1
fi
