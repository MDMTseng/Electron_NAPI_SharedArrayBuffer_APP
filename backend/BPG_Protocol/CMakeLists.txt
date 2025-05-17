cmake_minimum_required(VERSION 3.10)

project(BPG_Protocol CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Define the BPG library
add_library(bpg_protocol STATIC # Or SHARED
    bpg_encoder.cpp
    bpg_decoder.cpp
    # Add other source files if needed (e.g., specific link layer implementations)
)

# Public include directories for consumers of this library
target_include_directories(bpg_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Example of how to link against this library (in a parent CMakeLists.txt):
# add_subdirectory(native/plugins/BPG_Protocol)
# target_link_libraries(your_executable PRIVATE bpg_protocol)

# Optional: Add tests using CTest if desired
# enable_testing()
# add_executable(bpg_tests tests/test_main.cpp tests/test_encoder.cpp tests/test_decoder.cpp)
# target_link_libraries(bpg_tests PRIVATE bpg_protocol gtest gtest_main) # Assuming GoogleTest
# add_test(NAME BpgUnitTests COMMAND bpg_tests)

# Find OpenCV package
find_package(OpenCV REQUIRED)

# Include OpenCV headers (optional, but good practice for clarity)
include_directories(${OpenCV_INCLUDE_DIRS})

# Add the test application executable
add_executable(bpg_test_app tests/bpg_test_app.cpp)
target_link_libraries(bpg_test_app PRIVATE bpg_protocol ${OpenCV_LIBS})

# Define installation rules if needed
# install(TARGETS bpg_protocol DESTINATION lib)
# install(FILES bpg_types.h bpg_encoder.h bpg_decoder.h DESTINATION include/bpg_protocol) 