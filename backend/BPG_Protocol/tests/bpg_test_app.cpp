#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <cstring>
#include <opencv2/opencv.hpp> // Include OpenCV
#include "../bpg_encoder.h"
#include "../bpg_decoder.h"
#include "../bpg_types.h"
#include <map>
#include <algorithm> // for std::max
#include <cctype> // for std::isprint, std::isspace
#include <iomanip>

// --- Test Callbacks --- 
std::map<uint32_t, BPG::AppPacketGroup> received_groups;

void testPacketCallback(const BPG::AppPacket& packet) {
    std::cout << " -> Received Individual Packet (Group: " << packet.group_id << ")" << std::endl;
}

void testGroupCallback(uint32_t group_id, BPG::AppPacketGroup&& group) {
    std::cout << "===> Received COMPLETE Group ID: " << group_id << std::endl;
    std::cout << "     Group Packet Count: " << group.size() << std::endl;
    received_groups[group_id] = std::move(group);
}

// Helper function to print packet details
void printAppPacket(const BPG::AppPacket& packet) {
    std::cout << "  Packet GroupID: " << std::hex << packet.group_id << std::dec
              << ", TargetID: " << std::hex << packet.target_id << std::dec
              << ", Type: " << std::string(packet.tl, 2) 
              << ", EG Flag: " << (packet.is_end_of_group ? "Set" : "Not Set") << std::endl;

    if (!packet.content) {
        std::cout << "    Content: <null>" << std::endl;
        return; // Nothing more to print if no content
    }

    std::cout << "    Content: [HybridData] Meta: " <<
        (packet.content->metadata_str.empty() ? "<empty>" : packet.content->metadata_str)
              << ", Binary Size: " << std::hex << packet.content->internal_binary_bytes.size() << std::dec << " bytes" << std::endl;

    // Optionally show binary content if it's likely text
    if (packet.content->metadata_str.empty() && !packet.content->internal_binary_bytes.empty() && packet.content->internal_binary_bytes.size() < 100) { // Heuristic
        std::string potential_text(packet.content->internal_binary_bytes.begin(), packet.content->internal_binary_bytes.end());
        bool is_printable = true;
        for(char c : potential_text) {
            if (!std::isprint(static_cast<unsigned char>(c)) && !std::isspace(static_cast<unsigned char>(c))) {
                is_printable = false;
                break;
            }
        }
        if (is_printable) {
            std::cout << "      (Binary as text: \"" << potential_text << "\")" << std::endl;
        }
    }

    if (!packet.content->internal_binary_bytes.empty()) {
        std::cout << "    Binary Hex: ";
        size_t print_len = std::min(packet.content->internal_binary_bytes.size(), (size_t)64);
        for (size_t i = 0; i < print_len; ++i) {
            std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(packet.content->internal_binary_bytes[i]) << " ";
        }
        if (packet.content->internal_binary_bytes.size() > 64) {
            std::cout << "...";
        }
        std::cout << std::dec << std::endl; 
    }
}


