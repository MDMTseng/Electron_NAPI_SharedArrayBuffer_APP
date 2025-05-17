import React, { useEffect, useState, useRef, useCallback } from 'react';
import { SharedMemoryChannel } from './lib/SharedMemoryChannel';
import { nativeAddon } from './lib/nativeAddon';
// Import BPG Types needed for the App component
import { AppPacket, AppPacketGroup } from './lib/BPG_Protocol';
// Import the custom hook
import { useBPGProtocol, BPGPacketDescriptor, UseBPGProtocolOptions } from './hooks/useBPGProtocol';
import './App.css';

// Request ipcRenderer from main process (safer than direct require if contextIsolation is enabled)
const { ipcRenderer } = window.require ? window.require('electron') : { ipcRenderer: null }; 
// Basic check if we are in electron environment
const isElectron = !!ipcRenderer;

// Helper function to get hex preview of bytes
const bytesToHexPreview = (bytes: Uint8Array, maxBytes: number = 30): string => {
    if (!bytes || bytes.length === 0) {
        return "(no binary data)";
    }
    const byteToHex = (byte: number) => byte.toString(16).padStart(2, '0').toUpperCase();
    
    const len = bytes.length;
    let preview = "";

    if (len <= maxBytes * 2) { // If small enough, show all
        preview = Array.from(bytes).map(byteToHex).join(' ');
    } else {
        const firstPart = Array.from(bytes.slice(0, maxBytes)).map(byteToHex).join(' ');
        const lastPart = Array.from(bytes.slice(len - maxBytes)).map(byteToHex).join(' ');
        preview = `First ${maxBytes}: ${firstPart} ... Last ${maxBytes}: ${lastPart}`;
    }
    return preview;
};

