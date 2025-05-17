#!/bin/bash
set -euo pipefail # Exit on error, undefined variable, pipe failure

echo "Starting APP build process..."

# --- Configuration ---
# Script is now inside APP/, paths are relative to APP/
APP_DIR=$(pwd) # Assume script is run from APP/ directory
PROJECT_ROOT="${APP_DIR}/.." # Path to the project root
FRONTEND_DIR="frontend"
BACKEND_DIR="backend"
DIST_DIR="dist" # Output directory relative to APP/

# --- Cleanup and Setup ---
echo "Cleaning and creating distribution directory: ${DIST_DIR}"
rm -rf "${DIST_DIR}"
mkdir -p "${DIST_DIR}/frontend"
mkdir -p "${DIST_DIR}/backend"
mkdir -p "${DIST_DIR}/native" # <<< Create native subdir

# --- Build Frontend ---
echo "Building frontend in ${FRONTEND_DIR}..."
cd "${APP_DIR}/${FRONTEND_DIR}"
# Assuming dependencies are already installed. If not, uncomment the next line:
# echo "Installing frontend dependencies..."
# npm install
echo "Running frontend build..."
npm run build
cd "${APP_DIR}" # Go back to APP dir

echo "Copying frontend artifacts to ${DIST_DIR}/frontend..."
cp -R "${FRONTEND_DIR}/dist/"* "${DIST_DIR}/frontend/"

# --- Build Backend Plugin ---
echo "Building backend plugin (${BACKEND_DIR})..."
# Ensure build script is executable
chmod +x "${BACKEND_DIR}/build.sh"

# Execute build script directly (assuming npm not needed just for this)
cd "${APP_DIR}/${BACKEND_DIR}"
echo "Running backend build script..."
./build.sh
cd "${APP_DIR}" # Go back to APP dir

echo "Copying backend plugin artifacts to ${DIST_DIR}/backend..."
PLUGIN_BUILD_DIR="${BACKEND_DIR}/build/lib"
PLUGIN_TARGET_DIR="${DIST_DIR}/backend"
PLUGIN_COPIED=false
if [[ -f "${PLUGIN_BUILD_DIR}/libsample_plugin.dylib" ]]; then
    cp "${PLUGIN_BUILD_DIR}/libsample_plugin.dylib" "${PLUGIN_TARGET_DIR}/"
    PLUGIN_COPIED=true
    echo "Copied libsample_plugin.dylib"
elif [[ -f "${PLUGIN_BUILD_DIR}/libsample_plugin.so" ]]; then
    cp "${PLUGIN_BUILD_DIR}/libsample_plugin.so" "${PLUGIN_TARGET_DIR}/"
    PLUGIN_COPIED=true
    echo "Copied libsample_plugin.so"
elif [[ -f "${PLUGIN_BUILD_DIR}/sample_plugin.dll" ]]; then # Note: No 'lib' prefix on Windows by default
    cp "${PLUGIN_BUILD_DIR}/sample_plugin.dll" "${PLUGIN_TARGET_DIR}/"
    PLUGIN_COPIED=true
    echo "Copied sample_plugin.dll"
fi

# Copy ImgSrc_Opencv_webcam plugin library
if [[ -f "${PLUGIN_BUILD_DIR}/libImgSrc_Opencv_webcam.dylib" ]]; then
    cp "${PLUGIN_BUILD_DIR}/libImgSrc_Opencv_webcam.dylib" "${PLUGIN_TARGET_DIR}/"
    echo "Copied libImgSrc_Opencv_webcam.dylib"
elif [[ -f "${PLUGIN_BUILD_DIR}/libImgSrc_Opencv_webcam.so" ]]; then
    cp "${PLUGIN_BUILD_DIR}/libImgSrc_Opencv_webcam.so" "${PLUGIN_TARGET_DIR}/"
    echo "Copied libImgSrc_Opencv_webcam.so"
elif [[ -f "${PLUGIN_BUILD_DIR}/ImgSrc_Opencv_webcam.dll" ]]; then
    cp "${PLUGIN_BUILD_DIR}/ImgSrc_Opencv_webcam.dll" "${PLUGIN_TARGET_DIR}/"
    echo "Copied ImgSrc_Opencv_webcam.dll"
else
    echo "Warning: ImgSrc_Opencv_webcam plugin library not found in ${PLUGIN_BUILD_DIR}"
fi

if [ "$PLUGIN_COPIED" = false ]; then
    echo "Error: Backend plugin library not found in ${PLUGIN_BUILD_DIR}"
    exit 1
fi

# Copy the Python script needed by the backend plugin
PYTHON_SCRIPT="python_bidirectional_ipc_script.py"
if [[ -f "${BACKEND_DIR}/${PYTHON_SCRIPT}" ]]; then
    cp "${BACKEND_DIR}/${PYTHON_SCRIPT}" "${PLUGIN_TARGET_DIR}/"
    echo "Copied ${PYTHON_SCRIPT}"
else
    echo "Warning: Python IPC script ${PYTHON_SCRIPT} not found in ${BACKEND_DIR}"
fi

# --- Build Native Addon (NEW) ---
echo "Building native addon..."
cd "${PROJECT_ROOT}" # Go up to project root
node-gyp rebuild
cd "${APP_DIR}" # Go back to APP dir

echo "Copying native addon artifact to ${DIST_DIR}/native..."
NATIVE_BUILD_DIR="${PROJECT_ROOT}/build/Release" # Build output is in project root's build dir
NATIVE_TARGET_DIR="${DIST_DIR}/native"
if [[ -f "${NATIVE_BUILD_DIR}/addon.node" ]]; then
    cp "${NATIVE_BUILD_DIR}/addon.node" "${NATIVE_TARGET_DIR}/"
    echo "Copied addon.node"
else
    echo "Error: Native addon (addon.node) not found in ${NATIVE_BUILD_DIR}"
    exit 1
fi

# --- Finish ---
echo "-------------------------------------"
echo "APP build finished successfully!"
echo "Artifacts are located in: ${DIST_DIR}"
echo "Contents:"
ls -R "${DIST_DIR}"
echo "-------------------------------------"

exit 0 