// --- Test Case: Interleaved Groups --- Refactored for BufferWriter
int testCase_InterleavedGroups() {
    std::cout << "\n--- Test Case: Interleaved Groups --- \n" << std::endl;
    received_groups.clear();
    BPG::BpgDecoder decoder;

    // ... (Group/Target IDs remain the same) ...
    uint32_t group_id_101 = 101;
    uint32_t target_id_101 = 50;
    uint32_t group_id_102 = 102;
    uint32_t target_id_102 = 55;

    std::cout << "--- Sender Creating Packet Definitions --- " << std::endl;

    // Group 101 Definition (Image -> Report -> ACK)
    BPG::AppPacketGroup group101_def;
    {
        BPG::AppPacket img_packet;
        img_packet.group_id = group_id_101; img_packet.target_id = target_id_101; std::memcpy(img_packet.tl, "IM", 2); img_packet.is_end_of_group = false;
        auto img_hybrid_data_ptr = std::make_shared<BPG::HybridData>(); 
        cv::Mat original_image(5, 5, CV_8UC3, cv::Scalar(0, 100, 255));
        cv::putText(original_image, "Hi", cv::Point(1,4), cv::FONT_HERSHEY_PLAIN, 0.5, cv::Scalar(255,255,255), 1);
        std::string image_format = ".jpg"; std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 90};
        cv::imencode(image_format, original_image, img_hybrid_data_ptr->internal_binary_bytes, params);
        img_hybrid_data_ptr->metadata_str = "{\"w\":5,\"h\":5,\"f\":\"jpg\"}";
        img_packet.content = img_hybrid_data_ptr;
        group101_def.push_back(img_packet); printAppPacket(img_packet);

        BPG::AppPacket report_packet;
        report_packet.group_id = group_id_101; report_packet.target_id = target_id_101; std::memcpy(report_packet.tl, "RP", 2); report_packet.is_end_of_group = false;
        auto report_hybrid_data_ptr = std::make_shared<BPG::HybridData>();
        std::string report_str = "{\"p\":0.75}";
        report_hybrid_data_ptr->internal_binary_bytes.assign(report_str.begin(), report_str.end());
        report_packet.content = report_hybrid_data_ptr;
        group101_def.push_back(report_packet); printAppPacket(report_packet);

        BPG::AppPacket ack_packet;
        ack_packet.group_id = group_id_101; ack_packet.target_id = target_id_101; std::memcpy(ack_packet.tl, "AK", 2); ack_packet.is_end_of_group = true;
        auto ack_hybrid_data_ptr = std::make_shared<BPG::HybridData>();
        std::string ack_str = "{\"ok\":1}";
        ack_hybrid_data_ptr->internal_binary_bytes.assign(ack_str.begin(), ack_str.end());
        ack_packet.content = ack_hybrid_data_ptr;
        group101_def.push_back(ack_packet); printAppPacket(ack_packet);
    }

    // Group 102 Definition (Text -> Done)
    BPG::AppPacketGroup group102_def;
    {
        BPG::AppPacket text_packet;
        text_packet.group_id = group_id_102; text_packet.target_id = target_id_102; std::memcpy(text_packet.tl, "TX", 2); text_packet.is_end_of_group = false;
        auto text_hybrid_data_ptr = std::make_shared<BPG::HybridData>();
        std::string text_str = "Hello 102";
        text_hybrid_data_ptr->internal_binary_bytes.assign(text_str.begin(), text_str.end());
        text_packet.content = text_hybrid_data_ptr;
        group102_def.push_back(text_packet); printAppPacket(text_packet);

        BPG::AppPacket done_packet;
        done_packet.group_id = group_id_102; done_packet.target_id = target_id_102; std::memcpy(done_packet.tl, "DN", 2); done_packet.is_end_of_group = true;
        auto done_hybrid_data_ptr = std::make_shared<BPG::HybridData>();
        std::string done_str = "{\"d\":1}";
        done_hybrid_data_ptr->internal_binary_bytes.assign(done_str.begin(), done_str.end());
        done_packet.content = done_hybrid_data_ptr;
        group102_def.push_back(done_packet); printAppPacket(done_packet);
    }

    // Calculate total size needed and pre-allocate buffer
    size_t total_estimated_size = 0;
    auto calculate_packet_size = [&](const BPG::AppPacket& p){ 
        return BPG::BPG_HEADER_SIZE + (p.content ? p.content->calculateEncodedSize() : 0); 
    };
    for(const auto& p : group101_def) total_estimated_size += calculate_packet_size(p);
    for(const auto& p : group102_def) total_estimated_size += calculate_packet_size(p);

    std::vector<uint8_t> stream_buffer_vec(total_estimated_size); // Pre-allocate
    BPG::BufferWriter stream_writer(stream_buffer_vec.data(), stream_buffer_vec.size());

    std::cout << "\n--- Sender Encoding Interleaved Packets into Buffer --- " << std::endl;
    BPG::BpgError encode_err = BPG::BpgError::Success;

    // Encode interleaved directly into the writer using packet.encode()
    encode_err = group101_def[0].encode(stream_writer); assert(encode_err == BPG::BpgError::Success);
    encode_err = group102_def[0].encode(stream_writer); assert(encode_err == BPG::BpgError::Success);
    encode_err = group101_def[1].encode(stream_writer); assert(encode_err == BPG::BpgError::Success);
    encode_err = group102_def[1].encode(stream_writer); assert(encode_err == BPG::BpgError::Success);
    encode_err = group101_def[2].encode(stream_writer); assert(encode_err == BPG::BpgError::Success);

    size_t actual_written_size = stream_writer.size();
    std::cout << "Total bytes written to buffer: " << actual_written_size << " (Estimated: " << total_estimated_size << ")" << std::endl;
    // Optional: Resize vector down to actual size if needed, but not necessary for decoder test
    // stream_buffer_vec.resize(actual_written_size);

    std::cout << "\n--- Receiver Processing Stream from Buffer --- " << std::endl;
    // Process the entire buffer at once (no chunk simulation needed here)
    decoder.processData(stream_buffer_vec.data(), actual_written_size, testPacketCallback, testGroupCallback);

    // --- Verification --- (remains the same, checks received_groups map)
    std::cout << "\n--- Verification --- " << std::endl;
    assert(received_groups.count(group_id_101));
    const auto& received_group_101 = received_groups[group_id_101];
    assert(received_group_101.size() == 3);
    assert(received_group_101[0].content && strncmp(received_group_101[0].tl, "IM", 2) == 0 && !received_group_101[0].is_end_of_group);
    assert(received_group_101[1].content && strncmp(received_group_101[1].tl, "RP", 2) == 0 && !received_group_101[1].is_end_of_group);
    assert(received_group_101[2].content && strncmp(received_group_101[2].tl, "AK", 2) == 0 && received_group_101[2].is_end_of_group);
    std::cout << "Verifying Group 101... PASSED." << std::endl;

    assert(received_groups.count(group_id_102));
    const auto& received_group_102 = received_groups[group_id_102];
    assert(received_group_102.size() == 2);
    assert(received_group_102[0].content && strncmp(received_group_102[0].tl, "TX", 2) == 0 && !received_group_102[0].is_end_of_group);
    assert(received_group_102[1].content && strncmp(received_group_102[1].tl, "DN", 2) == 0 && received_group_102[1].is_end_of_group);
    std::cout << "Verifying Group 102... PASSED." << std::endl;

    std::cout << "\nOverall Verification PASSED." << std::endl;
    return 0;
}

