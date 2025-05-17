import { Throttle } from './throttle';
import { nativeAddon } from './nativeAddon';

// Constants for control array indices
const R2N_SIGNAL = 0;
const R2N_LENGTH = 1;
const N2R_SIGNAL = 2;
const N2R_LENGTH = 3;

// Constants for lock states
const UNLOCKED = 0;
const LOCKED = 1;

// --- Debugging ---
// Set this flag to true to enable detailed logging
const DEBUG_ENABLED = false; 
// ---

export class SharedMemoryChannel {
    private RENDERER_TO_NATIVE_SIZE: number;
    private NATIVE_TO_RENDERER_SIZE: number;
    private sharedBuffer: ArrayBuffer | null;
    private control: Int32Array | null;
    private dataR2N: Uint8Array | null;
    private dataN2R: Uint8Array | null;
    
    // --- Send Queue State ---
    private isProcessingSendQueue: boolean; // Tracks if the _processSendQueue loop is active
    public messageQueue: Uint8Array[]; // Queue for messages sent via send()
    public onMessageQueueEmptyCallback: (() => void) | null;
    public queueUpdateThrottle: Throttle;
    private binded_processSendQueue: () => void;

    // --- Receive State ---
    private isReceiving: boolean; // Tracks if the _processReceiveQueue loop is active
    private onMessageCallback: ((rawData: Uint8Array) => void) | null;
    private recv_fast_check_interval: number;
    private recv_slow_check_interval: number;
    private binded_processReceiveQueue: () => void;

    // --- Synchronization ---
    // Mutex to ensure only one operation (send_direct or _processSendQueue)
    // accesses the Renderer-to-Native buffer (dataR2N, control[0], control[1]) at a time.
    private r2nMutex: Promise<void> = Promise.resolve();

    /**
     * Conditional debug logging function.
     * Only logs if DEBUG_ENABLED is true.
     */
    private DBG(...args: any[]) {
        if (DEBUG_ENABLED) {
            console.log('[SMC_DBG]', ...args);
        }
    }

    constructor(rendererToNativeSize = 1024, nativeToRendererSize = 1024) {
        this.DBG("Constructor called");
        this.RENDERER_TO_NATIVE_SIZE = rendererToNativeSize;
        this.NATIVE_TO_RENDERER_SIZE = nativeToRendererSize;
        
        this.messageQueue = [];
        this.isProcessingSendQueue = false;
        this.onMessageQueueEmptyCallback = null;
        this.queueUpdateThrottle = new Throttle(100); // Consider if still needed/how it's used

        this.isReceiving = false;
        this.onMessageCallback = null;
        this.recv_fast_check_interval = 1; // ms - Consider slightly increasing if CPU usage is high
        this.recv_slow_check_interval = 10; // ms - Consider slightly increasing

        this.sharedBuffer = null;
        this.control = null;
        this.dataR2N = null;
        this.dataN2R = null;
        this.initialize();

        this.binded_processSendQueue = this._processSendQueue.bind(this);
        this.binded_processReceiveQueue = this._processReceiveQueue.bind(this);
        this.DBG("Constructor finished");
    }

    private initialize() {
        this.DBG("Initializing...");
        // Create shared buffer with space for both directions plus control array (16 bytes)
        const controlSizeBytes = 4 * Int32Array.BYTES_PER_ELEMENT; // 4 control integers
        this.sharedBuffer = new ArrayBuffer(controlSizeBytes + this.RENDERER_TO_NATIVE_SIZE + this.NATIVE_TO_RENDERER_SIZE);

        // Initialize the addon with the shared buffer and sizes
        // Make sure the native addon expects the control block at the start
        nativeAddon.setSharedBuffer(this.sharedBuffer, this.RENDERER_TO_NATIVE_SIZE, this.NATIVE_TO_RENDERER_SIZE);

        // Set up a potential native callback (example, adapt as needed)
        // nativeAddon.setMessageCallback((buffer: ArrayBuffer) => {
        //     console.log('Native callback received buffer:', buffer);
        // });

        // Control array layout (16 bytes total):
        // [0] R2N_SIGNAL: Renderer -> Native Signal (0: Ready for data, 1: Data ready)
        // [1] R2N_LENGTH: Renderer -> Native Data Length
        // [2] N2R_SIGNAL: Native -> Renderer Signal (0: Ready for data, 1: Data ready)
        // [3] N2R_LENGTH: Native -> Renderer Data Length
        this.control = new Int32Array(this.sharedBuffer, 0, controlSizeBytes / Int32Array.BYTES_PER_ELEMENT);
        Atomics.store(this.control, R2N_SIGNAL, 0); // Initially ready for R->N data
        Atomics.store(this.control, N2R_SIGNAL, 0); // Initially ready for N->R data (Renderer side)

        // Create views for the data regions
        this.dataR2N = new Uint8Array(this.sharedBuffer, controlSizeBytes, this.RENDERER_TO_NATIVE_SIZE);
        this.dataN2R = new Uint8Array(this.sharedBuffer, controlSizeBytes + this.RENDERER_TO_NATIVE_SIZE, this.NATIVE_TO_RENDERER_SIZE);
        this.DBG("Initialization complete.");
    }

