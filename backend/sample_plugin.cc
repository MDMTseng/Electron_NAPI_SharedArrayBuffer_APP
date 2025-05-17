#include "include/plugin_interface.h"
#include <cstring>
#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <opencv2/opencv.hpp>
#include <iomanip>
#include <memory>
#include <dlfcn.h>
#include "InspTarPluginInterface.h"
#include <unistd.h>  // for getcwd

// Include BPG Protocol headers
#include "BPG_Protocol/bpg_decoder.h"
#include "BPG_Protocol/bpg_encoder.h"
#include "BPG_Protocol/bpg_types.h"

// Include our Python IPC header
#include "python_ipc.h"

BPG::AppPacket create_string_packet(uint32_t group_id, uint32_t target_id,std::string TL, std::string str);

static MessageCallback g_send_message = nullptr;
static BufferRequestCallback g_buffer_request_callback = nullptr;
static BufferSendCallback g_buffer_send_callback = nullptr;
static BPG::BpgDecoder g_bpg_decoder; // Decoder instance for this plugin

// Handles for dynamically loaded ImgSrc plugin
static void* imgsrc_lib_handle = nullptr;
static ITPIF_PluginInterface* imgsrc_interface = nullptr;
static void* imgsrc_instance = nullptr;
static ITPIF_ManagerInterface imgsrc_manager = {nullptr, nullptr, nullptr, nullptr, nullptr};

// Manager callback stubs
static int mgr_dispatch(void* main_ctx, struct ITPIF_StageInfo_c* data) { (void)main_ctx; (void)data; return 0; }
static cJSON* mgr_getNLockGlobalValue(void* main_ctx) { (void)main_ctx; return nullptr; }
static void mgr_unLockGlobalValue(void* main_ctx) { (void)main_ctx; }
static ITPIF_ImageInfo mgr_requestImg(void* main_ctx, int width, int height, int channels, int type) {
    (void)main_ctx;
    ITPIF_ImageInfo info = ITPIF_ImageInfoInit();
    size_t sz = width * height * channels * sizeof(uint8_t);
    void* buf = std::malloc(sz);
    info.buffer = buf;
    info.width = width;
    info.height = height;
    info.channels = channels;
    info.step = width * channels * sizeof(uint8_t);
    info.type = type;
    info.elemSize = channels * sizeof(uint8_t);
    info.totalSize = sz;
    return info;
}
static struct ITPIF_StageInfo_c mgr_requestStageInfo(void* main_ctx) { (void)main_ctx; struct ITPIF_StageInfo_c s = {}; return s; }

class HybridData_cvMat:public BPG::HybridData{
    public:
    cv::Mat img;
    std::string img_format;
    HybridData_cvMat(cv::Mat img,std::string img_format):img(img),img_format(img_format){
        printf("HybridData_cvMat: %zu <<format: %s\n",calculateBinarySize(),img_format.c_str());
    }

    size_t calculateBinarySize() const {
        if(img_format=="raw"){
            return img.total()*img.elemSize();
        }
        if(img_format=="raw_rgba"){
            return img.total()*4;
        }
        return 0;
    }

    size_t calculateEncodedSize() const override {
        return sizeof(uint32_t) + metadata_str.length() + calculateBinarySize();
    }

    BPG::BpgError encode_binary_to(BPG::BufferWriter& writer) const override {
        uint8_t* buffer = writer.claim_space(calculateBinarySize());
        if(buffer == nullptr) {
             std::cerr << "[HybridData_cvMat ERR] Failed to claim space in buffer! Capacity: " 
                       << writer.capacity() << ", Current Size: " << writer.size() 
                       << ", Requested: " << calculateBinarySize() << std::endl;
             return BPG::BpgError::BufferTooSmall; // Or another appropriate error
        }
        
        if(img_format=="raw"){
            std::memcpy(buffer, img.data, img.total()*img.elemSize());
            return BPG::BpgError::Success;
        }
        if(img_format=="raw_rgba"){

            switch(img.type()){
                case CV_8UC1:
                {
                    uint8_t* img_data = img.data;
                    uint8_t* buffer_ptr = buffer;
                    for(int i=0;i<img.total();i++){
                        uint8_t pixel = *img_data++;
                        *buffer_ptr++=pixel;
                        *buffer_ptr++=pixel;
                        *buffer_ptr++=pixel;
                        *buffer_ptr++=255;
                    }
                }
                    break;
                case CV_8UC3:
                {
                    uint8_t* img_data = img.data;
                    uint8_t* buffer_ptr = buffer;
                    for(int i=0;i<img.total();i++){
                        *buffer_ptr++=*img_data++;
                        *buffer_ptr++=*img_data++;
                        *buffer_ptr++=*img_data++;
                        *buffer_ptr++=255;
                    }
                }
                    break;
                case CV_8UC4:
                {
                    memcpy(buffer,img.data,img.total()*4);
                }
                    break;
            }
            return BPG::BpgError::Success;
        }
        return BPG::BpgError::EncodingError;
    }
};

