#include "python_ipc.h"
#include <iostream>
#include <string>
#include <vector>
#include <atomic>
#include <thread>        // For std::this_thread::sleep_for
#include <chrono>        // For std::chrono::milliseconds, system_clock
#include <mutex> // For protecting send access
#include <sstream>   // For string streams
#include <iomanip>   // For setfill, setw
#include <algorithm> // For std::min

// POSIX IPC includes
#include <fcntl.h>       // For O_* constants
#include <sys/mman.h>    // For mmap, shm_open, shm_unlink
#include <sys/stat.h>    // For mode constants
#include <unistd.h>      // For ftruncate, close
#include <signal.h>      // For kill
#include <cstdlib>       // For system(), WEXITSTATUS, WIFEXITED etc.
#include <cstring>       // For memcpy, memset
#include <cerrno>        // For errno

// Global variables for Bi-directional IPC
static int shm_fd_bi = -1;
static SharedIPCBidirectional* shm_ptr_bi = nullptr;
static std::thread listener_thread;    // Thread to listen for Acceptor messages
static std::atomic<bool> keep_listener_running(false);
static AcceptorDataCallback data_callback = nullptr; // Use renamed callback type
static std::mutex send_mutex; 

// --- Helper function for Hex Preview ---
std::string bytes_to_hex_preview_cpp(const uint8_t* data, size_t length, size_t max_bytes = 30) {
    if (!data || length == 0) {
        return "(no binary data)";
    }
    std::stringstream ss;
    size_t print_len_each_side = std::min(length, max_bytes);

    if (length <= max_bytes * 2) { // Show all if short enough
        for (size_t i = 0; i < length; ++i) {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]) << " ";
        }
    } else {
        ss << "First " << print_len_each_side << ": ";
        for (size_t i = 0; i < print_len_each_side; ++i) {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]) << " ";
        }
        ss << "... Last " << print_len_each_side << ": ";
        for (size_t i = length - print_len_each_side; i < length; ++i) {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]) << " ";
        }
    }
    return ss.str();
}

