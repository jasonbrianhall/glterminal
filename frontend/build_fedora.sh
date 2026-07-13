#!/bin/bash

# Build script for FelixTerminalGUI on Fedora using g++

echo "Installing wxWidgets development files..."
sudo dnf install -y wxGTK-devel gcc-c++

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
