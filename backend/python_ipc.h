#ifndef PYTHON_IPC_H
#define PYTHON_IPC_H

#include <string>
#include <vector>
#include <atomic>
#include <cstddef> // For size_t
#include <cstdint> // For int32_t
#include <functional> // For callback type

// --- Configuration ---
// Name for the shared memory segment (must match Python script)
#define SHM_NAME_BI "/electron_python_shm_bi_123" // Bi-directional

// MAX size definitions for EACH buffer direction
const size_t SHM_C2A_BUFFER_MAX_SIZE = 1024;           // Creator -> Acceptor (TX)
const size_t SHM_A2C_BUFFER_MAX_SIZE = 1024 * 1024 * 2; // Acceptor -> Creator (RX)

// --- Shared Memory Structure ---
// IMPORTANT: Ensure total size and layout (including padding) EXACTLY 
// matches the Python ctypes.Structure definition.
struct SharedIPCBidirectional {
    // --- Control block --- 
    // Atomics for status/commands
    std::atomic<int32_t> c_to_a_command;  // 0: Idle, 1: Data Ready, 99: Shutdown
    std::atomic<size_t>  c_to_a_data_len;
    std::atomic<int32_t> a_to_c_status;   // 0: Idle, 1: Data Ready, -1: Error
    std::atomic<size_t>  a_to_c_data_len;
    // Defined buffer sizes (set by Creator, read by Acceptor)
    size_t defined_c2a_buffer_size; // Actual usable size for buffer_c_to_a
    size_t defined_a2c_buffer_size; // Actual usable size for buffer_a_to_c
    
    // Padding to ensure alignment and consistent control block size.
    // Let's pad to 128 bytes for potential cache alignment benefits.
    // Calculation assumes atomic<int32> is 4, atomic<size_t> is 8, size_t is 8.
    // Current size = 4*2 + 8*2 + 8*2 = 8 + 16 + 16 = 40 bytes.
    // Padding needed = 128 - 40 = 88 bytes.
    char _padding1[128 - sizeof(std::atomic<int32_t>)*2 - sizeof(std::atomic<size_t>)*2 - sizeof(size_t)*2]; 
    // --- End Control Block (Total 128 bytes) ---

    // Data Buffers (Allocated based on MAX size constants)
    char buffer_c_to_a[SHM_C2A_BUFFER_MAX_SIZE]; // Use TX max size
    char buffer_a_to_c[SHM_A2C_BUFFER_MAX_SIZE]; // Use RX max size
};

// Ensure the struct size calculation reflects the padding goal
static_assert(sizeof(SharedIPCBidirectional::_padding1) == (128 - sizeof(std::atomic<int32_t>)*2 - sizeof(std::atomic<size_t>)*2 - sizeof(size_t)*2),
              "Padding calculation error in SharedIPCBidirectional");

// --- Callback Type for Received Data ---
// This callback will be invoked by the C++ listener thread when data arrives from Python.
// It needs to handle copying the data and likely forwarding it to the JS layer.
// IMPORTANT: This runs in a separate thread, ensure thread safety in the callback implementation!
typedef std::function<void(const uint8_t* data, size_t length)> AcceptorDataCallback;

// --- IPC Management Functions ---

/**
 * @brief Initializes the Bi-directional IPC channel using busy-wait flags.
 * @param python_executable Path to the python executable.
 * @param script_path Path to the python_bidirectional_ipc_script.py.
 * @param callback The function to call when data is received from Python.
 * @return True on success, false on failure.
 */
bool init_acceptor_ipc_bidirectional(
    const std::string& acceptor_executable,
    const std::string& acceptor_script_path,
    AcceptorDataCallback callback
);

/**
 * @brief Shuts down the Bi-directional IPC channel.
 */
void shutdown_acceptor_ipc_bidirectional();

/**
 * @brief Sends data asynchronously to the Python process. (Non-blocking)
 * Writes data to the C->P buffer and signals Python via semaphore.
 * Does NOT wait for a response. Responses arrive via the PythonDataCallback.
 * @param input_data Pointer to the input data buffer.
 * @param input_len Length of the input data.
 * @return True if data was successfully written and signaled, false otherwise.
 */
bool send_data_to_acceptor_async(const uint8_t* input_data, size_t input_len);

#endif // PYTHON_IPC_H 