    // --- Synchronization Helper ---

    /**
     * Acquires exclusive access to the Renderer-to-Native channel.
     * Returns a function that must be called to release the lock.
     */
    private async acquireR2NLock(): Promise<() => void> {
        const currentMutex = this.r2nMutex;
        let release: () => void;
        // Create the next link in the mutex chain. The promise resolves
        // when the 'release' function is called by the holder of the lock.
        this.r2nMutex = new Promise<void>(resolve => {
            release = resolve;
        });
        // Wait for the previous operation to complete and release its lock.
        await currentMutex;
        // Return the function to release the lock for the current operation.
        return release!;
    }

    // --- Sending Logic ---

    /**
     * Sends a message directly over the shared memory, bypassing the queue.
     * Waits for the native side to be ready before sending.
     * Ensures atomicity with queued messages using a mutex.
     * @param messageBytes The message data.
     * @param wait_ms Maximum time to wait for the native side to become ready.
     * @throws Error if the message is too long.
     * @throws Error if the native side doesn't become ready within wait_ms.
     */
    public async send_direct(messageBytes: Uint8Array, wait_ms: number = 1000): Promise<void> {
        this.DBG(`send_direct called with ${messageBytes.length} bytes, wait_ms=${wait_ms}`);
        if (!this.sharedBuffer || !this.control || !this.dataR2N) {
            throw new Error("SharedMemoryChannel not initialized or already cleaned up.");
        }
        if (messageBytes.length > this.RENDERER_TO_NATIVE_SIZE) {
            throw new Error(`Message too long (${messageBytes.length} bytes). Maximum size is ${this.RENDERER_TO_NATIVE_SIZE} bytes.`);
        }

        this.DBG("send_direct: Attempting to acquire R2N Lock...");
        const releaseLock = await this.acquireR2NLock();
        this.DBG("send_direct: Acquired R2N Lock");

        try {
            // Wait for the native side to signal it's ready (R2N_SIGNAL == 0)
            const startTime = Date.now();
            // Consider using Atomics.waitAsync in the future if available/suitable
            // For now, use polling with a small delay to yield the event loop
            while (Atomics.load(this.control, R2N_SIGNAL) !== 0) {
                if (Date.now() - startTime > wait_ms) {
                     console.error("send_direct timeout: Native side did not become ready.");
                    throw new Error(`send_direct timeout: Native side not ready after ${wait_ms}ms.`);
                }
                 this.DBG("send_direct: Waiting for Native Ready (Signal is 1)...");
                // Yield event loop briefly, avoid tight loop. Increase delay if CPU usage is high.
                await new Promise(resolve => setTimeout(resolve, 1)); // 1ms delay
            }

            this.DBG("send_direct: Native Ready. Sending message.");

            // Native side is ready, write the data
            this.dataR2N.set(messageBytes, 0);
            this.control[R2N_LENGTH] = messageBytes.length; // Non-atomic write okay after check & lock

            // Signal native side that data is ready
            this.DBG(`send_direct: Setting R2N_SIGNAL to 1 (Length: ${messageBytes.length})`);
            Atomics.store(this.control, R2N_SIGNAL, 1);
            Atomics.notify(this.control, R2N_SIGNAL); // Notify waiters if native uses Atomics.wait

        } finally {
            this.DBG("send_direct: Releasing R2N Lock");
            releaseLock(); // Ensure the lock is always released
        }
         this.DBG("send_direct finished");
    }

