# native/plugins/python_bidirectional_ipc_script.py
import sys
# Add print statement immediately after initial imports
print(f"[IPC Python EXEC] Script execution started. Args: {sys.argv}")

import time
import ctypes
import mmap # Use mmap directly
import os
import signal
import traceback
import numpy as np
import cv2 # Assuming OpenCV is still needed for processing
import struct
import fcntl

# --- Helper function to get hex preview of bytes (Python version) ---
def bytesToHexPreview(bytes_data: bytes, max_bytes: int = 30) -> str:
    if not bytes_data or len(bytes_data) == 0:
        return "(no binary data)"
    
    byte_to_hex = lambda byte: format(byte, '02X') # Use format for uppercase hex
    
    length = len(bytes_data)
    preview = ""

    if length <= max_bytes * 2:
        preview = ' '.join(map(byte_to_hex, bytes_data))
    else:
        first_part = ' '.join(map(byte_to_hex, bytes_data[:max_bytes]))
        last_part = ' '.join(map(byte_to_hex, bytes_data[length - max_bytes:]))
        preview = f"First {max_bytes}: {first_part} ... Last {max_bytes}: {last_part}"
    return preview
# ---------------------------------------------------------------------

# --- Constants (REMOVED - Sizes now read from SHM) ---
# SHM_DATA_BUFFER_SIZE_CONST = ... 

# --- POSIX constants needed for shm_open via ctypes ---
# Found in <fcntl.h>
O_RDWR = os.O_RDWR 
# Found in <sys/stat.h> (using standard Python os module values)
S_IRUSR = 0o400
S_IWUSR = 0o200
S_IRGRP = 0o040
S_IWGRP = 0o020
S_IROTH = 0o004
S_IWOTH = 0o002
DEFAULT_SHM_MODE = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH # 0666
# -----------------------------------------------------

# --- Load Standard C Library --- 
try:
    libc = ctypes.CDLL(None) # Use None to let ctypes find libc
except OSError as e:
    print(f"[IPC Python] Error loading libc: {e}. Cannot use POSIX functions via ctypes.")
    sys.exit(1)

# --- Define ctypes function prototypes --- 
# int shm_open(const char *name, int oflag, mode_t mode);
try:
    c_shm_open = libc.shm_open
    c_shm_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
    c_shm_open.restype = ctypes.c_int
except AttributeError:
     print(f"[IPC Python] Error: libc.shm_open not found. Is this a POSIX system?")
     sys.exit(1)

# int close(int fd);
try:
    c_close = libc.close
    c_close.argtypes = [ctypes.c_int]
    c_close.restype = ctypes.c_int
except AttributeError:
     print(f"[IPC Python] Error: libc.close not found.")
     # Non-fatal, might still work but can't close FD
     c_close = None 

# --- Shared Memory Structure Definition (Matches C++ Creator/Acceptor) --- 
class SharedIPCBidirectional(ctypes.Structure):
    _fields_ = [
        # Creator -> Acceptor
        ("c_to_a_command", ctypes.c_int32), 
        ("c_to_a_data_len", ctypes.c_size_t),
        # Acceptor -> Creator
        ("a_to_c_status", ctypes.c_int32),  
        ("a_to_c_data_len", ctypes.c_size_t),
        # Defined sizes
        ("defined_c2a_buffer_size", ctypes.c_size_t), 
        ("defined_a2c_buffer_size", ctypes.c_size_t),
        # Padding
        ("_padding1", ctypes.c_char * (128 - ctypes.sizeof(ctypes.c_int32)*2 - ctypes.sizeof(ctypes.c_size_t)*4)),
    ]

# Global variables
shm_fd = -1       
mmap_obj = None   
shm_struct = None 
running = True
SHM_CONTROL_BLOCK_SIZE = ctypes.sizeof(SharedIPCBidirectional) # Should be 128
# Actual buffer sizes read from SHM
ACTUAL_C2A_BUFFER_SIZE = 0 
ACTUAL_A2C_BUFFER_SIZE = 0 