// --- Test Case: Single Packet Group --- Refactored for BufferWriter
int testCase_SinglePacketGroup() {
    std::cout << "\n--- Test Case: Single Packet Group --- " << std::endl;
    received_groups.clear();
    BPG::BpgDecoder decoder;

    uint32_t group_id = 201;
    uint32_t target_id = 60;

    std::cout << "Sender: Creating Single Packet Definition" << std::endl;
    BPG::AppPacket single_packet;
    single_packet.group_id = group_id; single_packet.target_id = target_id; std::memcpy(single_packet.tl, "ST", 2); single_packet.is_end_of_group = true;
    auto status_hybrid_data_ptr = std::make_shared<BPG::HybridData>();
    std::string status_str = "{\"status\":\"ready\"}";
    status_hybrid_data_ptr->internal_binary_bytes.assign(status_str.begin(), status_str.end());
    single_packet.content = status_hybrid_data_ptr;
    printAppPacket(single_packet);

    // Allocate buffer and writer
    size_t required_size = BPG::BPG_HEADER_SIZE + (single_packet.content ? single_packet.content->calculateEncodedSize() : 0);
    std::vector<uint8_t> buffer_vec(required_size);
    BPG::BufferWriter writer(buffer_vec.data(), buffer_vec.size());

    // Encode into writer using packet.encode()
    BPG::BpgError encode_err = single_packet.encode(writer);
    assert(encode_err == BPG::BpgError::Success);
    size_t bytes_written = writer.size();
    std::cout << "Encoded single packet size: " << bytes_written << " bytes" << std::endl;

    std::cout << "Receiver: Processing stream from buffer" << std::endl;
    decoder.processData(buffer_vec.data(), bytes_written, testPacketCallback, testGroupCallback);

    std::cout << "\nVerifying Single Packet Group..." << std::endl;
    assert(received_groups.count(group_id));
    const auto& received_group = received_groups[group_id];
    assert(received_group.size() == 1);
    assert(received_group[0].content && strncmp(received_group[0].tl, "ST", 2) == 0);
    assert(received_group[0].is_end_of_group == true);
    std::cout << "Single Packet Group PASSED." << std::endl;
    return 0;
}

int main() {
    if (testCase_InterleavedGroups() != 0) return 1;
    if (testCase_SinglePacketGroup() != 0) return 1; // Renamed test

    std::cout << "\n--------------------------\n";
    std::cout << "All test cases PASSED." << std::endl;
    std::cout << "--------------------------\n" << std::endl;
    return 0;
}