    /**
     * Queues a message to be sent asynchronously.
     * @param messageBytes The message data.
     * @throws Error if the message is too long.
     */
    public send(messageBytes: Uint8Array) {
        this.DBG(`send: Queuing message of ${messageBytes.length} bytes.`);
        if (!this.sharedBuffer || !this.control || !this.dataR2N) {
             console.error("send: Attempted to send but channel not initialized or cleaned up.");
             // Optionally throw an error, or just return
            return;
             // throw new Error("SharedMemoryChannel not initialized or already cleaned up.");
        }
        if (messageBytes.length > this.RENDERER_TO_NATIVE_SIZE) {
            // It's better to throw here to signal the error to the caller
            throw new Error(`Message too long (${messageBytes.length} bytes) for queue. Maximum size is ${this.RENDERER_TO_NATIVE_SIZE} bytes.`);
        }

        this.messageQueue.push(messageBytes);
        this.DBG(`send: Queue size now ${this.messageQueue.length}`);

        // If the queue processing loop isn't running, start it.
        if (!this.isProcessingSendQueue) {
            this.isProcessingSendQueue = true;
            this.DBG("send: Kicking off _processSendQueue");
            // Use setTimeout to avoid blocking the current call stack
            setTimeout(this.binded_processSendQueue, 0);
        }
    }

    /**
     * Internal method to process the message queue.
     * Runs asynchronously via setTimeout.
     * Acquires lock before accessing shared R->N buffer.
     */
    private async _processSendQueue() {
        this.DBG("_processSendQueue: Tick");
        if (!this.sharedBuffer || !this.control || !this.dataR2N) {
            console.warn("_processSendQueue: Aborting, channel not initialized.");
            this.isProcessingSendQueue = false;
            return;
        }

        if (this.messageQueue.length === 0) {
            this.DBG("_processSendQueue: Queue empty, stopping loop.");
            this.isProcessingSendQueue = false;
            if (this.onMessageQueueEmptyCallback) {
                 this.DBG("_processSendQueue: Calling onMessageQueueEmptyCallback");
                 try { this.onMessageQueueEmptyCallback(); } catch (e) { console.error("Error in onMessageQueueEmptyCallback:", e); }
            }
            return;
        }
        this.DBG(`_processSendQueue: Queue length: ${this.messageQueue.length}`);

        this.DBG("_processSendQueue: Attempting to acquire R2N Lock...");
        const releaseLock = await this.acquireR2NLock();
         this.DBG("_processSendQueue: Acquired R2N Lock");

        let processedMessage = false;
        try {
            // Check if the native side is ready AFTER acquiring the lock
            if (Atomics.load(this.control, R2N_SIGNAL) === 0) {
                this.DBG("_processSendQueue: Native Ready. Processing message from queue.");
                // Native side is ready, process one message (or potentially stack them)

                // --- Simple: Send one message at a time ---
                const messageToSend = this.messageQueue[0];
                this.dataR2N.set(messageToSend, 0);
                this.control[R2N_LENGTH] = messageToSend.length; // Non-atomic write okay after check & lock
                this.messageQueue.shift(); // Remove message from queue
                this.DBG(`_processSendQueue: Dequeued message, ${this.messageQueue.length} remaining.`);

                 // --- Optional: Stacking multiple messages (if needed) ---
                 /*
                 let packOffset = 0;
                 let messagesToSendCount = 0;
                 for (let i = 0; i < this.messageQueue.length; i++) {
                     const msg = this.messageQueue[i];
                     if (packOffset + msg.length > this.RENDERER_TO_NATIVE_SIZE) {
                         break; // Not enough space for the next message
                     }
                     this.dataR2N.set(msg, packOffset);
                     packOffset += msg.length;
                     messagesToSendCount++;
                 }
                 if (messagesToSendCount > 0) {
                     this.control[R2N_LENGTH] = packOffset;
                     this.messageQueue.splice(0, messagesToSendCount); // Remove sent messages
                     processedMessage = true;
                     this.DBG(`_processSendQueue: Sent ${messagesToSendCount} stacked messages, total length ${packOffset}`);
                 } else {
                    console.log("_processSendQueue: Native Ready, but first message too large? Skipping.");
                 }
                 */
                 // -----------------------------------------------------

                // Signal native side that data is ready
                this.DBG(`_processSendQueue: Setting R2N_SIGNAL to 1 (Length: ${this.control[R2N_LENGTH]})`);
                Atomics.store(this.control, R2N_SIGNAL, 1);
                 Atomics.notify(this.control, R2N_SIGNAL); // Notify waiters

                processedMessage = true;

            } else {
                 this.DBG("_processSendQueue: Native not ready (Signal is 1). Will retry later.");
                // Native side not ready, do nothing this cycle.
                processedMessage = false;
            }
        } catch (e) {
              console.error("_processSendQueue: Error during processing:", e);
              // Decide how to handle errors, e.g., stop processing or just log
        } finally {
             this.DBG("_processSendQueue: Releasing R2N Lock");
            releaseLock(); // Ensure the lock is always released
        }

        // Regardless of whether a message was sent, schedule the next check
        // if the loop should continue.
        if (this.isProcessingSendQueue) {
             this.DBG("_processSendQueue: Scheduling next check.");
            // Use setTimeout to yield and prevent stack overflow
             // Adjust delay based on whether we processed a message or are waiting
             const delay = processedMessage ? 0 : 5; // Small delay if waiting for native
             this.DBG(`_processSendQueue: Next check delay: ${delay}ms`);
            setTimeout(this.binded_processSendQueue, delay);
        } else {
            this.DBG("_processSendQueue: Loop externally stopped.");
        }
    }

