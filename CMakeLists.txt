cmake_minimum_required(VERSION 3.20)
project(VC2HD LANGUAGES CXX)

add_executable(VC2HD WIN32 src/main.cpp)
target_compile_features(VC2HD PRIVATE cxx_std_17)

if(MSVC)
  target_compile_options(VC2HD PRIVATE /W4 /O2 /MT)
  target_link_libraries(VC2HD PRIVATE psapi)
else()
  target_compile_options(VC2HD PRIVATE -O2 -Wall -Wextra)
  target_link_options(VC2HD PRIVATE -static -static-libgcc -static-libstdc++)
  target_link_libraries(VC2HD PRIVATE psapi)
endif()