// --- Listener Thread Function (Busy-Wait Version) ---
void acceptor_listener_thread_func() { // Renamed function
    std::cout << "[IPC C++ Listener] Listener thread for Acceptor started (polling mode)." << std::endl;
    while (keep_listener_running.load()) {
        if (!shm_ptr_bi) { 
            std::cerr << "[IPC C++ Listener] Error: Shared memory pointer is null. Exiting thread." << std::endl;
            keep_listener_running.store(false);
            break;
        }

        // --- Poll status from Acceptor --- 
        int32_t a_status = shm_ptr_bi->a_to_c_status.load(); // Use a_to_c_status

        if (a_status == 1) { // Data Ready from Acceptor
            size_t data_len = shm_ptr_bi->a_to_c_data_len.load(); // Use a_to_c_data_len
            std::cout << "[IPC C++ Listener] Received Status=1 from Acceptor, Data Len=" << data_len << std::endl; 
            std::this_thread::sleep_for(std::chrono::microseconds(500)); // Keep delay for now

            // Check received length against the defined A->C (RX) buffer size
            if (data_len <= shm_ptr_bi->defined_a2c_buffer_size && data_len > 0) { // Use defined_a2c_buffer_size
                const uint8_t* acceptor_buffer_ptr = reinterpret_cast<const uint8_t*>(shm_ptr_bi->buffer_a_to_c); // Use buffer_a_to_c
                std::string hex_preview = bytes_to_hex_preview_cpp(acceptor_buffer_ptr, data_len);
                std::cout << "[IPC C++ Listener] Acceptor SHM Buffer Preview (after delay): " << hex_preview << std::endl;
                if (data_callback) {
                    try {
                        data_callback(acceptor_buffer_ptr, data_len); 
                    } catch (const std::exception& e) {
                        std::cerr << "[IPC C++ Listener] Exception in data_callback: " << e.what() << std::endl;
                    } catch (...) {
                        std::cerr << "[IPC C++ Listener] Unknown exception in data_callback." << std::endl;
                    }
                } else {
                    std::cerr << "[IPC C++ Listener] Warning: No data callback registered." << std::endl;
                }
            } else {
                 std::cerr << "[IPC C++ Listener] Error: Acceptor reported data size (" << data_len
                           << ") invalid or larger than defined A->C buffer (" 
                           << shm_ptr_bi->defined_a2c_buffer_size << ")." << std::endl; // Use defined_a2c_buffer_size
            }
            // Acknowledge processing by resetting Acceptor's status
            shm_ptr_bi->a_to_c_status.store(0); // Use a_to_c_status
            std::cout << "[IPC C++ Listener] Acknowledged Acceptor (set a_to_c_status = 0)." << std::endl;

        } else if (a_status == -1) { // Error status from Acceptor
            std::cerr << "[IPC C++ Listener] Received Error Status (-1) from Acceptor." << std::endl;
            shm_ptr_bi->a_to_c_status.store(0); // Use a_to_c_status
             std::cout << "[IPC C++ Listener] Acknowledged Acceptor Error (set a_to_c_status = 0)." << std::endl;

        } else if (a_status == 0) { // Idle status from Acceptor
            std::this_thread::sleep_for(std::chrono::microseconds(500)); 
        } else { // Unknown status
             std::cerr << "[IPC C++ Listener] Warning: Unknown Acceptor status code: " << a_status << ". Resetting." << std::endl;
             shm_ptr_bi->a_to_c_status.store(0); // Use a_to_c_status
             std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    std::cout << "[IPC C++ Listener] Listener thread exiting." << std::endl;
}

// --- Implementation of Public Functions --- 

bool init_acceptor_ipc_bidirectional( // Renamed function
    const std::string& acceptor_executable,
    const std::string& acceptor_script_path,
    AcceptorDataCallback callback)
{
    std::cout << "[IPC C++] Initializing Bi-directional IPC with Acceptor..." << std::endl;
    data_callback = callback; 
    shm_unlink(SHM_NAME_BI);

    // --- Create/Open Shared Memory --- 
    size_t control_block_size = 128; 
    size_t total_shm_size = control_block_size + SHM_C2A_BUFFER_MAX_SIZE + SHM_A2C_BUFFER_MAX_SIZE; // Use renamed constants 
    std::cout << "[IPC C++] Calculated total SHM allocation size: " << total_shm_size << " bytes." << std::endl;
    std::cout << "          Control Block Size: " << control_block_size << std::endl;
    std::cout << "          Max C2A Size: " << SHM_C2A_BUFFER_MAX_SIZE << std::endl; // Use renamed constants 
    std::cout << "          Max A2C Size: " << SHM_A2C_BUFFER_MAX_SIZE << std::endl; // Use renamed constants 

    shm_fd_bi = shm_open(SHM_NAME_BI, O_CREAT | O_RDWR, 0666);
    if (shm_fd_bi == -1) {
        perror("[IPC C++] shm_open failed");
        return false;
    }
    if (ftruncate(shm_fd_bi, total_shm_size) == -1) {
        perror("[IPC C++] ftruncate failed");
        close(shm_fd_bi); shm_unlink(SHM_NAME_BI); return false;
    }
    shm_ptr_bi = (SharedIPCBidirectional*)mmap(0, total_shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_bi, 0);
    if (shm_ptr_bi == MAP_FAILED) {
        perror("[IPC C++] mmap failed");
        close(shm_fd_bi); shm_unlink(SHM_NAME_BI); return false;
    }
    std::cout << "[IPC C++] Bi-directional SHM created/opened and mapped." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Keep delay for safety

    // --- Initialize Shared Memory Control Block --- 
    new (&shm_ptr_bi->c_to_a_command) std::atomic<int32_t>(0); // Use c_to_a
    new (&shm_ptr_bi->c_to_a_data_len) std::atomic<size_t>(0); // Use c_to_a
    new (&shm_ptr_bi->a_to_c_status) std::atomic<int32_t>(0);  // Use a_to_c
    new (&shm_ptr_bi->a_to_c_data_len) std::atomic<size_t>(0);  // Use a_to_c
    
    // Set the defined buffer sizes using the specific constants
    shm_ptr_bi->defined_c2a_buffer_size = SHM_C2A_BUFFER_MAX_SIZE; // Use renamed constants & fields
    shm_ptr_bi->defined_a2c_buffer_size = SHM_A2C_BUFFER_MAX_SIZE; // Use renamed constants & fields
    std::cout << "[IPC C++] Set defined buffer sizes in SHM: C2A=" 
              << shm_ptr_bi->defined_c2a_buffer_size << ", A2C=" 
              << shm_ptr_bi->defined_a2c_buffer_size << std::endl;

    // Zero out padding and buffers using their specific max sizes
    memset(shm_ptr_bi->_padding1, 0, sizeof(shm_ptr_bi->_padding1));
    memset(shm_ptr_bi->buffer_c_to_a, 0, SHM_C2A_BUFFER_MAX_SIZE); // Use renamed buffer & constant
    memset(shm_ptr_bi->buffer_a_to_c, 0, SHM_A2C_BUFFER_MAX_SIZE); // Use renamed buffer & constant
    std::cout << "[IPC C++] Bi-directional SHM control block initialized." << std::endl;

    // --- Launch Acceptor Script --- 
    std::string full_script_path = "APP/backend/" + acceptor_script_path; // Construct path relative to project root
    std::string command = acceptor_executable + " -u " + full_script_path + " " 
                         + SHM_NAME_BI + " &"; 
    std::cout << "[IPC C++] Launching Acceptor script: " << command << std::endl;
    int result = system(command.c_str());
     if (result != 0) {
        // Note: system() return value is complex to interpret reliably for background processes.
        // We might not get a useful error code here if the script fails later.
        std::cerr << "[IPC C++] Warning: system() call for Acceptor script returned non-zero or error. Check script output/logs." << std::endl;
        // Consider adding a short delay and then checking if Acceptor is alive, 
        // or having Acceptor create a small pid/status file on successful startup.
    }

    // --- Start Listener Thread --- 
    keep_listener_running.store(true);
    listener_thread = std::thread(acceptor_listener_thread_func); // Call renamed listener
    std::cout << "[IPC C++] Listener thread starting." << std::endl;

    std::cout << "[IPC C++] Bi-directional IPC Initialization complete." << std::endl;
    return true;
}

void shutdown_acceptor_ipc_bidirectional() { // Renamed function
    std::cout << "[IPC C++] Shutting down Bi-directional IPC with Acceptor..." << std::endl;

    // --- Stop Listener Thread --- 
    if (keep_listener_running.load()) {
        keep_listener_running.store(false);
        if (listener_thread.joinable()) {
            listener_thread.join();
             std::cout << "[IPC C++] Listener thread joined." << std::endl;
        }
    }

    // --- Signal Acceptor process to shut down via SHM flag --- 
    if (shm_ptr_bi) {
        std::cout << "[IPC C++] Sending Shutdown command (99) to Acceptor..." << std::endl;
        shm_ptr_bi->c_to_a_command.store(99); // Use c_to_a_command

        // Optional: Wait briefly for Acceptor to acknowledge shutdown
        auto shutdown_start = std::chrono::steady_clock::now();
        while (shm_ptr_bi->c_to_a_command.load() == 99) { // Use c_to_a_command
            if (std::chrono::steady_clock::now() - shutdown_start > std::chrono::milliseconds(500)) {
                 std::cerr << "[IPC C++] Warning: Timeout waiting for Acceptor to acknowledge shutdown command." << std::endl;
                 break;
            }
             std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (shm_ptr_bi->c_to_a_command.load() == 0) { // Use c_to_a_command
             std::cout << "[IPC C++] Acceptor acknowledged shutdown command." << std::endl;
        }

        // Calculate total size for munmap using the MAX constants
        size_t control_block_size = 128;
        size_t total_shm_size = control_block_size + SHM_C2A_BUFFER_MAX_SIZE + SHM_A2C_BUFFER_MAX_SIZE;
        munmap(shm_ptr_bi, total_shm_size);
        shm_ptr_bi = nullptr;
         std::cout << "[IPC C++] Shared memory unmapped." << std::endl;
    }

    // --- Cleanup Resources (SHM only) --- 
    if (shm_fd_bi != -1) {
        close(shm_fd_bi); shm_fd_bi = -1;
        shm_unlink(SHM_NAME_BI);
         std::cout << "[IPC C++] Shared memory unlinked." << std::endl;
    }

    data_callback = nullptr; // Clear callback
    std::cout << "[IPC C++] Bi-directional IPC Shutdown complete." << std::endl;
}

bool send_data_to_acceptor_async(const uint8_t* input_data, size_t input_len) { // Renamed function
     std::lock_guard<std::mutex> lock(send_mutex);
     if (!shm_ptr_bi) { return false; }
     
     // Check input length against the defined C->A (TX) buffer size
     if (input_len > shm_ptr_bi->defined_c2a_buffer_size) { // Use defined_c2a_buffer_size
         std::cerr << "[IPC C++] Error: Input data size (" << input_len
                   << ") exceeds defined C->A buffer size (" << shm_ptr_bi->defined_c2a_buffer_size << ")." << std::endl;
        return false;
     }

     // --- Busy-wait for Acceptor to be ready --- 
     auto wait_start_time = std::chrono::steady_clock::now();
     while (shm_ptr_bi->c_to_a_command.load() != 0) { // Use c_to_a_command
         if (!keep_listener_running.load()) { 
             std::cerr << "[IPC C++] Aborting send: Shutdown in progress." << std::endl;
             return false;
         }
         if (std::chrono::steady_clock::now() - wait_start_time > std::chrono::seconds(5)) { 
             std::cerr << "[IPC C++] Error: Timeout waiting for Acceptor to acknowledge previous C->A command (" 
                       << shm_ptr_bi->c_to_a_command.load() << "). Sending failed." << std::endl;
             return false; 
         }
         std::this_thread::sleep_for(std::chrono::microseconds(500)); 
     }
     // ---------------------------------------

     // Write data to C->A buffer
     memcpy(shm_ptr_bi->buffer_c_to_a, input_data, input_len); // Use buffer_c_to_a
     shm_ptr_bi->c_to_a_data_len.store(input_len); // Use c_to_a_data_len
     shm_ptr_bi->c_to_a_command.store(1); // Use c_to_a_command
     std::cout << "[IPC C++] Data written to C->A SHM (" << input_len << " bytes). Command set to 1." << std::endl;
     return true;
}

// Remove the old synchronous function
// int process_data_with_python_sync(...) { ... } 