    // --- Receiving Logic ---

    /**
     * Starts the loop to check for messages from the native side.
     * @param callback Function to call with received message data.
     */
    public startReceiving(callback: (message: Uint8Array) => void) {
        this.DBG("startReceiving called");
        if (!this.sharedBuffer || !this.control || !this.dataN2R) {
             console.error("startReceiving: Attempted to start but channel not initialized or cleaned up.");
            return;
             // throw new Error("SharedMemoryChannel not initialized or already cleaned up.");
        }
        if (this.isReceiving) {
             console.warn("startReceiving: Already receiving.");
            return;
        }
         this.DBG("startReceiving: Starting receive loop.");
        this.onMessageCallback = (rawData: Uint8Array) => {
            this.DBG(`Received message, calling user callback with ${rawData.length} bytes.`);
             try { callback(rawData); } catch(e) { console.error("Error in onMessageCallback:", e); }
         };
        this.isReceiving = true;
        // Use setTimeout to start the loop asynchronously
        setTimeout(this.binded_processReceiveQueue, this.recv_slow_check_interval);
    }

    /**
     * Stops the loop that checks for messages from the native side.
     */
    public stopReceiving() {
         this.DBG("stopReceiving: Stopping receive loop.");
        this.isReceiving = false;
        // No need to clear onMessageCallback immediately, _processReceiveQueue checks isReceiving
    }

