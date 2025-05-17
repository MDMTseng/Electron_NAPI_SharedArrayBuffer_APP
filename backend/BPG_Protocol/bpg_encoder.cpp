// #include "bpg_encoder.h" // No longer needed directly for encoding logic
#include "bpg_types.h" // Now includes encoding methods
// #include <cstring> // Included via bpg_types.h
// #include <arpa/inet.h> // Included via bpg_types.h
// #include <iostream> // Was for debugging, remove if not needed
// #include <iomanip> // Was for debugging, remove if not needed
// #include <stdexcept> // Was for debugging, remove if not needed

namespace BPG {

// BpgEncoder class is now essentially empty or could be removed if 
// it holds no other state or methods.

// --- All method implementations below are now removed as they --- 
// --- reside within AppPacket and HybridData in bpg_types.h --- 

// // Renamed internal helper
// bool BpgEncoder::serializeHeaderInternal(const PacketHeader& header, BufferWriter& writer) {
//     // ... implementation removed ... 
// }

// // Renamed internal helper
// bool BpgEncoder::serializeAppDataInternal(const HybridData& data, BufferWriter& writer) {
//     // ... implementation removed ... 
// }

// // calculateAppDataSize remains the same
// size_t BpgEncoder::calculateAppDataSize(const HybridData& data) {
//    // ... implementation removed ... 
// }

// // *** IMPLEMENTATION OF OVERLOAD using BufferWriter ***
// BpgError BpgEncoder::encodePacket(const AppPacket& packet, BufferWriter& writer)
// {
//    // ... implementation removed ... 
// }

} // namespace BPG 