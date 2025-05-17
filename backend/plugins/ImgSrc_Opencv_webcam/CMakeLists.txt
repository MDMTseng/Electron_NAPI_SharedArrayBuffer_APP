cmake_minimum_required(VERSION 3.10)
project(ImgSrc_Opencv_webcam)

# Find required packages
find_package(OpenCV REQUIRED COMPONENTS core videoio imgproc)
find_package(Threads REQUIRED)
# cJSON is fetched and made available by the parent CMakeLists.txt using FetchContent.

# Add the shared library target
add_library(ImgSrc_Opencv_webcam SHARED ImgSrc_Opencv_webcam.cc)

# Set include directories
target_include_directories(ImgSrc_Opencv_webcam PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}          # For plugin source files
    ${CMAKE_SOURCE_DIR}/include         # For InspTarPluginInterface.h
    ${OpenCV_INCLUDE_DIRS}             # For OpenCV headers
    ${cjson_SOURCE_DIR}                # For cJSON headers (fetched by parent)
)

# Link libraries
target_link_libraries(ImgSrc_Opencv_webcam PRIVATE
    ${OpenCV_LIBS}
    cjson                              # Link against the cJSON library target
    Threads::Threads
)

# CMake handles the platform-specific library suffix (.dll, .so, .dylib) automatically for SHARED libraries.
# The output name will be ImgSrc_Opencv_webcam by default.