    /**
     * Internal method to check for and process messages from the native side.
     * Runs asynchronously via setTimeout.
     */
    private _processReceiveQueue() {
        // This logs frequently, keep it concise or remove if too noisy even when DEBUG_ENABLED=true
        // this.DBG("_processReceiveQueue: Tick"); 
        if (!this.isReceiving) {
             this.DBG("_processReceiveQueue: Loop stopped.");
            return; // Stop the loop if isReceiving is false
        }
        if (!this.sharedBuffer || !this.control || !this.dataN2R) {
             console.warn("_processReceiveQueue: Aborting, channel not initialized.");
            this.isReceiving = false; // Stop if channel disappears
            return;
        }

        let nextCheckInterval = this.recv_slow_check_interval;

        // Check if native side has sent a message (N2R_SIGNAL == 1)
        // Use Atomics.load for thread safety
        if (Atomics.load(this.control, N2R_SIGNAL) === 1) {
             this.DBG("_processReceiveQueue: Received N2R_SIGNAL=1.");
            // Atomically read length AND signal that we are processing
            // This prevents native side from overwriting while we read
            // (Optional: If native side waits for signal 0 before writing again)
            const length = Atomics.load(this.control, N2R_LENGTH);

            if (length > 0 && length <= this.NATIVE_TO_RENDERER_SIZE) {
                 this.DBG(`_processReceiveQueue: Processing message of length ${length}.`);
                // Read the data - slice creates a copy
                const dataCopy = this.dataN2R.slice(0, length);

                // Signal native side that we have finished processing the message
                 this.DBG("_processReceiveQueue: Setting N2R_SIGNAL to 0.");
                Atomics.store(this.control, N2R_SIGNAL, 0);
                 Atomics.notify(this.control, N2R_SIGNAL); // Notify native if it's waiting

                // Process the received data
                if (this.onMessageCallback) {
                    // Run callback *after* signaling native, allows native to prepare next message sooner
                    this.onMessageCallback(dataCopy);
                }
                 // Check faster next time as we just received a message
                nextCheckInterval = this.recv_fast_check_interval;
            } else if (length > this.NATIVE_TO_RENDERER_SIZE) {
                 console.error(`_processReceiveQueue: Received message length ${length} exceeds buffer size ${this.NATIVE_TO_RENDERER_SIZE}. Data lost.`);
                 // Signal native side we are done, even though data was bad
                 this.DBG("_processReceiveQueue: Setting N2R_SIGNAL to 0 after oversized message.");
                 Atomics.store(this.control, N2R_SIGNAL, 0);
                 Atomics.notify(this.control, N2R_SIGNAL);
            } else {
                 // Length is 0 or negative, likely indicates an issue or just an empty signal
                 console.warn(`_processReceiveQueue: Received signal but length is ${length}. Ignoring.`);
                 // Still need to signal we are done processing this invalid state
                 this.DBG(`_processReceiveQueue: Setting N2R_SIGNAL to 0 after invalid length ${length}.`);
                 Atomics.store(this.control, N2R_SIGNAL, 0);
                 Atomics.notify(this.control, N2R_SIGNAL);
            }
        } else {
             // No message waiting, use the slow interval
             nextCheckInterval = this.recv_slow_check_interval;
        }

        // Schedule the next check if still receiving
        if (this.isReceiving) {
            // this.DBG(`_processReceiveQueue: Scheduling next check in ${nextCheckInterval}ms`);
            setTimeout(this.binded_processReceiveQueue, nextCheckInterval);
        }
    }

    // --- Cleanup ---

    /**
     * Cleans up resources, stops loops, and clears callbacks.
     * Should be called when the channel is no longer needed.
     */
    public cleanup() {
        this.DBG("SharedMemoryChannel: Cleaning up...");
        this.stopReceiving();
        this.isProcessingSendQueue = false; // Stop send queue processing

        // Cancel any pending throttle actions
        if (this.queueUpdateThrottle) {
            this.queueUpdateThrottle.cancel();
        }

        // Clear queues and callbacks
        this.messageQueue = [];
        this.onMessageCallback = null;
        this.onMessageQueueEmptyCallback = null;

        // Break mutex chain to prevent future waits from blocking indefinitely
        // Resolve the current mutex promise to allow any pending waits to finish (or fail)
        // then reset it to a resolved promise so future calls fail fast.
        this.DBG("SharedMemoryChannel: Resetting R2N mutex.");
        let releaseCurrent: () => void;
        this.r2nMutex.then(()=>{}); // Ensure it settles
        this.r2nMutex = new Promise<void>(resolve => { releaseCurrent = resolve; });
        if(releaseCurrent!) releaseCurrent!(); // Release any current waiters immediately
        this.r2nMutex = Promise.resolve(); // Reset for future calls (which should ideally not happen after cleanup)

        // Call native cleanup if necessary
        // Ensure nativeAddon.cleanup() handles potential null buffer gracefully
        // or check here
        if (this.sharedBuffer) {
             this.DBG("SharedMemoryChannel: Calling nativeAddon.cleanup().");
             try {
                 nativeAddon.cleanup(); // Assuming this exists and handles its state
             } catch (e) {
                 console.error("Error during nativeAddon.cleanup():", e);
             }
        }

        // Nullify references to allow garbage collection
        this.sharedBuffer = null;
        this.control = null;
        this.dataR2N = null;
        this.dataN2R = null;
         this.DBG("SharedMemoryChannel: Cleanup complete.");
    }
} 