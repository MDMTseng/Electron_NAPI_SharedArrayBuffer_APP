#include "bpg_decoder.h"
#include <cstring> // For memcpy, memcmp
#include <arpa/inet.h> // For ntohl, htonl (assuming network byte order)
#include <iostream> // For potential debug output
#include <algorithm> // For std::copy, std::copy_n, std::min
#include <iterator> // For std::make_move_iterator, std::next
#include <iomanip> // For std::setw, std::setfill
#include <vector> // For temporary contiguous buffer

namespace BPG {

// --- Forward Declarations for new helpers ---
static bool parseHeaderFromBuffer(const uint8_t* buffer_start, size_t buffer_len, PacketHeader& out_header);
static BpgError parseDataFromBuffer(const PacketHeader& header, const uint8_t* data_start, HybridData& out_data);
// ---

BpgDecoder::BpgDecoder() = default;

void BpgDecoder::reset() {
    internal_buffer_.clear();
    active_groups_.clear();
    std::cout << "BPG Decoder reset." << std::endl;
}

// --- Helper: Parse Header from contiguous buffer --- Updated for 18 bytes, new order
static bool parseHeaderFromBuffer(const uint8_t* buffer_start, size_t buffer_len, PacketHeader& out_header) {
    if (!buffer_start || buffer_len < BPG_HEADER_SIZE) {
        std::cerr << "[BPG Decode ERR] parseHeaderFromBuffer called with invalid args: buffer_start="
                  << (void*)buffer_start << ", buffer_len=" << buffer_len << " (expected >= " << BPG_HEADER_SIZE << ")" << std::endl;
        return false;
    }

    const uint8_t* ptr = buffer_start;
    // Network order temporary variables
    uint32_t prop_n, target_id_n, group_id_n, data_length_n;

    // Read fields according to the DOCUMENTED order
    std::memcpy(out_header.tl, ptr, sizeof(PacketType)); ptr += sizeof(PacketType);     // TL (2 bytes)
    std::memcpy(&prop_n, ptr, sizeof(prop_n)); ptr += sizeof(prop_n);               // Prop (4 bytes)
    std::memcpy(&target_id_n, ptr, sizeof(target_id_n)); ptr += sizeof(target_id_n); // TargetID (4 bytes)
    std::memcpy(&group_id_n, ptr, sizeof(group_id_n)); ptr += sizeof(group_id_n);     // GroupID (4 bytes)
    std::memcpy(&data_length_n, ptr, sizeof(data_length_n)); ptr += sizeof(data_length_n); // DataLength (4 bytes)

    // Convert from network order to host order and store in struct members
    // TL is already raw bytes
    out_header.prop = ntohl(prop_n);
    out_header.target_id = ntohl(target_id_n);
    out_header.group_id = ntohl(group_id_n);
    out_header.data_length = ntohl(data_length_n);

    return true;
}

// --- New Helper: Parse Data from contiguous buffer ---
static BpgError parseDataFromBuffer(const PacketHeader& header, const uint8_t* data_start, HybridData& out_data) {
     if (!data_start) {
         std::cerr << "[BPG Decode ERR] parseDataFromBuffer called with null data_start for TL: " << std::string(header.tl, 2) << std::endl;
         return BpgError::DecodingError; 
     }

     const uint8_t* data_end = data_start + header.data_length; 

    // Check required size for string length field
    constexpr size_t STR_LENGTH_SIZE = sizeof(uint32_t);
    if (header.data_length < STR_LENGTH_SIZE) {
        std::cerr << "[BPG Decode ERR] HdrDataLen (" << header.data_length
                  << ") < StrLenSize (" << STR_LENGTH_SIZE << ") for TL: "
                  << std::string(header.tl, 2) << std::endl;
        return BpgError::DecodingError;
    }

    const uint8_t* current_ptr = data_start;

    // 1. Read string length
    uint32_t str_len_n;
    if (current_ptr + STR_LENGTH_SIZE > data_end) { 
         std::cerr << "[BPG Decode ERR] Incomplete data reading str length for TL: " << std::string(header.tl, 2) << std::endl;
         return BpgError::IncompletePacket;
    }
    std::memcpy(&str_len_n, current_ptr, STR_LENGTH_SIZE);
    current_ptr += STR_LENGTH_SIZE;
    uint32_t str_len = ntohl(str_len_n);

     // Verify consistency check
    if (STR_LENGTH_SIZE + str_len > header.data_length) {
        std::cerr << "[BPG Decode ERR] StrLen+Hdr (" << (STR_LENGTH_SIZE + str_len)
                  << ") > HdrDataLen (" << header.data_length
                  << ") | str_len_n=0x" << std::hex << str_len_n << std::dec
                  << ", str_len=" << str_len
                  << ", TL: " << std::string(header.tl, 2) << std::endl;
        return BpgError::DecodingError;
    }

    // 2. Read metadata string
    out_data.metadata_str.clear();
    if (str_len > 0) {
        if (current_ptr + str_len > data_end) {
             std::cerr << "[BPG Decode ERR] Incomplete metadata string data for TL: " << std::string(header.tl, 2) << std::endl;
             return BpgError::IncompletePacket;
        }
        out_data.metadata_str.assign(reinterpret_cast<const char*>(current_ptr), str_len);
        current_ptr += str_len;
    }

    // 3. Read remaining binary bytes
    out_data.internal_binary_bytes.clear();
    size_t binary_bytes_len = header.data_length - STR_LENGTH_SIZE - str_len;
     if (binary_bytes_len > 0) {
        if (current_ptr + binary_bytes_len > data_end) {
            std::cerr << "[BPG Decode ERR] Incomplete Binary data for TL: " << std::string(header.tl, 2) << std::endl;
             return BpgError::IncompletePacket;
        }
        out_data.internal_binary_bytes.assign(current_ptr, current_ptr + binary_bytes_len);
    }

    return BpgError::Success;
}

// --- Refactored tryParsePacket ---
bool BpgDecoder::tryParsePacket(std::deque<uint8_t>& buffer,
                            const AppPacketCallback& packet_callback,
                            const AppPacketGroupCallback& group_callback) {
    // --- Step 1: Check if enough data for header using constant size --- 
    if (buffer.size() < BPG_HEADER_SIZE) { // Use constant
        return false; // Not enough data yet
    }

    // --- Step 2: Peek at header to get data_length --- 
    // Copy exactly BPG_HEADER_SIZE bytes for peeking
    uint8_t header_peek_bytes[BPG_HEADER_SIZE]; // Use constant
    std::copy_n(buffer.begin(), BPG_HEADER_SIZE, header_peek_bytes); // Use constant
    PacketHeader peek_header;
    // Pass BPG_HEADER_SIZE as the length we copied
    if (!parseHeaderFromBuffer(header_peek_bytes, BPG_HEADER_SIZE, peek_header)) { 
         std::cerr << "[BPG Decode ERR] Header peek failed during parseHeaderFromBuffer." << std::endl;
         reset(); 
         return false; 
    }
    uint32_t data_length = peek_header.data_length;
    // Calculate total size using the constant
    size_t total_packet_size = BPG_HEADER_SIZE + data_length; 
    
    // --- Step 3: Check if enough data for the *entire* packet --- 
    if (buffer.size() < total_packet_size) {
        return false; // Not enough data yet
    }

    // --- Step 4: Copy the full potential packet data to a contiguous buffer --- 
    std::vector<uint8_t> temp_packet_buffer(total_packet_size);
    std::copy_n(buffer.begin(), total_packet_size, temp_packet_buffer.begin());

    // --- Step 5: Parse Header and Data from the contiguous buffer --- 
    PacketHeader header; 
    HybridData hybrid_data; 

    // Parse header from temp buffer, passing its actual size
    if (!parseHeaderFromBuffer(temp_packet_buffer.data(), temp_packet_buffer.size(), header)) {
         std::cerr << "[BPG Decode ERR] Header parse failed on temp buffer." << std::endl;
         buffer.erase(buffer.begin(), buffer.begin() + total_packet_size);
         return true; 
    }
    if (header.data_length != data_length) {
         std::cerr << "[BPG Decode ERR] Peeked data length (" << data_length 
                   << ") != Parsed data length (" << header.data_length << "). Corrupted header? Discarding." << std::endl;
         buffer.erase(buffer.begin(), buffer.begin() + total_packet_size);
         return true;
    }

    // Parse data from temp buffer (pointing after the fixed header size)
    BpgError data_err = parseDataFromBuffer(header, temp_packet_buffer.data() + BPG_HEADER_SIZE, hybrid_data);

    // --- Step 6: Consume data from the main deque buffer --- 
    buffer.erase(buffer.begin(), buffer.begin() + total_packet_size);

    // --- Step 7: Process the successfully parsed packet (if applicable) --- 
    if (data_err == BpgError::Success) {
        // Check the EG bit from the uint32_t prop field
        bool is_end = (header.prop & BPG_PROP_EG_BIT_MASK) != 0;

        AppPacket app_packet;
        app_packet.group_id = header.group_id;
        app_packet.target_id = header.target_id;
        std::memcpy(app_packet.tl, header.tl, sizeof(PacketType));
        app_packet.is_end_of_group = is_end;

        app_packet.content = std::make_shared<HybridData>(std::move(hybrid_data));

        if (!active_groups_.count(app_packet.group_id)) {
            active_groups_[app_packet.group_id] = {};
        }
        active_groups_[app_packet.group_id].push_back(std::move(app_packet));

        const auto& stored_packet = active_groups_[header.group_id].back();

        if (packet_callback) {
            try { packet_callback(stored_packet); } catch(const std::exception& e) {
                 std::cerr << "[BPG ERR] Exception in packet_callback: " << e.what() << std::endl;
             } catch(...) { std::cerr << "[BPG ERR] Unknown exception in packet_callback" << std::endl; }
        }

        if (is_end && group_callback) {
            auto group_iter = active_groups_.find(header.group_id);
            if (group_iter != active_groups_.end()) {
                 try { group_callback(header.group_id, std::move(group_iter->second)); } catch(const std::exception& e) {
                     std::cerr << "[BPG ERR] Exception in group_callback: " << e.what() << std::endl;
                 } catch(...) { std::cerr << "[BPG ERR] Unknown exception in group_callback" << std::endl; }
                active_groups_.erase(group_iter);
            }
        }
    } else {
        std::cerr << "BPG Decoder: Error deserializing app data for packet type "
                  << std::string(header.tl, 2) << " (Error code: " << static_cast<int>(data_err) << ")" << std::endl;
    }

    return true; 
}

BpgError BpgDecoder::processData(const uint8_t* data, size_t len,
                                 const AppPacketCallback& packet_callback,
                                 const AppPacketGroupCallback& group_callback) {
    if (!data || len == 0) {
        return BpgError::Success;
    }

    // Append incoming data to the internal buffer (deque insert)
    // This copy handles the volatility of the input 'data' pointer
    try {
        internal_buffer_.insert(internal_buffer_.end(), data, data + len);
    } catch (const std::exception& e) {
        std::cerr << "[BPG Decode ERR] Failed to insert data into deque buffer: " << e.what() << std::endl;
        // Depending on severity, might want to clear buffer or just return error
        // reset(); 
        return BpgError::DecodingError; // Or a more specific error like BufferError
    }


    // Process as many complete packets as possible
    while (tryParsePacket(internal_buffer_, packet_callback, group_callback)) {
        // Loop continues as long as tryParsePacket returns true (meaning it processed something)
    }

    return BpgError::Success;
}

} // namespace BPG 