#ifndef PLUGIN_INTERFACE_H
#define PLUGIN_INTERFACE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

// Plugin API version
#define PLUGIN_API_VERSION 1

// Plugin initialization status
typedef enum {
    PLUGIN_SUCCESS = 0,
    PLUGIN_ERROR_INVALID_VERSION = -1,
    PLUGIN_ERROR_INITIALIZATION = -2
} PluginStatus;

// Plugin info structure
typedef struct {
    const char* name;
    const char* version;
    uint32_t api_version;
} PluginInfo;

// Message callback type
typedef void (*MessageCallback)(const uint8_t* data, size_t length);

typedef int (*BufferRequestCallback)(uint32_t wait_ms,uint8_t** buffer, uint32_t* buffer_sapce);

typedef int (*BufferSendCallback)(uint32_t data_length);


// Plugin interface
typedef struct {
    // Plugin information
    PluginInfo info;
    
    // Initialize the plugin
    PluginStatus (*initialize)(
        MessageCallback callback,
        BufferRequestCallback buffer_request_callback,
        BufferSendCallback buffer_send_callback);
    
    // Cleanup and shutdown the plugin
    void (*shutdown)();
    
    // Process incoming message from the renderer
    void (*process_message)(const uint8_t* data, size_t length);
    
    // Called periodically by the host
    void (*update)();
} PluginInterface;

// Plugin entry point - must be implemented by each plugin
#ifdef _WIN32
    #define PLUGIN_EXPORT __declspec(dllexport)
#else
    #define PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

PLUGIN_EXPORT const PluginInterface* get_plugin_interface();

#ifdef __cplusplus
}
#endif

#endif // PLUGIN_INTERFACE_H 