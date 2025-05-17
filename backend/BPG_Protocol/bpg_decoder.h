#pragma once

#include "bpg_types.h"
#include <vector>
#include <functional>
#include <map>
#include <deque>

namespace BPG {

// Callback type for when a complete application packet is decoded
using AppPacketCallback = std::function<void(const AppPacket&)>;

// Callback type for when a complete packet group (ending with EG) is decoded
using AppPacketGroupCallback = std::function<void(uint32_t group_id, AppPacketGroup&& group)>; // Pass group by rvalue ref

class BpgDecoder {
public:
    BpgDecoder();

    /**
     * @brief Processes incoming binary data potentially containing BPG packets.
     *        Decodes packets and invokes callbacks as they are fully received.
     *        Handles partial packets across multiple calls.
     * @param data The incoming chunk of binary data.
     * @param len The length of the incoming data chunk.
     * @param packet_callback Callback invoked for each fully decoded packet.
     * @param group_callback Callback invoked when an 'EG' packet completes a group.
     * @return BpgError indicating success or failure during processing.
     */
    BpgError processData(const uint8_t* data, size_t len,
                         const AppPacketCallback& packet_callback,
                         const AppPacketGroupCallback& group_callback);

    /**
     * @brief Resets the internal state of the decoder (e.g., clears buffers).
     */
    void reset();

private:
    // Use std::deque for efficient front removal
    std::deque<uint8_t> internal_buffer_;
    std::map<uint32_t, AppPacketGroup> active_groups_;

    // Helper to try parsing a complete packet from the internal buffer
    // Takes non-const buffer reference if modification is needed internally
    bool tryParsePacket(std::deque<uint8_t>& buffer, // Pass buffer by ref
                        const AppPacketCallback& packet_callback,
                        const AppPacketGroupCallback& group_callback);

    // Helper to deserialize the header (reads from buffer)
    bool deserializeHeader(const std::deque<uint8_t>& buffer, PacketHeader& out_header);

    // Helper to deserialize application data
    BpgError deserializeAppData(const PacketHeader& header, 
                                std::deque<uint8_t>::const_iterator data_start,
                                HybridData& out_data);
};

} // namespace BPG 