// --- BPG Callbacks --- 

// Callback for data received FROM Python via the listener thread
static void handle_python_data(const uint8_t* data, size_t length) {
    std::cout << "[SamplePlugin PythonCallback] Received " << length << " bytes from Python listener." << std::endl;
    
    // TODO: Need context (like original group_id, target_id) to send a proper response.
    // For now, just create a generic BPG packet with the received data.
    // A more robust solution would involve associating requests with responses, 
    // perhaps using a map or storing context when send_data_to_python_async is called.
    uint32_t response_group_id = 999; // Placeholder group ID
    uint32_t response_target_id = 1;   // Placeholder target ID
    
    BPG::AppPacketGroup response_group;
    BPG::AppPacket resp_packet = create_string_packet(response_group_id, response_target_id, "PR", ""); // "PR" = Python Response
    
    // Check if data length exceeds capacity (shouldn't if SHM sizes match)
    if (length > resp_packet.content->internal_binary_bytes.max_size()) {
         std::cerr << "[SamplePlugin PythonCallback] Error: Received data length (" << length 
                   << ") exceeds BPG packet binary buffer capacity." << std::endl;
         // Optionally send an error status back to JS?
         return;
    }
    
    // Copy data into the BPG packet
    resp_packet.content->internal_binary_bytes.assign(data, data + length); 
    
    response_group.push_back(resp_packet);
    response_group.back().is_end_of_group = true;
    
    // Send the response group using the buffer callbacks
    uint8_t* buffer = nullptr;
    uint32_t buffer_size = 0;
    // Use a reasonable buffer size estimate
    size_t estimated_size = BPG::BPG_HEADER_SIZE + resp_packet.content->calculateEncodedSize(); 

    // Request buffer - Ensure callbacks are valid
    if (g_buffer_request_callback && g_buffer_send_callback) {
        if (g_buffer_request_callback(estimated_size, &buffer, &buffer_size) == 0 && buffer != nullptr) {
            BPG::BufferWriter stream_writer(buffer, buffer_size);
            BPG::BpgError encode_err = response_group[0].encode(stream_writer);
            if (encode_err == BPG::BpgError::Success) {
                g_buffer_send_callback(stream_writer.size());
                std::cout << "   Sent Python result back via BPG (Group " << response_group_id << ")." << std::endl;
            } else {
                g_buffer_send_callback(0); // Indicate error by sending 0 size
                std::cerr << "   Error encoding Python result BPG packet: " << static_cast<int>(encode_err) << std::endl;
            }
        } else {
             std::cerr << "   Failed to get buffer for sending Python result." << std::endl;
             g_buffer_send_callback(0); // Indicate error
        }
    } else {
         std::cerr << "[SamplePlugin PythonCallback] Error: Buffer callbacks not available!" << std::endl;
    }
}