def signal_handler(sig, frame):
    global running
    print(f"[IPC Python] Received signal {sig}. Attempting graceful shutdown...")
    running = False # Signal main loop to stop

def get_buffer_view(offset, length):
    """Gets a memoryview slice of the mmap object."""
    if mmap_obj and offset is not None and (offset + length) <= mmap_obj.size():
        try:
            return mmap_obj[offset:offset+length]
        except IndexError:
             print(f"[IPC Python] Error: Buffer slice out of bounds - offset={offset}, length={length}, mmap_size={mmap_obj.size()}")
             return None
    else:
        print(f"[IPC Python] Error: Invalid buffer access - mmap_obj={mmap_obj}, offset={offset}, length={length}, mmap_size={mmap_obj.size() if mmap_obj else 'N/A'}")
        return None

def process_data_from_creator(data_len):
    """Processes data received from the Creator (C++)."""
    print(f"[IPC Python Acceptor] Received command: Process {data_len} bytes from Creator.")
    if data_len > 0:
        # Check against the actual C2A buffer size
        if data_len > ACTUAL_C2A_BUFFER_SIZE:
             print(f"[IPC Python Acceptor] Error: data_len ({data_len}) > ACTUAL_C2A_BUFFER_SIZE ({ACTUAL_C2A_BUFFER_SIZE})")
             return b"Error: Creator data too large"
        try:
            # Read from C2A buffer (offset is control block size)
            c2a_buffer_offset = SHM_CONTROL_BLOCK_SIZE
            c2a_buffer = get_buffer_view(c2a_buffer_offset, data_len)
            if c2a_buffer is None:
                 return b"Error: Failed to get C2A buffer view"
            input_bytes = c2a_buffer
            
            # --- Example Processing --- 
            width, height = 10, 10
            gray_image = np.zeros((height, width), dtype=np.uint8)
            for j in range(height):
                for i in range(width):
                    gray_image[j, i] =int(i * (255.0 / (width - 1)))
            bgr_image = cv2.cvtColor(gray_image, cv2.COLOR_GRAY2BGR)
            rgba_image = cv2.cvtColor(bgr_image, cv2.COLOR_BGR2RGBA)
            response_data = rgba_image.tobytes()
            # -------------------------

            print(f"[IPC Python Acceptor] Processing complete. Response size: {len(response_data)} bytes.")
            return response_data
        except Exception as e:
            print(f"[IPC Python Acceptor] Error processing data: {e}")
            traceback.print_exc()
            return b"Error during Python processing"
    return b"Acknowledged empty Creator message" 

