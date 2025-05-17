#include <opencv2/opencv.hpp>
#include <opencv2/videoio.hpp>
#include <vector>
#include <string>
#include <memory>
#include <iostream>
#include <mutex>
#include <cstring> // For strcmp
#include <thread>
#include <atomic>
#include <chrono>

#include "InspTarPluginInterface.h" // Assuming this is in the include path set by parent CMake

// Define PLUGIN_EXPORT based on platform (basic example)
#if defined(_WIN32) || defined(_WIN64)
    #define PLUGIN_EXPORT __declspec(dllexport)
#else
    #define PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

struct PluginState {
    cv::VideoCapture cap;
    ITPIF_ManagerInterface manager; // Copy of manager interface
    void* main_ctx = nullptr;       // Context from the manager/host
    std::string plugin_id;          // ID assigned to this plugin instance
    int plugin_pgID = 0;            // Placeholder for pgID used in communication
    std::mutex mtx;
    std::atomic<bool> streaming{false};
    std::thread stream_thread;
    int req_width = 640;
    int req_height = 480;
    int req_channels = 3;
    int req_type = CV_8UC3;
};

// --- Plugin Interface Implementation ---

extern "C" { // Start extern "C"

void* CreatePluginInstance(const char *id, cJSON* def, const char *local_env_path, struct ITPIF_ManagerInterface* manager, void* main_ctx) {
    PluginState* state = new (std::nothrow) PluginState();
    if (!state) {
        std::cerr << "ImgSrc_Opencv_webcam: Failed to allocate plugin state for ID: " << (id ? id : "null") << std::endl;
        return nullptr;
    }

    state->manager = *manager; // Copy manager interface
    state->main_ctx = main_ctx;
    state->plugin_id = (id ? id : "");
    state->plugin_pgID = 0; // Placeholder

    // Optional: Use 'def' or 'local_env_path' to configure camera index, resolution etc.
    // cJSON* camera_index_item = cJSON_GetObjectItemCaseSensitive(def, "camera_index");
    // int camera_index = (cJSON_IsNumber(camera_index_item)) ? camera_index_item->valueint : 0;
    int camera_index = 0; // Default to 0

    std::cout << "ImgSrc_Opencv_webcam [" << state->plugin_id << "]: Attempting to open webcam (index " << camera_index << ")..." << std::endl;
    state->cap.open(camera_index); // Open the specified camera

    if (!state->cap.isOpened()) {
        std::cerr << "ImgSrc_Opencv_webcam [" << state->plugin_id << "]: Error opening video stream (index " << camera_index << ")" << std::endl;
        delete state;
        return nullptr;
    }
    std::cout << "ImgSrc_Opencv_webcam [" << state->plugin_id << "]: Webcam opened successfully." << std::endl;

    // ensure streaming flags initialized
    state->streaming = false;
    state->req_width = 640;
    state->req_height = 480;
    state->req_channels = 3;
    state->req_type = CV_8UC3;

    return static_cast<void*>(state);
}

// Matches: typedef void (*ITPIF_DestroyPluginInstance)(void* instance);
void DestroyPluginInstance(void* instance) {
    if (!instance) return;
    PluginState* state = static_cast<PluginState*>(instance);
    // Stop streaming thread if running
    if (state->streaming) {
        state->streaming = false;
        if (state->stream_thread.joinable()) state->stream_thread.join();
    }
    std::cout << "ImgSrc_Opencv_webcam [" << state->plugin_id << "]: Releasing webcam." << std::endl;
    if (state->cap.isOpened()) {
        state->cap.release();
    }
    delete state;
    std::cout << "ImgSrc_Opencv_webcam: Plugin instance destroyed." << std::endl;
}

// Matches: typedef void (*ITPIF_setEnvPath)(void* instance, const char *path);
void setEnvPath(void* instance, const char* path) {
    if (!instance) return;
    PluginState* state = static_cast<PluginState*>(instance);
    std::cout << "ImgSrc_Opencv_webcam [" << state->plugin_id << "]: setEnvPath called (path ignored)." << std::endl;
    (void)path;
}

// Matches: typedef int (*ITPIF_PluginSetDef)(void* instance, cJSON* def);
int PluginSetDef(void* instance, cJSON* def) {
    if (!instance) return -1;
    PluginState* state = static_cast<PluginState*>(instance);
    std::cout << "ImgSrc_Opencv_webcam [" << state->plugin_id << "]: PluginSetDef called." << std::endl;
    (void)def;
    return 0;
}

// Matches: typedef int (*ITPIF_PluginExchangeCMD)(void* instance, cJSON *info, int id, struct ITPIF_CMDActInterface act);
int PluginExchangeCMD(void* instance, cJSON *info, int id, struct ITPIF_CMDActInterface act) {
    if (!instance || !info) return -1;
    PluginState* state = static_cast<PluginState*>(instance);
    int result = 0;

    cJSON* command_item = cJSON_GetObjectItemCaseSensitive(info, "command");
    if (!cJSON_IsString(command_item) || (command_item->valuestring == NULL)) {
        std::cerr << "ImgSrc_Opencv_webcam [" << state->plugin_id << "]: Invalid or missing 'command' in info JSON." << std::endl;
        if (act.sendACK) {
             act.sendACK(state->plugin_pgID, 0, "{\"error\":\"Invalid command JSON\"}");
        }
        return -1;
    }

    const char* command = command_item->valuestring;
    std::cout << "ImgSrc_Opencv_webcam [" << state->plugin_id << "]: Received command: " << command << " (id: " << id << ")" << std::endl;

    // Handle start/stop streaming commands
    if (strcmp(command, "START_STREAM") == 0) {
        if (!state->streaming) {
            state->streaming = true;
            state->stream_thread = std::thread([state]() {
                while (state->streaming) {
                    auto stage = state->manager.requestStageInfo(state->main_ctx);
                    auto imgInfo = state->manager.requestImg(
                        state->main_ctx,
                        state->req_width,
                        state->req_height,
                        state->req_channels,
                        state->req_type
                    );
                    stage.img = imgInfo;
                    state->manager.dispatch(state->main_ctx, &stage);
                    std::this_thread::sleep_for(std::chrono::milliseconds(33));
                }
            });
        }
        if (act.sendACK) {
            act.sendACK(state->plugin_pgID, 1, "{\"status\":\"START_STREAM_OK\"}");
        }
        return 0;
    }
    if (strcmp(command, "STOP_STREAM") == 0) {
        if (state->streaming) {
            state->streaming = false;
            if (state->stream_thread.joinable()) state->stream_thread.join();
        }
        if (act.sendACK) {
            act.sendACK(state->plugin_pgID, 1, "{\"status\":\"STOP_STREAM_OK\"}");
        }
        return 0;
    }

    if (strcmp(command, "GET_FRAME") == 0) {
        cv::Mat frame;
        bool success = false;
        {
            std::lock_guard<std::mutex> lock(state->mtx);
            if (state->cap.isOpened()) {
                success = state->cap.read(frame);
            }
        }

        if (success && !frame.empty()) {
            ITPIF_ImageInfo imgInfo = ITPIF_ImageInfoInit();
            imgInfo.buffer = frame.data;
            imgInfo.width = frame.cols;
            imgInfo.height = frame.rows;
            imgInfo.channels = frame.channels();
            imgInfo.step = frame.step;
            imgInfo.type = frame.type();
            imgInfo.elemSize = frame.elemSize();
            imgInfo.totalSize = frame.total() * frame.elemSize();

            if (act.sendImage) {
                act.sendImage(state->plugin_pgID, &imgInfo, "raw", 1.0f);
            } else {
                 std::cerr << "ImgSrc_Opencv_webcam [" << state->plugin_id << "]: sendImage function pointer is null." << std::endl;
            }

            if (act.sendACK) {
                act.sendACK(state->plugin_pgID, 1, "{\"status\":\"ACK_GET_FRAME_SUCCESS\"}");
            }
        } else {
            std::cerr << "ImgSrc_Opencv_webcam [" << state->plugin_id << "]: Failed to capture frame." << std::endl;
            if (act.sendACK) {
                act.sendACK(state->plugin_pgID, 0, "{\"error\":\"Failed to capture frame\"}");
            }
            result = -1;
        }
    } else {
        std::cerr << "ImgSrc_Opencv_webcam [" << state->plugin_id << "]: Unknown command '" << command << "'." << std::endl;
        if (act.sendACK) {
             act.sendACK(state->plugin_pgID, 0, "{\"error\":\"NACK_UNKNOWN_COMMAND\"}");
        }
        result = -1;
    }
    return result;
}

// Matches: typedef int (*ITPIF_PluginProcess)(void* instance, struct ITPIF_StageInfo_c* data);
int PluginProcess(void* instance, struct ITPIF_StageInfo_c* data) {
    if (!instance) return -1;
    PluginState* state = static_cast<PluginState*>(instance);
    (void)state; // Use state to avoid unused variable warning if not otherwise used
    (void)data;
    // Placeholder implementation - does nothing
    return 0;
}

} // End extern "C"

// --- Plugin Interface Definition ---
// This struct itself doesn't need C linkage, but the functions it points to do.
struct ITPIF_PluginInterface plugin_interface = {
    CreatePluginInstance,
    DestroyPluginInstance,
    setEnvPath,
    PluginSetDef,
    PluginExchangeCMD,
    PluginProcess
};

// This function needs C linkage as defined in the header.
extern "C" PLUGIN_EXPORT ITPIF_PluginInterface* ITPIF_GetPluginInterface() {
    return &plugin_interface;
}