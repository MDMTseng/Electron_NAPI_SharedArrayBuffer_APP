# ElectronSharedBuffer Communication System Guide

## Table of Contents
- [Overview](#overview)
- [Core Architecture](#core-architecture)
  - [Memory Layout](#memory-layout)
  - [Communication Flow](#communication-flow)
  - [Synchronization Mechanism](#synchronization-mechanism)
- [Developer Guide](#developer-guide)
  - [Step 1: Understanding the Components](#step-1-understanding-the-components)
  - [Step 2: Setting Up Your Project](#step-2-setting-up-your-project)
  - [Step 3: Implementing Your Plugin](#step-3-implementing-your-plugin)
  - [Step 4: Frontend Integration](#step-4-frontend-integration)
  - [Step 5: Building and Packaging](#step-5-building-and-packaging)
- [API Reference](#api-reference)
  - [Frontend API: SharedMemoryChannel](#frontend-api-sharedmemorychannel)
  - [Backend API: PluginInterface](#backend-api-plugininterface)
- [Advanced Usage](#advanced-usage)
  - [Direct Send Mode](#direct-send-mode)
  - [Message Queueing](#message-queueing)
  - [Throttled Processing](#throttled-processing)
- [Performance Optimization](#performance-optimization)
- [Security Considerations](#security-considerations)
- [Troubleshooting](#troubleshooting)

## Overview

ElectronSharedBuffer provides a high-performance bidirectional communication channel between Electron's frontend (JavaScript/TypeScript) and backend (C++ native code) using SharedArrayBuffer. This eliminates serialization overhead and enables efficient high-throughput data exchange.

### Key Advantages

- **Zero-Copy Communication**: Direct memory access without serialization/deserialization overhead
- **Bidirectional Channel**: Full-duplex communication between JavaScript and C++
- **High Performance**: Designed for efficient handling of large amounts of data
- **Message Queueing**: Automatic batching and processing of messages
- **Controlled Resource Usage**: Throttled processing to maintain UI responsiveness

## Core Architecture

### Memory Layout

The system uses a shared memory region organized into three sections:

```
SharedArrayBuffer Layout:
+----------------+------------------+------------------+
|   Control (16B)|    R→N Buffer   |    N→R Buffer   |
+----------------+------------------+------------------+
```

- **Control Section (16 bytes)**: Four 32-bit integers used for signaling
  - [0] - Renderer-to-Native signal
  - [1] - Renderer-to-Native message length
  - [2] - Native-to-Renderer signal
  - [3] - Native-to-Renderer message length
- **Renderer-to-Native Buffer**: For data sent from JavaScript to C++
- **Native-to-Renderer Buffer**: For data sent from C++ to JavaScript

### Communication Flow

#### JavaScript to C++ (Renderer to Native)
1. JavaScript calls `send()` on the `SharedMemoryChannel` instance
2. Message is added to the queue
3. Queue processor checks if the signal channel is clear (Atomics.load)
4. When clear, the message is written to the shared buffer
5. Signal is set to notify native code (Atomics.store)
6. Native code processes the message and resets the signal

#### C++ to JavaScript (Native to Renderer)
1. C++ writes data to the shared buffer
2. Signal is set to indicate new data is available
3. JavaScript polls for the signal in its processing loop
4. When a signal is detected, JavaScript reads the data
5. JavaScript resets the signal to acknowledge receipt
6. Process repeats for new messages

### Synchronization Mechanism

The communication is synchronized using atomic operations:
- JavaScript: `Atomics.load` and `Atomics.store`
- C++: `std::atomic` operations

This ensures proper synchronization without race conditions.

## Developer Guide

This section explains step-by-step how to use this framework in your own application.

### Step 1: Understanding the Components

The ElectronSharedBuffer system consists of three main components:

1. **Native Addon (`addon.node`)**
   - Core C++ implementation of the SharedMemoryChannel
   - Provided by the framework - no need to build it yourself
   - Manages the shared memory and plugin loading

2. **Backend Plugin (Your C++ Code)**
   - Your custom plugin implementing the `PluginInterface`
   - Processes messages from the frontend
   - Sends responses back to the frontend

3. **Frontend (Your JavaScript/TypeScript Code)**
   - Uses `SharedMemoryChannel` to communicate with your plugin
   - Sends commands and receives responses
   - Manages the user interface

### Step 2: Setting Up Your Project

1. **Directory Structure**
   Create a project with the following basic structure:
   ```
   YourApp/
   ├── native/
   │   └── addon.node       # Copy from ElectronSharedBuffer
   ├── backend/
   │   ├── include/
   │   │   └── plugin_interface.h  # Copy from ElectronSharedBuffer
   │   └── your_plugin.cpp  # Your plugin implementation
   ├── frontend/
   │   ├── src/
   │   │   ├── lib/
   │   │   │   ├── SharedMemoryChannel.ts  # Copy from ElectronSharedBuffer
   │   │   │   └── nativeAddon.ts          # Copy from ElectronSharedBuffer
   │   │   └── Your frontend code...
   │   └── index.html
   └── build_scripts/
       └── build_plugin.sh  # Script to build your plugin
   ```

2. **Dependencies**
   Make sure your `package.json` includes:
   ```json
   "dependencies": {
     "electron": "^29.0.0"
   },
   "devDependencies": {
     "node-addon-api": "^7.0.0",
     "electron-builder": "^24.0.0"
   }
   ```

### Step 3: Implementing Your Plugin

Create your plugin by implementing the `PluginInterface`:

```cpp
// your_plugin.cpp
#include "plugin_interface.h"
#include <string.h>
#include <stdio.h>

// Message callback to send data back to JavaScript
static MessageCallback g_messageCallback = nullptr;
static BufferRequestCallback g_bufferRequestCallback = nullptr;
static BufferSendCallback g_bufferSendCallback = nullptr;

// Plugin initialization
PluginStatus initialize(MessageCallback callback, 
                        BufferRequestCallback buffer_request_callback,
                        BufferSendCallback buffer_send_callback) {
    // Store the callback for later use
    g_messageCallback = callback;
    g_bufferRequestCallback = buffer_request_callback;
    g_bufferSendCallback = buffer_send_callback;
    
    printf("Plugin initialized\n");
    return PLUGIN_SUCCESS;
}

// Plugin shutdown
void shutdown() {
    printf("Plugin shutdown\n");
    g_messageCallback = nullptr;
    g_bufferRequestCallback = nullptr;
    g_bufferSendCallback = nullptr;
}

// Process messages from JavaScript
void process_message(const uint8_t* data, size_t length) {
    printf("Received message of length %zu\n", length);
    
    // Echo the message back as an example
    if (g_messageCallback) {
        g_messageCallback(data, length);
    }
    
    // Or use direct buffer access for more efficient transfer
    if (g_bufferRequestCallback && g_bufferSendCallback) {
        uint8_t* buffer = nullptr;
        uint32_t buffer_space = 0;
        
        // Request a buffer to write to
        int result = g_bufferRequestCallback(1000, &buffer, &buffer_space);
        if (result == 0 && buffer && buffer_space >= length) {
            // Copy data to the buffer
            memcpy(buffer, data, length);
            
            // Send the data
            g_bufferSendCallback(length);
        }
    }
}

// Periodic update function
void update() {
    // Called periodically by the host
    // Can be used for background processing
}

// Plugin info
static PluginInfo g_pluginInfo = {
    "MyPlugin",       // name
    "1.0.0",          // version
    PLUGIN_API_VERSION // API version
};

// Plugin interface
static PluginInterface g_interface = {
    g_pluginInfo,  // Plugin info
    initialize,    // Initialize function
    shutdown,      // Shutdown function
    process_message, // Process message function
    update         // Update function
};

// Plugin entry point - required export
#ifdef __cplusplus
extern "C" {
#endif

PLUGIN_EXPORT const PluginInterface* get_plugin_interface() {
    return &g_interface;
}

#ifdef __cplusplus
}
#endif
```

Build your plugin with a script like this:

```bash
#!/bin/bash
# build_plugin.sh

# Determine OS-specific extension
if [[ "$OSTYPE" == "darwin"* ]]; then
    EXT=".dylib"
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    EXT=".so"
elif [[ "$OSTYPE" == "msys"* || "$OSTYPE" == "win32" ]]; then
    EXT=".dll"
else
    echo "Unsupported OS type: $OSTYPE"
    exit 1
fi

# Compile the plugin
g++ -shared -fPIC -o backend/libmy_plugin$EXT \
    -Ibackend/include \
    backend/your_plugin.cpp
```

### Step 4: Frontend Integration

1. **Import the Required Modules**

```typescript
// In your component
import { SharedMemoryChannel } from './lib/SharedMemoryChannel';
import { nativeAddon } from './lib/nativeAddon';
import path from 'path';
```

2. **Load Your Plugin**

```typescript
// Determine the correct path based on environment
let pluginPath;
if (process.env.NODE_ENV === 'development') {
  // Development path
  pluginPath = path.join(__dirname, '../backend/libmy_plugin.dylib');
} else {
  // Production path
  pluginPath = path.join(process.resourcesPath, 'backend/libmy_plugin.dylib');
}

// Load the plugin
const success = nativeAddon.loadPlugin(pluginPath);
if (!success) {
  console.error('Failed to load plugin');
}
```

3. **Create and Configure the Communication Channel**

```typescript
// Create a channel with 1MB buffers in each direction
const channel = new SharedMemoryChannel(1024 * 1024, 1024 * 1024);

// Set up message reception
channel.startReceiving((message) => {
  // Process incoming messages from the plugin
  console.log('Received message:', message);
  
  // If message is binary, you might need to decode it
  // Example: Parse binary message
  const messageType = message[0];
  const payload = message.slice(1);
  
  // Handle different message types
  switch (messageType) {
    case 0x01: // Example message type
      handleTypeOneMessage(payload);
      break;
    case 0x02: // Another message type
      handleTypeTwoMessage(payload);
      break;
  }
});
```

4. **Send Messages to Your Plugin**

```typescript
// Send a simple message
const sendSimpleMessage = () => {
  const message = new Uint8Array([0x01, 0x02, 0x03, 0x04]);
  channel.send(message);
};

// Send a structured message
const sendStructuredMessage = (type, data) => {
  // Create a message with a type byte followed by data
  const message = new Uint8Array(1 + data.length);
  message[0] = type;
  message.set(data, 1);
  channel.send(message);
};

// Send a message that requires immediate response
const sendUrgentMessage = async (data) => {
  try {
    await channel.send_direct(data, 2000); // 2 second timeout
    console.log('Message sent successfully');
  } catch (error) {
    console.error('Send failed:', error);
  }
};
```

5. **Clean Up Resources**

```typescript
// In your component cleanup or unmount
const cleanup = () => {
  if (channel) {
    channel.cleanup();
  }
};

// For React components:
useEffect(() => {
  // Setup code
  
  return () => {
    cleanup();
  };
}, []);
```

### Step 5: Building and Packaging

1. **Build Your Plugin**
   ```bash
   ./build_scripts/build_plugin.sh
   ```

2. **Configure Electron Builder**
   In your `package.json`:
   ```json
   "build": {
     "appId": "your.app.id",
     "files": [
       "dist/**/*",
       "node_modules/**/*",
       "package.json"
     ],
     "extraResources": [
       {
         "from": "backend/",
         "to": "backend/",
         "filter": ["**/*.dylib", "**/*.dll", "**/*.so"]
       },
       {
         "from": "native/",
         "to": "native/"
       }
     ]
   }
   ```

3. **Build Your Electron App**
   ```bash
   npm run build
   npm run electron:build
   ```

## API Reference

### Frontend API: SharedMemoryChannel

The `SharedMemoryChannel` class provides the following key methods:

```typescript
class SharedMemoryChannel {
  // Constructor - specify buffer sizes in bytes
  constructor(rendererToNativeSize: number, nativeToRendererSize: number);
  
  // Queue a message for sending
  send(messageBytes: Uint8Array): void;
  
  // Send a message directly with timeout
  send_direct(messageBytes: Uint8Array, wait_ms: number): Promise<void>;
  
  // Start receiving messages
  startReceiving(callback: (message: Uint8Array) => void): void;
  
  // Stop receiving messages
  stopReceiving(): void;
  
  // Clean up resources
  cleanup(): void;
  
  // Optional: Callback when message queue is empty
  onMessageQueueEmptyCallback: (() => void) | null;
  
  // Configure throttling for UI responsiveness
  queueUpdateThrottle: Throttle;
}
```

### Backend API: PluginInterface

Your C++ plugin must implement this interface:

```cpp
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

// Callback types
typedef void (*MessageCallback)(const uint8_t* data, size_t length);
typedef int (*BufferRequestCallback)(uint32_t wait_ms, uint8_t** buffer, uint32_t* buffer_space);
typedef int (*BufferSendCallback)(uint32_t data_length);

// Plugin interface
typedef struct {
    // Plugin information
    PluginInfo info;
    
    // Initialize the plugin
    PluginStatus (*initialize)(MessageCallback callback,
                              BufferRequestCallback buffer_request_callback,
                              BufferSendCallback buffer_send_callback);
    
    // Cleanup and shutdown the plugin
    void (*shutdown)();
    
    // Process incoming message from the renderer
    void (*process_message)(const uint8_t* data, size_t length);
    
    // Called periodically by the host
    void (*update)();
} PluginInterface;
```

## Advanced Usage

### Direct Send Mode

to prevent datacopy to queue, use `send_direct` instead of `send`:

```typescript
// Send with a 1000ms timeout
try {
  await channel.send_direct(data, 1000);
  console.log('Message sent successfully');
} catch (error) {
  console.error('Send timed out or failed');
}
```

### Message Queueing

The system automatically queues messages for efficiency:

```typescript
// Enqueue multiple messages - they'll be processed efficiently
channel.send(message1);
channel.send(message2);
channel.send(message3);

// Optional: Be notified when all messages are sent
channel.onMessageQueueEmptyCallback = () => {
  console.log('All messages processed');
  // Trigger next operation
};
```

### Throttled Processing

Control CPU usage with throttling:

```typescript
// Adjust throttling for better UI responsiveness
channel.queueUpdateThrottle = new Throttle(200); // 200ms

// Import Throttle class
import { Throttle } from './lib/throttle';
```

## Performance Optimization

For optimal performance:

1. **Choose Appropriate Buffer Sizes**
   - Smaller buffers for frequent small messages (e.g., 16KB-64KB)
   - Larger buffers for occasional large data transfers (e.g., 1MB-10MB)
   - Extreme cases can use up to 500MB buffers

2. **Use Binary Message Format**
   - Design a compact binary protocol for your messages
   - Use typed arrays (Uint8Array) instead of strings
   - Consider message type indicators at the start of your messages

3. **Batch Operations When Possible**
   - Group related messages together
   - Reduce the number of send operations for better throughput

4. **Optimize Polling Intervals**
   - Adjust `recv_fast_check_interval` and `recv_slow_check_interval` based on your use case
   - Faster polling for real-time applications
   - Slower polling for background tasks

## Security Considerations

When using SharedArrayBuffer:

1. **Electron Security Headers**
   - SharedArrayBuffer requires specific headers:
   ```javascript
   // In main.js
   app.on('ready', () => {
     session.defaultSession.webRequest.onHeadersReceived((details, callback) => {
       callback({
         responseHeaders: {
           ...details.responseHeaders,
           'Cross-Origin-Opener-Policy': ['same-origin'],
           'Cross-Origin-Embedder-Policy': ['require-corp']
         }
       });
     });
   });
   ```

2. **Buffer Validation**
   - Always validate message sizes before processing
   - Check buffer boundaries to prevent overflows
   - Use defensive coding in C++ plugin implementations

3. **Plugin Path Validation**
   - Validate plugin paths to prevent loading untrusted code
   - Use absolute paths in production environments

## Troubleshooting

Common issues and solutions:

1. **Plugin Loading Fails**
   - Check file path correctness
   - Verify file permissions
   - Ensure the plugin is compiled for the correct platform
   - Check for missing dependencies

2. **Messages Not Being Received**
   - Verify signal handlers are properly initialized
   - Check that `startReceiving()` has been called
   - Ensure the plugin's `process_message` function is working

3. **Performance Issues**
   - Adjust buffer sizes based on your data needs
   - Check for excessive message frequency
   - Use appropriate throttling settings
   - Consider batching small messages

4. **Memory Leaks**
   - Ensure `cleanup()` is called when components unmount
   - Verify plugin's `shutdown()` function releases all resources
   - Check for any unreleased buffer references 