def send_data_to_creator(data_bytes):
    """Sends data back to the Creator (C++)."""
    global shm_struct, mmap_obj, ACTUAL_C2A_BUFFER_SIZE, ACTUAL_A2C_BUFFER_SIZE 
    if not shm_struct or not mmap_obj:
        print("[IPC Python Acceptor] Error: Cannot send data, IPC not initialized.")
        return False

    data_len = len(data_bytes)
    
    # Calculate offset using the actual C2A size read from SHM
    a2c_buffer_offset = SHM_CONTROL_BLOCK_SIZE + ACTUAL_C2A_BUFFER_SIZE 
    print(f"[IPC Python Acceptor] Calculated A2C offset: {a2c_buffer_offset}") 

    # Check size against the actual A2C size read from SHM
    if data_len > ACTUAL_A2C_BUFFER_SIZE:
        print(f"[IPC Python Acceptor] Error: Response data size ({data_len}) exceeds ACTUAL_A2C_BUFFER_SIZE ({ACTUAL_A2C_BUFFER_SIZE}). Signaling error.")
        while shm_struct.a_to_c_status != 0 and running: time.sleep(0.005) 
        if not running: return False
        shm_struct.a_to_c_data_len = 0
        shm_struct.a_to_c_status = -1
        return False

    # --- Busy-wait for Creator to be ready --- 
    wait_start_time = time.time()
    while shm_struct.a_to_c_status != 0:
        if not running: print("[IPC Python Acceptor] Shutdown requested while waiting to send."); return False
        if time.time() - wait_start_time > 5.0: print("[IPC Python Acceptor] Error: Timeout waiting for Creator ack."); return False
        time.sleep(0.005) 
    # ---------------------------------------

    try:
        # Write data using mmap seek and write at the corrected offset
        if mmap_obj.size() < a2c_buffer_offset + data_len:
            print(f"[IPC Python Acceptor] Error: Calculated write position ({a2c_buffer_offset + data_len}) exceeds mmap size ({mmap_obj.size()}).")
            return False
            
        # --- Log data hex preview BEFORE writing ---
        hex_preview_before_write = bytesToHexPreview(data_bytes)
        print(f"[IPC Python Acceptor] PRE-WRITE Hex Preview: {hex_preview_before_write}")
        # -------------------------------------------

        mmap_obj.seek(a2c_buffer_offset)
        bytes_written = mmap_obj.write(data_bytes)

        if bytes_written != data_len:
             print(f"[IPC Python Acceptor] Warning: mmap.write wrote {bytes_written} bytes, expected {data_len}.")

        # Flush changes
        try:
            mmap_obj.flush() 
        except OSError as e:
            print(f"[IPC Python Acceptor] Warning: mmap.flush failed: {e}") 

        # Set length first, then status
        shm_struct.a_to_c_data_len = data_len 
        shm_struct.a_to_c_status = 1

        print(f"[IPC Python Acceptor] Response ({data_len} bytes) written to A2C buffer (mmap @{a2c_buffer_offset}). Status set to 1.")
        return True

    except Exception as e:
        print(f"[IPC Python Acceptor] Error sending data to Creator (mmap): {e}")
        traceback.print_exc()
        # Attempt to signal error if possible
        try:
            wait_start_time = time.time()
            while shm_struct.a_to_c_status != 0:
                 if not running: break
                 if time.time() - wait_start_time > 0.5: break 
                 time.sleep(0.005)
            if running and shm_struct.a_to_c_status == 0:
                 shm_struct.a_to_c_data_len = 0
                 shm_struct.a_to_c_status = -1 
        except Exception as e_inner:
             print(f"[IPC Python Acceptor] Error trying to signal send error: {e_inner}")
        return False

