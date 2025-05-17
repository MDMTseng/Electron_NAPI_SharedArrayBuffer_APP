#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <cstring> // For memcpy
#include <numeric> // For std::accumulate (can be replaced if needed)
#include <arpa/inet.h> // For htonl
#include <memory> // <<< RE-ADDED for std::shared_ptr >>>
#include "buffer_writer.h" // Include BufferWriter definition
// #include <array> // No longer needed

namespace BPG {

// Fixed size of the BPG packet header in bytes.
// Breakdown: group_id(4) + target_id(4) + tl(2) + prop(4) + data_length(4) = 18
constexpr size_t BPG_HEADER_SIZE = 18;
constexpr uint32_t BPG_PROP_EG_BIT_MASK = 0x00000001; // Mask for the EG bit (LSB of prop field)

// Two-letter packet type identifier
typedef char PacketType[2];

// Error codes (defined earlier for visibility)
enum class BpgError {
    Success = 0,
    EncodingError,
    DecodingError,
    BufferTooSmall,
    InvalidPacketHeader,
    IncompletePacket,
    LinkLayerError
};

// Packet Header Structure Definition.
// NOTE: Use the BPG_HEADER_SIZE constant for serialization/deserialization logic.
struct PacketHeader {
    uint32_t group_id;      // 4 bytes, Big Endian
    uint32_t target_id;     // 4 bytes, Big Endian
    PacketType tl;          // 2 bytes
    uint32_t prop;          // 4 bytes, Big Endian, Property bitfield
    uint32_t data_length;   // 4 bytes, Big Endian (Length of data *after* header)

    // Helper to encode header into a writer
    BpgError encode(BufferWriter& writer) const {
        if (!writer.canWrite(BPG_HEADER_SIZE)) {
            return BpgError::BufferTooSmall;
        }
        uint32_t group_id_n = htonl(group_id);
        uint32_t target_id_n = htonl(target_id);
        uint32_t prop_n = htonl(prop);
        uint32_t data_length_n = htonl(data_length);

        // Write fields according to the DOCUMENTED order
        writer.write(tl, sizeof(PacketType));             // TL (2 bytes)
        writer.write(&prop_n, sizeof(prop_n));             // Prop (4 bytes, Big Endian)
        writer.write(&target_id_n, sizeof(target_id_n));   // TargetID (4 bytes, Big Endian)
        writer.write(&group_id_n, sizeof(group_id_n));     // GroupID (4 bytes, Big Endian)
        writer.write(&data_length_n, sizeof(data_length_n)); // DataLength (4 bytes, Big Endian)
        
        return BpgError::Success;
    }
};

// Simple structure for holding raw binary data
// using BinaryData = std::vector<uint8_t>;

// HybridData structure will now be used for ALL packet content types.
// Format on the wire: str_length(4) + metadata_str(str_length) + binary_bytes(...)
class HybridData {
    public:
    std::string metadata_str; // Describes the binary data. UTF-8 encoded string.
    std::vector<uint8_t> internal_binary_bytes;//if empty, use binary_bytes2
    BufferWriter external_binary_bytes;

    // Calculates the size needed to encode this HybridData instance.
    virtual size_t calculateEncodedSize() const {
        size_t binary_size = 0;
        if (!internal_binary_bytes.empty()){
            binary_size = internal_binary_bytes.size();
        } else if (external_binary_bytes.size() > 0) {
            binary_size = external_binary_bytes.size();
        }
        return sizeof(uint32_t) + metadata_str.length() + binary_size;
    }
    virtual BpgError encode(BufferWriter& writer) const {
        uint32_t json_len = static_cast<uint32_t>(metadata_str.length());
        uint32_t json_len_n = htonl(json_len);
        
        size_t binary_size = calculateEncodedSize();


        size_t required_size = sizeof(uint32_t) + json_len + binary_size;

        if (!writer.canWrite(required_size)) {
            return BpgError::BufferTooSmall;
        }

        // Write JSON length
        writer.write(&json_len_n, sizeof(json_len_n));

        // Write JSON string (if any)
        writer.write(metadata_str.data(), json_len);

        encode_binary_to(writer);

        return BPG::BpgError::Success;
    }

    virtual BpgError encode_binary_to(BufferWriter& writer) const {
        if (!internal_binary_bytes.empty()) {
            if (!writer.canWrite(internal_binary_bytes.size())) return BpgError::BufferTooSmall;
            writer.write(internal_binary_bytes.data(), internal_binary_bytes.size());
            return BpgError::Success;
        }
        return BpgError::Success;
    }

    HybridData() : external_binary_bytes(nullptr, 0) {}
};

// Structure representing a packet at the application layer
struct AppPacket {
    uint32_t group_id;
    uint32_t target_id;
    PacketType tl;
    bool is_end_of_group; // Flag to indicate if this is the last packet of the group
    std::shared_ptr<HybridData> content;

    // Encodes the entire AppPacket (header + content) into the BufferWriter.
    BpgError encode(BufferWriter& writer) const {
        if (!content) {
            printf("content is null\n");
            uint32_t data_len = 0; 
            PacketHeader header;
            header.group_id = group_id;
            header.target_id = target_id;
            std::memcpy(header.tl, tl, sizeof(PacketType));
            header.prop = is_end_of_group ? BPG_PROP_EG_BIT_MASK : 0;
            header.data_length = data_len;
            return header.encode(writer);
        }

        // 1. Calculate data length (using pointer & virtual call)
        uint32_t data_len = static_cast<uint32_t>(content->calculateEncodedSize());
        
        // 2. Construct Header
        PacketHeader header;
        header.group_id = group_id;
        header.target_id = target_id;
        std::memcpy(header.tl, tl, sizeof(PacketType));
        header.prop = is_end_of_group ? BPG_PROP_EG_BIT_MASK : 0;
        header.data_length = data_len;

        // 3. Check if writer has enough space for header AND data
        size_t total_required = BPG_HEADER_SIZE + data_len;
        if (!writer.canWrite(total_required)) {
            return BpgError::BufferTooSmall;
        }

        // 4. Encode Header
        BpgError header_err = header.encode(writer);
        if (header_err != BpgError::Success) {
            return header_err;
        }

        // 5. Encode Content (using pointer & virtual call)
        BpgError content_err = content->encode(writer);
        if (content_err != BpgError::Success) {
            return content_err;
        }

        return BpgError::Success;
    }
};

// Represents a group of packets at the application level
// Note: The concept of a 'group' is now less explicit in the protocol itself,
// but still useful at the application layer for collecting related packets.
using AppPacketGroup = std::vector<AppPacket>;

} // namespace BPG 