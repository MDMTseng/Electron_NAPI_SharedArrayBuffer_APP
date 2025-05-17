#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring> // For memcpy
#include <vector>
#include <string>
#include <arpa/inet.h> // For htonl

namespace BPG {

// --- BufferWriter Utility Class ---
// Wraps a raw buffer to provide safer, vector-like append operations.
class BufferWriter {


private:
    uint8_t* start_ptr_;
    size_t capacity_;
    size_t current_offset_;
public:

    BufferWriter():start_ptr_(nullptr), capacity_(0), current_offset_(0){}
    BufferWriter(uint8_t* buffer, size_t capacity,size_t init_size=0)
        : start_ptr_(buffer), capacity_(capacity), current_offset_(init_size) {
        if (!buffer && capacity > 0) {
            // Handle error: Null buffer with non-zero capacity
            // Maybe throw or set an internal error state? For now, let capacity be 0.
            capacity_ = 0;
        }
    }

    void init(uint8_t* buffer, size_t capacity,size_t init_size=0)
    {
        start_ptr_ = buffer;
        capacity_ = capacity;
        current_offset_ = init_size;
    }


    // Appends raw bytes if capacity allows
    bool append(const void* data, size_t length) {
        if (!start_ptr_ || current_offset_ + length > capacity_) {
            return false; // Not enough space or invalid buffer
        }
        std::memcpy(start_ptr_ + current_offset_, data, length);
        current_offset_ += length;
        return true;
    }

    // Appends a network-order (Big Endian) uint32_t
    bool append_uint32_network(uint32_t value) {
        uint32_t value_n = htonl(value);
        return append(&value_n, sizeof(value_n));
    }

    // Appends bytes from a specific pointer
    bool append_bytes(const uint8_t* data, size_t length) {
        return append(data, length);
    }

     // Appends 2 bytes directly (useful for TL)
    bool append_bytes_2(const char data[2]) {
        return append(data, 2);
    }

    // Appends data from a std::string
    bool append_string(const std::string& str) {
        return append(str.data(), str.length());
    }

    // Appends data from a std::vector<uint8_t>
    bool append_vector(const std::vector<uint8_t>& vec) {
    return append(vec.data(), vec.size());
    }

    uint8_t* claim_space(size_t size){
        if(!canWrite(size)){
            return nullptr;
        }
        uint8_t* ptr = start_ptr_ + current_offset_;
        current_offset_ += size;
        return ptr;
    }
    // Returns the number of bytes currently written
    size_t size() const {
        return current_offset_;
    }

    // Returns the remaining capacity
    size_t remaining() const {
        return capacity_ - current_offset_;
    }
    
    // Returns true if the writer can accommodate writing 'length' more bytes
    bool canWrite(size_t length) const {
        return start_ptr_ && (current_offset_ + length <= capacity_);
    }

    // Overload to check if can write 'length' bytes starting from a specific offset
    bool canWrite(size_t length, size_t starting_offset) const {
        return start_ptr_ && (starting_offset + length <= capacity_);
    }

    // Returns pointer to the start of the buffer (const version)
    const uint8_t* data() const {
        return start_ptr_;
    }
     // Returns pointer to the start of the buffer (non-const version)
     // Use with caution, direct writes bypass the writer's size tracking.
     uint8_t* raw_data() {
         return start_ptr_;
     }
     
     // Get current write position (offset)
    size_t currentPosition() const {
        return current_offset_;
    }

    // Returns total capacity
    size_t capacity() const {
        return capacity_;
    }
    
    // Writes raw bytes using memcpy, advancing the offset. Use append for safety checks.
    // Returns true on success, false if data is null or length is zero or exceeds capacity.
    bool write(const void* data, size_t length) {
        if (!data || length == 0 || !canWrite(length)) {
            return false;
        }
        std::memcpy(start_ptr_ + current_offset_, data, length);
        current_offset_ += length;
        return true;
    }
    bool write(BufferWriter& writer){
        if(!canWrite(writer.size())){
            return false;
        }
        std::memcpy(start_ptr_ + current_offset_, writer.data(), writer.size());
        current_offset_ += writer.size();
    }
};
// --- End BufferWriter ---

} // namespace BPG 