def main_loop(shm_name):
    global mmap_obj, shm_struct, running, ACTUAL_C2A_BUFFER_SIZE, ACTUAL_A2C_BUFFER_SIZE, shm_fd
    print(f"[IPC Python Acceptor] Script started. PID: {os.getpid()}")
    print(f"[IPC Python Acceptor] Using SHM name: {shm_name}")

    # --- Open Shared Memory using ctypes shm_open --- 
    shm_fd = -1
    encoded_shm_name = shm_name.encode('utf-8')
    attach_attempts = 5
    attach_delay = 0.1 # seconds
    for attempt in range(attach_attempts):
        # Open EXISTING shared memory (no O_CREAT)
        shm_fd = c_shm_open(encoded_shm_name, O_RDWR, DEFAULT_SHM_MODE)
        if shm_fd != -1:
            print(f"[IPC Python Acceptor] Successfully opened SHM '{shm_name}' via shm_open (fd={shm_fd}) on attempt {attempt + 1}.")
            break # Success!
        else:
            # Get errno to understand why it failed
            errno = ctypes.get_errno()
            print(f"[IPC Python Acceptor] shm_open failed for '{shm_name}' (attempt {attempt + 1}/{attach_attempts}, fd={shm_fd}, errno={errno}, msg='{os.strerror(errno)}'). Retrying in {attach_delay}s...")
            if attempt == attach_attempts - 1:
                 print(f"[IPC Python Acceptor] Error: Failed to open SHM via shm_open after {attach_attempts} attempts. Exiting.")
                 sys.exit(1)
            time.sleep(attach_delay)
            attach_delay *= 1.5
    # -----------------------------------------------
    
    # --- Get SHM size (for mapping only) --- 
    reported_shm_size = 0
    try:
        fstat_info = os.fstat(shm_fd)
        reported_shm_size = fstat_info.st_size
        print(f"[IPC Python Acceptor] Obtained SHM size via fstat: {reported_shm_size} bytes (Expected: {SHM_CONTROL_BLOCK_SIZE + ACTUAL_C2A_BUFFER_SIZE + ACTUAL_A2C_BUFFER_SIZE}).")
        # Optional: Check if reported size is at least the expected size
        if reported_shm_size < SHM_CONTROL_BLOCK_SIZE + ACTUAL_C2A_BUFFER_SIZE + ACTUAL_A2C_BUFFER_SIZE:
             print(f"[IPC Python Acceptor] Warning: fstat size ({reported_shm_size}) is less than expected size ({SHM_CONTROL_BLOCK_SIZE + ACTUAL_C2A_BUFFER_SIZE + ACTUAL_A2C_BUFFER_SIZE}). Potential issue.")
             # Decide whether to proceed or exit
    except Exception as e:
        print(f"[IPC Python Acceptor] Error getting SHM size via fstat(fd={shm_fd}): {e}. Exiting.")
        if c_close and shm_fd != -1: c_close(shm_fd)
        sys.exit(1)

    # --- Map Shared Memory using mmap (map reported size) --- 
    try:
        # Map the actual size reported by fstat to avoid errors if it's larger
        mmap_obj = mmap.mmap(shm_fd, reported_shm_size, access=mmap.ACCESS_WRITE, flags=mmap.MAP_SHARED)
        print(f"[IPC Python Acceptor] Successfully memory-mapped SHM (fd={shm_fd}, size={reported_shm_size}).")
    except Exception as e:
         print(f"[IPC Python Acceptor] Error memory-mapping SHM (fd={shm_fd}, size={reported_shm_size}): {e}. Exiting.")
         if c_close and shm_fd != -1: c_close(shm_fd)
         sys.exit(1)
    # ------------------------------------ 

    # --- Map Control Structure --- 
    try:
        # Map the control block part first
        shm_struct = SharedIPCBidirectional.from_buffer(mmap_obj) 
        print("[IPC Python Acceptor] Successfully mapped control structure using from_buffer.")
    except TypeError as e:
         print(f"[IPC Python Acceptor] Warning: from_buffer failed ({e}). Trying from_buffer_copy...")
         try:
              shm_struct = SharedIPCBidirectional.from_buffer_copy(mmap_obj[:SHM_CONTROL_BLOCK_SIZE])
              print("[IPC Python Acceptor] Successfully mapped struct using from_buffer_copy.")
         except Exception as e_copy:
             # Ensure this block uses consistent spacing (e.g., 4 spaces per level)
             print(f"[IPC Python Acceptor] Error creating ctypes structure from mmap buffer copy: {e_copy}. Exiting.")
             # Cleanup ONLY if from_buffer_copy fails
             if mmap_obj:
                 mmap_obj.close()
             if c_close and shm_fd != -1:
                 c_close(shm_fd)
             sys.exit(1)
        
    # --- Read Defined Buffer Sizes from SHM --- 
    try:
        ACTUAL_C2A_BUFFER_SIZE = shm_struct.defined_c2a_buffer_size # Use renamed field
        ACTUAL_A2C_BUFFER_SIZE = shm_struct.defined_a2c_buffer_size # Use renamed field
        if ACTUAL_C2A_BUFFER_SIZE <= 0 or ACTUAL_A2C_BUFFER_SIZE <= 0:
            raise ValueError("Buffer sizes read from SHM are zero or invalid.")
        print(f"[IPC Python Acceptor] Read defined buffer sizes from SHM: C2A={ACTUAL_C2A_BUFFER_SIZE}, A2C={ACTUAL_A2C_BUFFER_SIZE}")
        expected_total = SHM_CONTROL_BLOCK_SIZE + ACTUAL_C2A_BUFFER_SIZE + ACTUAL_A2C_BUFFER_SIZE
        print(f"          (Control Size: {SHM_CONTROL_BLOCK_SIZE})")
        if expected_total > mmap_obj.size():
             print(f"[IPC Python Acceptor] Warning: Sum of control block and defined buffer sizes ({expected_total}) exceeds mapped size ({mmap_obj.size()}).")
             # Decide whether to proceed or exit
    except AttributeError:
         print("[IPC Python Acceptor] Error: Failed to read defined buffer sizes from SHM struct. Structure mismatch?")
         if mmap_obj: mmap_obj.close()
         if c_close and shm_fd != -1: c_close(shm_fd)
         sys.exit(1)
    except ValueError as e:
         print(f"[IPC Python Acceptor] Error: {e}")
         if mmap_obj: mmap_obj.close()
         if c_close and shm_fd != -1: c_close(shm_fd)
         sys.exit(1)
    # -------------------------------------------
        
    # --- Set up signal handlers --- 
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    print("[IPC Python Acceptor] Initialization complete. Polling for Creator commands...")

    # --- Main Polling Loop --- 
    try:
        while running:
            try:
                # Check command from Creator
                command = shm_struct.c_to_a_command # Use c_to_a

                if command == 1: # Data Ready from Creator
                    data_len = shm_struct.c_to_a_data_len # Use c_to_a
                    response_bytes = process_data_from_creator(data_len) # Call renamed func
                    
                    if response_bytes is not None:
                        send_data_to_creator(response_bytes) # Call renamed func
                    else:
                         send_data_to_creator(b"Error during Creator data processing in Acceptor") 
                    
                    # Acknowledge Creator command 
                    shm_struct.c_to_a_command = 0 
                    print("[IPC Python Acceptor] Acknowledged Creator command (set c_to_a_command = 0). Waiting...")

                elif command == 99: # Shutdown command from Creator
                    print("[IPC Python Acceptor] Received shutdown command (99). Acknowledging and exiting.")
                    shm_struct.c_to_a_command = 0 # Acknowledge
                    running = False
                    break 

                elif command == 0: # Idle
                    time.sleep(0.005) 
                else:
                    print(f"[IPC Python Acceptor] Warning: Unknown command {command} received from Creator. Resetting.")
                    shm_struct.c_to_a_command = 0 
                    time.sleep(0.01) 

            except Exception as e:
                 print(f"[IPC Python Acceptor] Error in main loop: {e}")
                 traceback.print_exc()
                 if shm_struct: 
                     try:
                         shm_struct.c_to_a_command = 0
                     except Exception:
                         pass
                 time.sleep(1) 
    finally:
        print("[IPC Python Acceptor] Cleaning up resources...")
        # Unmap memory
        if mmap_obj:
            try:
                mmap_obj.close()
            except Exception as e:
                print(f"Error closing mmap: {e}")
        # Close file descriptor
        if c_close and shm_fd != -1:
            if c_close(shm_fd) == -1:
                 errno = ctypes.get_errno()
                 print(f"[IPC Python Acceptor] Warning: Error closing shm_fd {shm_fd} (errno={errno}, msg='{os.strerror(errno)}').")
            else:
                 print(f"[IPC Python Acceptor] Closed shm_fd {shm_fd}.")
        # Do NOT unlink shm here, C++ (the creator) should handle that.
        print("[IPC Python Acceptor] Script finished.")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: python {sys.argv[0]} <shm_name>")
        sys.exit(1)
    
    shm_name_arg = sys.argv[1]
    main_loop(shm_name_arg) 