// Example function to handle a fully decoded application packet
static void handle_decoded_packet(const BPG::AppPacket& packet) {
    std::cout << "[SamplePlugin BPG] Decoded Packet - Group: " << packet.group_id
              << ", Target: " << packet.target_id
              << ", Type: " << std::string(packet.tl, 2) << std::endl;

    if (!packet.content) {
        std::cout << "    Content: <null>" << std::endl;
        return; 
    }
    
    std::cout << "    Meta: " << (packet.content->metadata_str.empty() ? "<empty>" : packet.content->metadata_str) << std::endl;
    std::cout << "    Binary Size: " << packet.content->calculateEncodedSize() - sizeof(uint32_t) - packet.content->metadata_str.length() << std::endl; // Approx binary size

    // Print binary content hex preview (up to 64 bytes)
    if (!packet.content->internal_binary_bytes.empty()) {
        std::cout << "    Binary Hex: ";
        size_t print_len = std::min(packet.content->internal_binary_bytes.size(), (size_t)64);
        for (size_t i = 0; i < print_len; ++i) {
            std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(packet.content->internal_binary_bytes[i]) << " ";
        }
        if (packet.content->internal_binary_bytes.size() > 64) {
            std::cout << "...";
        }
        std::cout << std::dec << std::endl; // Reset stream to decimal
    }

    // --- Process TX packet via Python IPC (Now Asynchronous) ---
    if (strncmp(packet.tl, "TX", 2) == 0 && packet.content && !packet.content->internal_binary_bytes.empty()) {
        std::cout << "    -> Forwarding TX packet content to Python IPC (Async)..." << std::endl;
        
        // Send data asynchronously. The response will arrive via handle_python_data callback.
        bool send_success = send_data_to_acceptor_async(
            packet.content->internal_binary_bytes.data(),
            packet.content->internal_binary_bytes.size()
        );

        if (!send_success) {
            std::cerr << "    <- Error sending data to Python IPC via send_data_to_python_async." << std::endl;
            // Optionally send an immediate error back via BPG or log it
        } else {
             std::cout << "    <- Data sent to Python asynchronously." << std::endl;
        }
        // --- Remove the old synchronous result handling block ---
        // std::vector<uint8_t> python_result;
        // int ipc_ret = process_data_with_python_sync(...);
        // if (ipc_ret == 0) { ... } else { ... }
        // -----------------------------------------------------
    }
    // -----------------------------------------

    if (strncmp(packet.tl, "IM", 2) == 0) {
        std::cout << "    (Packet is an Image)" << std::endl;
    }
}


BPG::AppPacket create_image_packet(uint32_t group_id, uint32_t target_id, const cv::Mat& img, std::string img_format="") {
    BPG::AppPacket img_packet;
    img_packet.group_id = group_id;
    img_packet.target_id = target_id;
    std::memcpy(img_packet.tl, "IM", 2);
    img_packet.is_end_of_group = false;

    // Create the derived object using make_shared
    auto img_hybrid_data_ptr = std::make_shared<HybridData_cvMat>(img, img_format);
    
    // Set metadata on the object via the pointer
    img_hybrid_data_ptr->metadata_str = 
        "{\"width\":"+std::to_string(img.cols)+
        ",\"height\":"+std::to_string(img.rows)+
        ",\"channels\":"+std::to_string(img.channels())+
        ",\"type\":"+std::to_string(img.type())+
        ",\"format\":\""+img_format+"\"}";
    

    printf("metadata_str: %s\n",img_hybrid_data_ptr->metadata_str.c_str());
    // Assign the shared_ptr to content
    img_packet.content = img_hybrid_data_ptr;
    return img_packet;
}




BPG::AppPacket create_string_packet(uint32_t group_id, uint32_t target_id,std::string TL, std::string str){
    BPG::AppPacket string_packet;
    string_packet.group_id = group_id;
    string_packet.target_id = target_id; // Use the provided target_id
    std::memcpy(string_packet.tl, TL.c_str(), TL.size());
    string_packet.is_end_of_group = false;
    auto hybrid_data_ptr = std::make_shared<BPG::HybridData>();
    
    // Set metadata on the object via the pointer
    hybrid_data_ptr->metadata_str = str;
    string_packet.content = hybrid_data_ptr;
    return string_packet;
}

// --- Example Sending Functions --- 


