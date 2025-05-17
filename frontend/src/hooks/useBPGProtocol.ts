import { useState, useEffect, useRef, useCallback } from 'react';
import { SharedMemoryChannel } from '../lib/SharedMemoryChannel';
import {
    BpgEncoder, BpgDecoder, AppPacket, AppPacketGroup,
    HybridData, PacketCallback, GroupCallback
} from '../lib/BPG_Protocol';

// Input options for the hook
export interface UseBPGProtocolOptions {
    tx_size: number;
    rx_size: number;
    unhandled_group?: UnsolicitedGroupCallback; // Optional callback for groups not matching requests
}

// Simplified descriptor for packets to send
export interface BPGPacketDescriptor {
    tl: string;
    str?: string;
    bin?: Uint8Array;
}

// Callback type for unsolicited groups
export type UnsolicitedGroupCallback = (group: AppPacketGroup) => void;

// Type for the hook's return value
export interface UseBPGProtocolReturn {
    sendGroup: (groupId: number, targetId: number, packets: BPGPacketDescriptor[], timeout?: number) => Promise<AppPacket[]>;
    channel: SharedMemoryChannel | null; 
    isInitialized: boolean;
}

// Structure to hold pending request promises
export interface PendingRequest {
    resolve: (value: AppPacket[]) => void;
    reject: (reason?: any) => void;
    timeoutId?: NodeJS.Timeout; 
}

const DEFAULT_RESPONSE_TIMEOUT = 5000; // Default timeout

export function useBPGProtocol(options: UseBPGProtocolOptions): UseBPGProtocolReturn {
    const [channel, setChannel] = useState<SharedMemoryChannel | null>(null);
    const [isInitialized, setIsInitialized] = useState<boolean>(false);
    const encoderRef = useRef<BpgEncoder>(new BpgEncoder());
    const decoderRef = useRef<BpgDecoder>(new BpgDecoder());
    // Store pending requests: Key = `${groupId}:${targetId}`
    const pendingRequestsRef = useRef<Map<string, PendingRequest>>(new Map());

    // Internal callback to handle fully decoded groups
    const handleBpgGroup: GroupCallback = useCallback((groupId, group) => {
        if (!group || group.length === 0) return;

        const responseTargetId = group[0].target_id;
        const key = `${groupId}:${responseTargetId}`; 
        
        // 1. Check for pending requests
        const pending = pendingRequestsRef.current.get(key);
        if (pending) {
            console.log(`[useBPGProtocol] Received matching response for ${key}`);
            if (pending.timeoutId) clearTimeout(pending.timeoutId);
            pending.resolve(group); 
            pendingRequestsRef.current.delete(key);
            return; // Handled as a response
        }
        
        // 2. Check for unhandled_group callback provided in options
        if (options.unhandled_group) {
            console.log(`[useBPGProtocol] Passing unhandled group ${key} to provided callback.`);
            try {
                options.unhandled_group(group);
            } catch (e) {
                console.error(`[useBPGProtocol] Error in unhandled_group callback for ${key}:`, e);
            }
            return; // Handled by callback
        }

        // 3. If neither, log as truly unhandled
        console.log(`[useBPGProtocol] Received unhandled group for ${key}. No matching request or callback provided.`);

    }, [options.unhandled_group]); // Add options.unhandled_group to dependencies

    // Internal callback for individual packets (can be used for logging/debugging)
    const handleBpgPacket: PacketCallback = useCallback((packet) => {
        // console.log("[useBPGProtocol] RX Packet:", packet);
    }, []);

    // --- Initialization and Cleanup Effect --- 
    useEffect(() => {
        console.log("[useBPGProtocol] Initializing...");
        const newChannel = new SharedMemoryChannel(options.tx_size, options.rx_size);
        decoderRef.current.reset(); 
        newChannel.startReceiving((rawData: Uint8Array) => {
            try {
                decoderRef.current.processData(rawData, handleBpgPacket, handleBpgGroup);
            } catch (e) {
                console.error("[useBPGProtocol] Error processing BPG data:", e);
            }
        });
        setChannel(newChannel);
        setIsInitialized(true);
        console.log("[useBPGProtocol] Initialized.");
        
        return () => {
            console.log("[useBPGProtocol] Cleaning up...");
            // Reject pending requests
            pendingRequestsRef.current.forEach((pending, key) => {
                 if (pending.timeoutId) clearTimeout(pending.timeoutId);
                 pending.reject(new Error(`BPG request ${key} cancelled due to cleanup.`));
            });
            pendingRequestsRef.current.clear();
            newChannel.cleanup();
            setChannel(null);
            setIsInitialized(false);
             console.log("[useBPGProtocol] Cleaned up.");
        };
    }, [options.tx_size, options.rx_size, options.unhandled_group, handleBpgGroup, handleBpgPacket]);

    // --- sendGroup Function --- (Updated to accept optional timeout)
    const sendGroup = useCallback(async (
        groupId: number,
        targetId: number, 
        packets: BPGPacketDescriptor[],
        timeout: number = DEFAULT_RESPONSE_TIMEOUT // Use default or provided timeout
    ): Promise<AppPacket[]> => {
        if (!channel || !isInitialized) {
            throw new Error("useBPGProtocol: Channel not initialized.");
        }
        if (packets.length === 0) {
            throw new Error("useBPGProtocol: Cannot send an empty packet group.");
        }

        const requestKey = `${groupId}:${targetId}`;

        if (pendingRequestsRef.current.has(requestKey)) {
            throw new Error(`useBPGProtocol: Request already pending for ${requestKey}`);
        }

        return new Promise<AppPacket[]>((resolve, reject) => {
            const timeoutId = setTimeout(() => {
                pendingRequestsRef.current.delete(requestKey);
                reject(new Error(`BPG request ${requestKey} timed out after ${timeout}ms`));
            }, timeout); // Use the timeout parameter

            pendingRequestsRef.current.set(requestKey, { resolve, reject, timeoutId });

            try {
                const appPackets: AppPacket[] = packets.map((desc, index) => ({
                    group_id: groupId,
                    target_id: targetId,
                    tl: desc.tl,
                    is_end_of_group: index === packets.length - 1, 
                    content: { metadata_str: desc.str || "", binary_bytes: desc.bin || new Uint8Array(0) }
                }));

                for (const packet of appPackets) {
                    const encodedPacket = encoderRef.current.encodePacket(packet);
                    channel.send(encodedPacket);
                }
                console.log(`[useBPGProtocol] Sent group ${groupId} to target ${targetId}, expecting response (key: ${requestKey})`);
            } catch (error) {
                 clearTimeout(timeoutId);
                 pendingRequestsRef.current.delete(requestKey);
                 reject(error);
            }
        });
    }, [channel, isInitialized]);

    return { 
        sendGroup, 
        channel, 
        isInitialized, 
    };
}