function App() {
    const thisRef = useRef<any>({}).current; // Using useRef for mutable values across renders
    const [messages, setMessages] = useState<string[]>([]);
    const [queueStatus, setQueueStatus] = useState('Queue: 0 messages');
    const [pluginStatus, setPluginStatus] = useState<string>('Waiting for config...');
    const [isSending, setIsSending] = useState<boolean>(false); // To disable button during request
    const [receivedImageData, setReceivedImageData] = useState<ImageData | null>(null); // State for the image data
    const canvasRef = useRef<HTMLCanvasElement>(null); // Ref for the canvas element
    const [artifactPath, setArtifactPath] = useState<string | null>(null); // Store artifact path for prod mode
    const [appMode, setAppMode] = useState<string | null>(null); // Store 'dev' or 'prod'
    const [isPluginLoadAttempted, setIsPluginLoadAttempted] = useState<boolean>(false);

    const [speedTest_sendCount, setSpeedTest_sendCount] = useState<number>(1);
    const [speedTestStatus, setSpeedTestStatus] = useState<string>('Not started');

    // --- Use the Custom Hook ---
    const handleUnhandledGroup = useCallback((group: AppPacketGroup) => {
        console.log("[App] Received Unhandled Group:", group);
        
        // Find first packet with binary data for UI log preview
        let firstBinaryPacket: AppPacket | undefined = undefined;
        for (const packet of group) {
            if (packet.content?.binary_bytes?.length > 0) {
                firstBinaryPacket = packet;
                break;
            }
        }

        let binaryPreview = "(No binary data in group)";
        if (firstBinaryPacket) {
            binaryPreview = `First Bin (${firstBinaryPacket.content.binary_bytes.length} bytes, TL:${firstBinaryPacket.tl}): ` + bytesToHexPreview(firstBinaryPacket.content.binary_bytes);
        }
        
        // Construct the message for the UI log, including the preview
        const groupInfo = `[BPG Unhandled Group] GID:${group[0]?.group_id}, Count:${group.length}, ${binaryPreview}`;
        setMessages(prev => [...prev, groupInfo]);

        // Keep detailed logging to console
        group.forEach(packet => {
            if (packet.content?.binary_bytes?.length > 0) {
                const hexPreview = bytesToHexPreview(packet.content.binary_bytes);
                const previewMsg = `  > Console Log: Unhandled TL:${packet.tl}, Bin (${packet.content.binary_bytes.length} bytes): ${hexPreview}`;
                console.log(previewMsg);
            }
        });
    }, []);

    const bpgOptions: UseBPGProtocolOptions = {
        tx_size: 500 * 1024 * 1024,
        rx_size: 500 * 1024 * 1024,
        unhandled_group: handleUnhandledGroup // Pass the callback
    };
    const { sendGroup, channel, isInitialized } = useBPGProtocol(bpgOptions);
    // --------------------------

    const [bpgTargetId, setBpgTargetId] = useState<number>(1); // Example target ID for messages sent from UI
    const bpgGroupIdRef = useRef<number>(301); // Use ref for group ID to avoid re-renders

    // --- Effect to get config from main process --- 
    useEffect(() => {
        if (!isElectron) {
            console.warn("Not running in Electron, cannot get app config.");
            setPluginStatus("Error: Not in Electron");
            setAppMode("dev"); // Assume dev if not Electron
            return;
        }

        const handleSetConfig = (event: any, config: { mode: string | null, artifactPath: string | null }) => {
             console.log('[App] Received app config from main:', config);
             setAppMode(config.mode);
             setArtifactPath(config.artifactPath);
             // Update status only if still waiting
             setPluginStatus(prev => prev === 'Waiting for config...' ? 'Config received, ready to load plugin.' : prev);
        };

        ipcRenderer?.on('set-app-config', handleSetConfig); // Use the new event name
        console.log('[App] Listening for app config...');

        // Cleanup listener on unmount
        return () => {
            ipcRenderer?.removeListener('set-app-config', handleSetConfig);
        };
    }, []);

    // --- Load/Unload Plugin (uses appMode and artifactPath) ---
    const loadPlugin = useCallback(() => {
        if (!appMode) { // Check if mode is set
            setPluginStatus("Waiting for app mode...");
            console.log("loadPlugin called before appMode was set.");
            return;
        }
        if (!nativeAddon) {
            setPluginStatus("Error: Native addon not loaded");
            return;
        }

        setIsPluginLoadAttempted(true); 
        setPluginStatus("Loading plugin...");

        const platform = process.platform;
        const pluginExt = platform === 'win32' ? '.dll' : platform === 'darwin' ? '.dylib' : '.so';
        const pluginPrefix = platform === 'win32' ? '' : 'lib';
        
        let constructedPath = '';
        if (appMode === "dev") {
            // In dev, path is relative to project root
            constructedPath = `${process.cwd()}/APP/backend/build/lib/${pluginPrefix}sample_plugin${pluginExt}`;
            console.log("Constructed DEV plugin path:", constructedPath);
        } else if (appMode === "prod") {
            if (!artifactPath) {
                setPluginStatus("Error: Artifact path needed for prod mode!");
                console.error("loadPlugin called in prod mode without artifactPath.");
                return;
            }
            // In prod, path is relative to the artifact path set via BIOS

            console.log("Artifact path:", artifactPath);
            constructedPath = `${artifactPath}/backend/${pluginPrefix}sample_plugin${pluginExt}`;
            console.log("Constructed PROD plugin path:", constructedPath);
        } else {
            setPluginStatus(`Error: Unknown app mode '${appMode}'`);
            console.error(`loadPlugin called with unknown mode: ${appMode}`);
            return;
        }

        try {
            const success = nativeAddon.loadPlugin(constructedPath);
            setPluginStatus(success ? 'Plugin loaded successfully' : 'Failed to load plugin');
        } catch (error: any) {
            setPluginStatus(`Error loading plugin: ${error.message || error}`);
            console.error("Error during nativeAddon.loadPlugin:", error);
        }
    }, [appMode, artifactPath, nativeAddon]); // Depend on mode and path

    const unloadPlugin = useCallback(() => {
        if (!nativeAddon) {
            setPluginStatus("Error: Native addon not loaded");
            return;
        }
        try {
            nativeAddon.unloadPlugin();
            setPluginStatus('Plugin unloaded');
            setIsPluginLoadAttempted(false); // Allow reloading
        } catch (error: any) {
            setPluginStatus(`Error unloading plugin: ${error.message || error}`);
            console.error("Error during nativeAddon.unloadPlugin:", error);
        }
    }, [nativeAddon]);

    // --- Effect to load plugin *after* app mode is set --- 
    useEffect(() => {
        if (appMode && !isPluginLoadAttempted) { // Check appMode instead of artifactPath
            console.log("App mode set, attempting to load plugin.");
            loadPlugin();
        }
    }, [appMode, isPluginLoadAttempted, loadPlugin]); // Depend on appMode now

    // --- Original useEffect for initial setup and cleanup --- (REMOVED plugin load/unload)
    useEffect(() => {
        // Initialize channel is handled by the hook now
        // loadPlugin(); // <-- REMOVED from here
        console.log("Initial App setup effect (excluding plugin load).");
        return () => {
            // unloadPlugin(); // <-- REMOVED from here (can be called manually or on window close)
            // Channel cleanup is handled by the hook now
            console.log("App unmounting cleanup (excluding plugin unload).");
        };
    }, []); // Empty dependency array ensures this runs only once on mount


    // --- Speed Test / Queue Status Update (uses channel from hook) ---
    useEffect(() => {
        if (channel) {
            // Assign the callback directly to the channel instance from the hook
            channel.onMessageQueueEmptyCallback = () => {
                thisRef.sentCounter = (thisRef.sentCounter || 0) + 1; // Increment counter
                channel.queueUpdateThrottle.schedule(() => {
                    updateQueueStatus(channel);
                    if (channel.messageQueue.length === 0 && thisRef.startTime) { // Check if timing started
                        let now = Date.now();
                        let duration = now - thisRef.startTime;
                        let size_MB = (thisRef.totalSize || 0) / 1024 / 1024;
                        let speed = duration > 0 ? 1000 * size_MB / duration : 0;
                        let status = `SendCount:${thisRef.sentCounter} ${size_MB.toFixed(2)} MB, ${duration} ms, ${speed.toFixed(2)} MB/s`;
                        setSpeedTestStatus(status);
                        console.log(status);
                        thisRef.startTime = null; // Reset start time after completion
                        setIsSending(false); // Re-enable button
                    }
                });
            };
        }
        // Cleanup callback when channel changes or component unmounts
        return () => {
            if (channel) {
                channel.onMessageQueueEmptyCallback = null;
            }
        };
    }, [channel]); // Re-run if channel instance changes


    const updateQueueStatus = (currentChannel: SharedMemoryChannel) => {
        setQueueStatus(`Queue: ${currentChannel.messageQueue.length} messages`);
    };

    // Send Message using the hook's sendGroup function
    const handleSendMessage = async () => {
        const messageInput = document.getElementById('messageInput') as HTMLInputElement;
        const message = messageInput.value;
        if (!message || !sendGroup || !channel || isSending) return; // Check hook function and sending state

        setIsSending(true); // Disable button
        setSpeedTestStatus('Sending...');
        setMessages(prev => [...prev, `--- Sending Request ---`]);

        const currentGroupId = bpgGroupIdRef.current;
        bpgGroupIdRef.current++; // Increment for next use

        // Define the packet(s) to send using descriptors
        const packetsToSend: BPGPacketDescriptor[] = [
            {
                tl: "TX",
                // str: "", // Optional metadata string
                bin: new TextEncoder().encode(message)
            }
            // Add more packet descriptors here if needed for the group
        ];

        thisRef.startTime = Date.now();
        thisRef.totalSize = 0; // Reset total size for this send batch
        thisRef.sentCounter = 0;
        const sendCount = speedTest_sendCount; // Use state variable

        // Calculate expected total size *before* sending for speed test
         try {
            const { BpgEncoder } = await import('./lib/BPG_Protocol'); // Dynamically import for size calc
            const encoder = new BpgEncoder(); // Temp encoder instance
            packetsToSend.forEach(desc => {
                 const tempPacket: AppPacket = { // Construct temporary AppPacket for size calculation
                     group_id: 0, target_id: 0, tl: desc.tl, is_end_of_group: false, // temp values
                     content: { metadata_str: desc.str || "", binary_bytes: desc.bin || new Uint8Array(0) }
                 };
                 thisRef.totalSize += encoder.encodePacket(tempPacket).length;
            });
            thisRef.totalSize *= sendCount; // Multiply by number of sends
         } catch (e: any) { console.error("Error calculating size", e); }


        logRequestPackets(currentGroupId, bpgTargetId, packetsToSend, sendCount);

        try {
             // Send multiple times for speed test
             for (let i = 0; i < sendCount; i++) {
                 const isLastIteration = i === sendCount - 1;
                 const loopGroupId = currentGroupId + i; // Send unique group IDs
                 
                 // Send the request
                 const promise = sendGroup(loopGroupId, bpgTargetId, packetsToSend);

                 if (isLastIteration) {
                    // Await only the last request in the batch
                    const responsePackets = await promise;
                    logResponsePackets(loopGroupId, responsePackets);
                    console.log(responsePackets);
                    // Speed test results are now handled by onMessageQueueEmptyCallback
                 } else {
                     // Don't wait for intermediate responses in speed test, but catch errors
                     promise.catch((error: Error) => {
                         console.error(`Error sending intermediate group ${loopGroupId}:`, error);
                         setMessages(prev => [...prev, `[BPG Send Error iter ${i}] GID:${loopGroupId}, Error: ${error.message}`]);
                     });
                 }
                 // Need to manually update queue status if not waiting,
                 // though onMessageQueueEmptyCallback should eventually handle it.
                 if(channel) updateQueueStatus(channel);
             }

        } catch (error: any) {
            console.error("Error sending BPG group:", error);
            setMessages(prev => [...prev, `[BPG Send Error] GID:${currentGroupId}, Error: ${error.message || error}`]);
            setSpeedTestStatus(`Error: ${error.message || error}`);
             setIsSending(false); // Re-enable button on error
             thisRef.startTime = null; // Reset start time on error
        }
        // Note: setIsSending(false) is now primarily handled by the queue empty callback
    };

     // Helper function to log request details
     const logRequestPackets = (groupId: number, targetId: number, descriptors: BPGPacketDescriptor[], count: number) => {
         // Log only the first group if count > 1 for brevity
         const displayGroupId = count > 1 ? `${groupId}...${groupId + count - 1}` : `${groupId}`;
         setMessages(prev => [...prev, `[BPG Sent] GID:${displayGroupId} (x${count}), TID:${targetId}, TLs: ${descriptors.map(p => p.tl).join(',')}`]);
         descriptors.forEach((desc, index) => {
             let contentPreview = `Req TL:${desc.tl}, EG:${index === descriptors.length - 1 ? 'Y' : 'N'}`;
             if (desc.str) contentPreview += `, Meta: ${desc.str.substring(0, 30)}...`;
             if (desc.bin) {
                 contentPreview += `, Bin Size: ${desc.bin.length}`;
                 const hexPreview = Array.from(desc.bin.slice(0, 16)).map((b: number) => b.toString(16).padStart(2, '0')).join(' ');
                 contentPreview += ` (Hex: ${hexPreview}${desc.bin.length > 16 ? '...' : ''})`;
                 if (!desc.str && desc.bin.length < 100) {
                     try { const text = new TextDecoder().decode(desc.bin); if (/^[ -~\s]*$/.test(text)) contentPreview += ` (as text: "${text}")` } catch (e: any) { }
                 }
             }
             // Only log details for the first group if sending many
             if (count === 1) {
                 setMessages(prev => [...prev, ` > ${contentPreview}`]);
             }
         });
     };

     // Helper function to log response details
     const logResponsePackets = (originalGroupId: number, responsePackets: AppPacket[]) => {
         setMessages(prev => [...prev, `[BPG Resp Complete] GID:${originalGroupId}, Count:${responsePackets.length}`]);
         responsePackets.forEach(packet => {
             let isImagePacket = false; // Flag to check if we handled this as an image
             let contentPreview = `Resp TL:${packet.tl}, EG:${packet.is_end_of_group ? 'Y' : 'N'}, TID:${packet.target_id}`;
             if (packet.content.metadata_str) contentPreview += `, Meta: ${packet.content.metadata_str.substring(0, 30)}...`;
             if (packet.content.binary_bytes.length > 0) {
                 contentPreview += `, Bin Size: ${packet.content.binary_bytes.length}`;
                 // Use the new helper function for hex preview
                 const hexPreview = bytesToHexPreview(packet.content.binary_bytes);
                 contentPreview += ` (Hex: ${hexPreview})`; 
                  if (!packet.content.metadata_str && packet.content.binary_bytes.length < 100) {
                     try { const text = new TextDecoder().decode(packet.content.binary_bytes); if (/^[ -~\s]*$/.test(text)) contentPreview += ` (as text: "${text}")` } catch (e: any) { }
                  }
             }

             // --- Handle "IM" packets ---
             if (packet.tl === 'IM' && packet.content.metadata_str && packet.content.binary_bytes.length > 0) {
                 try {
                     const metadata = JSON.parse(packet.content.metadata_str);
                     if (metadata.format === 'raw_rgba' && metadata.width > 0 && metadata.height > 0) {
                         const width = metadata.width;
                         const height = metadata.height;
                         // Ensure binary data length matches expected RGBA size
                         if (packet.content.binary_bytes.length === width * height * 4) {
                             const clampedArray = new Uint8ClampedArray(packet.content.binary_bytes);
                             const imgData = new ImageData(clampedArray, width, height);
                             setReceivedImageData(imgData); // Update state
                             contentPreview += ` (Processed as ${width}x${height} RGBA Image)`;
                             isImagePacket = true;
                         } else {
                              contentPreview += ` (IM format=raw_rgba, but size mismatch: ${packet.content.binary_bytes.length} vs expected ${width * height * 4})`;
                         }
                     } else {
                          contentPreview += ` (IM packet, but unsupported format "${metadata.format}" or invalid dimensions)`;
                     }
                 } catch (e:any) {
                     console.error("Error processing IM packet:", e);
                     contentPreview += ` (Error parsing IM metadata: ${e.message})`;
                 }
             }
             // --- End Handle "IM" packets ---

             // Log to console as well for easier inspection
             console.log(`[App] Received Packet Content: ${contentPreview}`);

             setMessages(prev => [...prev, ` < ${contentPreview}`]);
         });
         setMessages(prev => [...prev, `--- Request Complete ---`]);
     };

    // Effect to draw the image onto the canvas when it changes
    useEffect(() => {
        if (receivedImageData && canvasRef.current) {
            const canvas = canvasRef.current;
            const ctx = canvas.getContext('2d');
            if (ctx) {
                // Resize canvas to fit image
                canvas.width = receivedImageData.width;
                canvas.height = receivedImageData.height;
                // Draw the image data
                ctx.putImageData(receivedImageData, 0, 0);
                console.log(`Drew ${receivedImageData.width}x${receivedImageData.height} image to canvas.`);
                setMessages(prev => [...prev, `[Display] Rendered ${receivedImageData.width}x${receivedImageData.height} image on canvas.`]);
            }
        }
    }, [receivedImageData]); // Dependency array ensures this runs when receivedImageData changes


     const triggerNativeCallback = () => {
        if (!nativeAddon) {
             setMessages(prev => [...prev, `Error: Native addon not available.`]);
             return;
        }
        nativeAddon.triggerTestCallback();
    };
    // --- End Plugin ---


    return (
        <div className="container">
            {/* Top Controls: Target ID, Send Count, Message Input, Send Button */}
             <div className="send-controls">
                 <label>Target ID:</label>
                 <select value={bpgTargetId} onChange={(e) => setBpgTargetId(Number(e.target.value))}>
                     {[1, 2, 50, 55].map(id => <option key={id} value={id}>{id}</option>)}
                 </select>
                 <label>Send Count:</label>
                 <select
                    value={speedTest_sendCount}
                    onChange={(e) => setSpeedTest_sendCount(Number(e.target.value))}
                    disabled={isSending} // Disable during send
                 >
                     {[1, 10, 100, 1000, 10000].map((count) => (
                         <option key={count} value={count}>{count}</option>
                     ))}
                 </select>
                 X
                 <input
                    type="text"
                    id="messageInput"
                    placeholder="Enter message for BPG TX packet"
                    onKeyPress={(e) => e.key === 'Enter' && handleSendMessage()}
                    disabled={isSending} // Disable during send
                 />
                 <button onClick={handleSendMessage} disabled={!isInitialized || isSending}>
                    {isSending ? 'Sending...' : 'Send BPG Group'}
                 </button>
             </div>

             {/* Status Line */}
             <div className="queue-status">{isInitialized ? queueStatus : 'Initializing...'} - {speedTestStatus}</div>

             {/* Log/Plugin Controls */}
            <div className="controls">
                <button onClick={() => setMessages([])} disabled={isSending}>Clear Log</button>
                <button onClick={triggerNativeCallback} disabled={isSending}>Trigger Native Callback</button>
            </div>
            <div className="plugin-controls">
                <button onClick={loadPlugin} disabled={isSending || !appMode}>Load Plugin</button>
                <button onClick={unloadPlugin} disabled={isSending}>Unload Plugin</button>
                <span className="plugin-status">{pluginStatus}</span>
            </div>

            {/* Message Log Area */}
            <div className="message-log">
                {messages.map((message, index) => (
                    <div key={index} className="message">
                        {message}
                    </div>
                ))}
            </div>

            {/* Canvas Area */}
            <div className="canvas-container">
                <h3>Received Image</h3>
                <canvas ref={canvasRef} style={{ border: '1px solid #ccc', maxWidth: '100%' }}></canvas>
            </div>
        </div>
    );
}

export default App; 