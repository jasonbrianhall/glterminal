x86_64-w64-mingw32-g++ -o FelixTerminal.exe FelixTerminalGUI.cpp \
  -I/usr/x86_64-w64-mingw32/sys-root/mingw/include/wx-3.0 \
  -I/usr/x86_64-w64-mingw32/sys-root/mingw/lib/wx/include/x86_64-w64-mingw32-msw-unicode-3.0 \
  -L/usr/x86_64-w64-mingw32/sys-root/mingw/lib \
  -lwx_mswu_core-3.0-x86_64-w64-mingw32 \
  -lwx_baseu-3.0-x86_64-w64-mingw32 \
  -std=c++17