int drawCounter=0;
// NEW: Function to send a simple Acknowledgement Group
static bool send_acknowledgement_group(uint32_t group_id, uint32_t target_id) {
    if (!g_send_message) {
        std::cerr << "[SamplePlugin BPG] Error: Cannot send ACK, g_send_message is null." << std::endl;
        return false;
    }

    std::cout << "[SamplePlugin BPG] Encoding and Sending ACK Group ID: " << group_id << std::endl;
    BPG::AppPacketGroup group_to_send;

    {
        // --- Construct IM Packet ---

        cv::Mat img(600,800,CV_8UC4,cv::Scalar(0,0,255,100));
        // draw text on the image
        cv::putText(img, "Hello, World!"+std::to_string(drawCounter++), cv::Point(10,50), cv::FONT_HERSHEY_SIMPLEX, 1, cv::Scalar(0,0,0,255), 2);
        group_to_send.push_back(
            create_image_packet(group_id, target_id, 
        img, 
        "raw_rgba")
        );
    }





    {
        group_to_send.push_back(
            create_string_packet(group_id, target_id,"AK","{\"received\":true}")
        );


    }

    //set last packet as end of group
    group_to_send.back().is_end_of_group = true;

    // --- Calculate Size and Create Buffer/Writer ---
    size_t total_estimated_size = 0;
    for(const auto& packet : group_to_send) {
        if (packet.content) { 
            // printf("packet.content->calculateEncodedSize(): %zu\n",packet.content->calculateEncodedSize());
            total_estimated_size += BPG::BPG_HEADER_SIZE + packet.content->calculateEncodedSize();
        } else {
            total_estimated_size += BPG::BPG_HEADER_SIZE; 
        }
    }

    // --- Request Buffer ---
    uint8_t* buffer = nullptr;
    uint32_t buffer_size = 0;
    g_buffer_request_callback(1000,&buffer, &buffer_size);

    BPG::BufferWriter stream_writer(buffer, buffer_size);



    // --- Encode the Group into the Writer ---
    // write packets back to back
    bool success = true;
    for (const auto& packet : group_to_send) { 
        uint8_t* buffer_ptr = stream_writer.raw_data()+stream_writer.currentPosition();
        memset(buffer_ptr,0,200);
        printf("encoding packet: %s, group_id: %d\n",std::string(packet.tl, 2).c_str(),packet.group_id);
        BPG::BpgError encode_err = packet.encode(stream_writer);


        if (encode_err != BPG::BpgError::Success) {
            std::cerr << "[SamplePlugin BPG] Error encoding ACK packet: " << static_cast<int>(encode_err) << std::endl;
            success = false;
            break; // Exit loop on error
        }
    }

    // --- Send the Entire Buffer ---
    if (success) {
         std::cout << "  Sending ACK Group (ID: " << group_id << "), Total Size: " << stream_writer.size() << std::endl;
        //  g_send_message(stream_writer.data(), stream_writer.size());
        g_buffer_send_callback(stream_writer.size());
    }
    else
    {
        g_buffer_send_callback(0);
    }

    return success; // Return overall success/failure
}

// Example function to handle a completed packet group
static void handle_decoded_group(uint32_t group_id, BPG::AppPacketGroup&& group) {
     std::cout << "[SamplePlugin BPG] Decoded COMPLETE Group - ID: " << group_id 
               << ", Packet Count: " << group.size() << std::endl;
    
    // --- TODO: Add application logic for the complete group --- 
    for(const auto& packet : group) {
         std::cout << "    - Packet Type in Group: " << std::string(packet.tl, 2) << std::endl;
         if (packet.content) {
             std::cout << "      Meta: " << (packet.content->metadata_str.empty() ? "<empty>" : packet.content->metadata_str) << std::endl;
             std::cout << "      Binary Size: " << packet.content->calculateEncodedSize() - sizeof(uint32_t) - packet.content->metadata_str.length() << std::endl; // Approx binary size
         } else {
             std::cout << "      Content: <null>" << std::endl;
         }
    }

    // --- Echo Back Logic --- 
    if (!group.empty()) {
        uint32_t original_target_id = group[0].target_id; // Assuming target_id is same for the group
        uint32_t response_target_id = original_target_id;
        send_acknowledgement_group(group_id, response_target_id); // Send ACK back
    } else {
         std::cerr << "[SamplePlugin BPG] Warning: Received empty group (ID: " << group_id << "), cannot echo back." << std::endl;
    }
}

// --- Plugin Interface Implementation --- 

static PluginInfo plugin_info = {
    "Sample Plugin (BPG + Python IPC)", // Updated name
    "1.3.0", // Version bump
    PLUGIN_API_VERSION
};

