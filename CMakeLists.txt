cmake_minimum_required(VERSION 3.24)
project(condor3VRCockpit LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Condor.exe is 32-bit, so the layer must be too. Configure with -A Win32 (cmake --preset win32).")
endif()

add_subdirectory(app)
add_subdirectory(layer)
add_subdirectory(tools/ovrprobe)
