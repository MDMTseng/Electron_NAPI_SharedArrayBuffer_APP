#pragma once

#include "bpg_types.h"
#include "buffer_writer.h"

namespace BPG {

// --- BufferWriter Utility Class definition moved to buffer_writer.h ---
// class BufferWriter { /* ... definition removed ... */ };
// --- End BufferWriter ---

class BpgEncoder {
public:
    BpgEncoder() = default;

    // Encoding methods are now part of AppPacket and HybridData in bpg_types.h
    // BpgError encodePacket(const AppPacket& packet, BufferWriter& writer);
    // size_t calculateAppDataSize(const HybridData& data);

private:
    // Helper functions are no longer needed as encoding is handled by data structures
    // bool serializeHeaderInternal(const PacketHeader& header, BufferWriter& writer); 
    // bool serializeAppDataInternal(const HybridData& data, BufferWriter& writer);  
};

} // namespace BPG 