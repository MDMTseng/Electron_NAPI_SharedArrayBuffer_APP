#!/bin/bash

# Function to get number of CPU cores (works on both Linux and macOS)
get_cpu_cores() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    else
        sysctl -n hw.ncpu
    fi
}

# Function to check if path has changed
check_path_change() {
    local current_path=$(pwd)
    local last_path_file="build/.last_build_path"
    
    if [ -f "$last_path_file" ]; then
        local last_path=$(cat "$last_path_file")
        if [ "$current_path" != "$last_path" ]; then
            echo "Build path has changed. Cleaning previous build..."
            rm -rf build
            mkdir -p build
            echo "$current_path" > "$last_path_file"
            return 0
        fi
    else
        mkdir -p build
        echo "$current_path" > "$last_path_file"
    fi
    return 1
}

# Function to initialize build directory
init_build() {
    if [ ! -d "build" ]; then
        echo "Creating build directory..."
        mkdir -p build
    fi
    
    # Always remove build directory to ensure clean cmake run
    echo "Cleaning build directory..."
    rm -rf build
    mkdir -p build
    
    echo "Running cmake..."
    cd build || exit 1
    cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ..
    cd ..
}

# Main build function
build_project() {
    echo "Building project..."
    cd build || exit 1
    make -j$(get_cpu_cores)
    local build_status=$?
    cd ..
    return $build_status
}

# Main execution
main() {
    # Check if path has changed
    check_path_change
    
    # Initialize build directory if needed
    init_build
    
    # Build the project
    if build_project; then
        echo "Build completed successfully!"
    else
        echo "Build failed!"
        exit 1
    fi
}

# Run main function
main