static PluginStatus initialize(MessageCallback send_message, 
                             BufferRequestCallback buffer_request_callback,
                             BufferSendCallback buffer_send_callback) {
    g_send_message = send_message;
    g_buffer_request_callback = buffer_request_callback;
    g_buffer_send_callback = buffer_send_callback;
    g_bpg_decoder.reset(); // Reset decoder state on initialization
    
    printf("Sample Plugin Initializing...\n");

    // Define paths for Python IPC
    // TODO: Make these paths configurable or relative?
    std::string python_executable = "/Users/mdm/workspace/LittleJourney/NNLoc/.venv/bin/python";
    // Use the NEW bidirectional script
    std::string script_path = "python_bidirectional_ipc_script.py"; // Updated path relative to APP/backend
    
    // Initialize Python IPC Channel (Bidirectional)
    // Pass the handle_python_data callback
    if (!init_acceptor_ipc_bidirectional(python_executable, script_path, handle_python_data)) {
        std::cerr << "FATAL: Failed to initialize Bi-directional Python IPC channel." << std::endl;
        return PLUGIN_ERROR_INITIALIZATION;
    }
    

    //print CWD
    std::cout << "Current working directory: " << getcwd(nullptr, 0) << std::endl;
    // --- Load ImgSrc_Opencv_webcam plugin ---
    const char* plugin_path = "/Users/mdm/workspace/LittleJourney/ElectronSharedBuffer/APP/backend/build/plugins/ImgSrc_Opencv_webcam/libImgSrc_Opencv_webcam.dylib"; // adjust path as needed
    imgsrc_lib_handle = dlopen(plugin_path, RTLD_NOW);
    if (!imgsrc_lib_handle) {
        std::cerr << "[SamplePlugin] dlopen failed: " << dlerror() << std::endl;
        return PLUGIN_ERROR_INITIALIZATION;
    }
    auto get_itpif = reinterpret_cast<ITPIF_PluginInterface*(*)()>(dlsym(imgsrc_lib_handle, "ITPIF_GetPluginInterface"));
    if (!get_itpif) {
        std::cerr << "[SamplePlugin] dlsym ITPIF_GetPluginInterface failed: " << dlerror() << std::endl;
        return PLUGIN_ERROR_INITIALIZATION;
    }
    imgsrc_interface = get_itpif();
    if (!imgsrc_interface) {
        std::cerr << "[SamplePlugin] ImgSrc interface is null" << std::endl;
        return PLUGIN_ERROR_INITIALIZATION;
    }
    // Set manager callbacks
    imgsrc_manager.dispatch = mgr_dispatch;
    imgsrc_manager.getNLockGlobalValue = mgr_getNLockGlobalValue;
    imgsrc_manager.unLockGlobalValue = mgr_unLockGlobalValue;
    imgsrc_manager.requestImg = mgr_requestImg;
    imgsrc_manager.requestStageInfo = mgr_requestStageInfo;
    // Create plugin instance
    cJSON* def = cJSON_CreateObject();
    imgsrc_instance = imgsrc_interface->create("opencv_cam", def, nullptr, &imgsrc_manager, nullptr);
    cJSON_Delete(def);
    if (!imgsrc_instance) {
        std::cerr << "[SamplePlugin] ImgSrc create instance failed" << std::endl;
        return PLUGIN_ERROR_INITIALIZATION;
    }
    std::cout << "[SamplePlugin] Loaded ImgSrc_Opencv_webcam plugin" << std::endl;

    std::cout << "Sample Plugin Initialized Successfully (with Bi-directional Python IPC)." << std::endl;
    return PLUGIN_SUCCESS;
}

static void shutdown() {
    std::cout << "Sample plugin shutting down..." << std::endl;
    // Shutdown ImgSrc plugin
    if (imgsrc_interface && imgsrc_instance) {
        imgsrc_interface->destroy(imgsrc_instance);
    }
    if (imgsrc_lib_handle) {
        dlclose(imgsrc_lib_handle);
        imgsrc_lib_handle = nullptr;
    }
    // Shutdown Bi-directional Python IPC Channel
    shutdown_acceptor_ipc_bidirectional();
    // Reset callbacks
    g_send_message = nullptr;
    g_buffer_request_callback = nullptr;
    g_buffer_send_callback = nullptr;
    std::cout << "Sample plugin shutdown complete." << std::endl;
}

// Process incoming raw data from the host using the BPG decoder
static void process_message(const uint8_t* data, size_t length) {
    std::cout << "Sample plugin received raw data length: " << length << std::endl;
    
    // Feed data into the BPG decoder
    BPG::BpgError decode_err = g_bpg_decoder.processData(
        data, 
        length, 
        handle_decoded_packet, // Callback for individual packets
        handle_decoded_group   // Callback for completed groups
    );
    std::cout << "processed " << std::endl;
    if (decode_err != BPG::BpgError::Success) {
        std::cerr << "[SamplePlugin BPG] Decoder error: " << static_cast<int>(decode_err) << std::endl;
        // Decide how to handle decoder errors (e.g., reset decoder?)
        // g_bpg_decoder.reset(); 
    }
}

static void update() {
    // Called periodically by the host
    // Could potentially check for timeouts on incomplete BPG groups here if needed
}

// Plugin interface instance
static PluginInterface plugin_interface = {
    plugin_info,
    initialize,
    shutdown,
    process_message,
    update
};

// Plugin entry point
extern "C" PLUGIN_EXPORT const PluginInterface* get_plugin_interface() {
    return &plugin_interface;
} 