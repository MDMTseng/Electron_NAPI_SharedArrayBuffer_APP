export interface PacketHeader {
    group_id: number;
    target_id: number;
    tl: string;
    prop: number; // New 4-byte property field (uint32_t)
    data_length: number;
}

export interface HybridData {
    metadata_str: string; 
    binary_bytes: Uint8Array;
}

export interface AppPacket {
    group_id: number;   
    target_id: number;  
    tl: string;         
    is_end_of_group: boolean; // New flag
    content: HybridData;
}

export type AppPacketGroup = AppPacket[];

export type PacketCallback = (packet: AppPacket) => void;
export type GroupCallback = (groupId: number, group: AppPacketGroup) => void;

// Constants
export const HEADER_SIZE = 18; // Updated Size
const PROP_EG_BIT_MASK = 0x00000001;
const STR_LENGTH_SIZE = 4; // Renamed from JSON_LENGTH_SIZE for clarity

// --- Encoder --- 

export class BpgEncoder {

    private calculateHybridDataSize(data: HybridData): number {
        const strBytes = new TextEncoder().encode(data.metadata_str); 
        return STR_LENGTH_SIZE + strBytes.length + data.binary_bytes.length;
    }

    encodePacket(packet: AppPacket): Uint8Array {
        const dataSize = this.calculateHybridDataSize(packet.content);
        const totalSize = HEADER_SIZE + dataSize;
        let offset = 0;
        const buffer = new ArrayBuffer(totalSize);
        const dataView = new DataView(buffer);
        const packetBytes = new Uint8Array(buffer);

        // --- Header (New Order) --- 
        // TL (2 bytes)
        packetBytes[offset++] = packet.tl.charCodeAt(0);
        packetBytes[offset++] = packet.tl.charCodeAt(1);
        
        // Prop (4 bytes, Big Endian)
        let propValue = 0;
        if (packet.is_end_of_group) {
            propValue |= PROP_EG_BIT_MASK; 
        }
        dataView.setUint32(offset, propValue, false); 
        offset += 4;

        // Target ID (4 bytes, Big Endian)
        dataView.setUint32(offset, packet.target_id, false); 
        offset += 4;

        // Group ID (4 bytes, Big Endian)
        dataView.setUint32(offset, packet.group_id, false); 
        offset += 4;

        // Data Length (4 bytes, Big Endian)
        dataView.setUint32(offset, dataSize, false); 
        offset += 4;
        
        // --- Data (HybridData) ---
        const strBytes = new TextEncoder().encode(packet.content.metadata_str); 
        const strLength = strBytes.length;
        
        // String Length 
        dataView.setUint32(offset, strLength, false); 
        offset += 4;

        // String Bytes
        if (strLength > 0) {
            packetBytes.set(strBytes, offset);
            offset += strLength;
        }

        // Binary Bytes 
        if (packet.content.binary_bytes.length > 0) {
            packetBytes.set(packet.content.binary_bytes, offset);
            offset += packet.content.binary_bytes.length;
        }
        
        if (offset !== totalSize) {
             console.warn(`BPG Encoder: Offset mismatch. Expected ${totalSize}, got ${offset}`);
        }
        return packetBytes;
    }

    // encodePacketGroup remains the same conceptually, just calls the updated encodePacket
    encodePacketGroup(group: AppPacketGroup): Uint8Array {
         let totalSize = 0;
         group.forEach(packet => { totalSize += HEADER_SIZE + this.calculateHybridDataSize(packet.content); });
         
         const combinedBuffer = new Uint8Array(totalSize);
         let currentOffset = 0;
         group.forEach(packet => {
             const encodedPacket = this.encodePacket(packet);
             combinedBuffer.set(encodedPacket, currentOffset);
             currentOffset += encodedPacket.length;
         });
         return combinedBuffer;
    }
}

// --- Decoder --- 

export class BpgDecoder {
    private internal_buffer: Uint8Array = new Uint8Array(0);
    private active_groups: Map<number, AppPacketGroup> = new Map();

    reset(): void {
        this.internal_buffer = new Uint8Array(0);
        this.active_groups.clear();
        console.log("BPG Decoder (TS) reset.");
    }

    processData(
        data: Uint8Array, 
        packetCallback: PacketCallback, 
        groupCallback: GroupCallback
    ): void {
        // Append new data
        const newData = new Uint8Array(this.internal_buffer.length + data.length);
        newData.set(this.internal_buffer, 0);
        newData.set(data, this.internal_buffer.length);
        this.internal_buffer = newData;

        while (true) {
            if (this.internal_buffer.length < HEADER_SIZE) break; // 18 bytes

            const dataView = new DataView(this.internal_buffer.buffer, this.internal_buffer.byteOffset, this.internal_buffer.byteLength);
            
            // Deserialize Header (New Order)
            let offset = 0;
            const tl = String.fromCharCode(this.internal_buffer[offset], this.internal_buffer[offset+1]); offset += 2; // TL (2 bytes)
            const propValue = dataView.getUint32(offset, false); offset += 4;                           // Prop (4 bytes)
            const targetId = dataView.getUint32(offset, false); offset += 4;                            // Target ID (4 bytes)
            const groupId = dataView.getUint32(offset, false); offset += 4;                             // Group ID (4 bytes)
            const dataLength = dataView.getUint32(offset, false); offset += 4;                          // Data Length (4 bytes)
            
            const totalPacketSize = HEADER_SIZE + dataLength;
            if (this.internal_buffer.length < totalPacketSize) break; 

            // Check EG Bit from prop field LSB
            const isEndOfGroup = (propValue & PROP_EG_BIT_MASK) !== 0;

            // --- Deserialize HybridData ---
            const hybridData: HybridData = { metadata_str: "", binary_bytes: new Uint8Array(0) };
            let dataOffset = HEADER_SIZE; 
            
            if(dataLength < STR_LENGTH_SIZE) {
                 console.error(`BPG Decoder: HdrDataLen (${dataLength}) < StrLenSize (${STR_LENGTH_SIZE}). Skipping packet.`);
                 this.internal_buffer = this.internal_buffer.slice(totalPacketSize);
                 continue; 
            }
            
            const strLength = dataView.getUint32(dataOffset, false); dataOffset += STR_LENGTH_SIZE;
            const binaryBytesLength = dataLength - STR_LENGTH_SIZE - strLength;

            if (binaryBytesLength < 0) {
                  console.error(`BPG Decoder: Invalid str length (${strLength}) resulting in negative binary length. Skipping packet.`);
                   this.internal_buffer = this.internal_buffer.slice(totalPacketSize);
                   continue; 
            }

            // Metadata String
            if (strLength > 0) {
                 if(dataOffset + strLength > totalPacketSize) { console.error("Incomplete metadata"); break; }
                const strBytes = this.internal_buffer.slice(dataOffset, dataOffset + strLength);
                hybridData.metadata_str = new TextDecoder().decode(strBytes); 
                dataOffset += strLength;
            }

            // Binary bytes
            if (binaryBytesLength > 0) {
                 if(dataOffset + binaryBytesLength > totalPacketSize) { console.error("Incomplete binary data"); break; }
                hybridData.binary_bytes = this.internal_buffer.slice(dataOffset, dataOffset + binaryBytesLength);
                // dataOffset += binaryBytesLength; // Not strictly needed as it's the last part
            }

            // --- Create AppPacket --- 
            const appPacket: AppPacket = {
                group_id: groupId,
                target_id: targetId,
                tl: tl,
                is_end_of_group: isEndOfGroup, // Set flag
                content: hybridData
            };

            // Store and trigger individual callback
            if (!this.active_groups.has(groupId)) {
                this.active_groups.set(groupId, []);
            }
            this.active_groups.get(groupId)?.push(appPacket);

            try { packetCallback(appPacket); } catch(e) { console.error("[BPG TS ERR] Exception in packetCallback:", e); }
            
            // Trigger group callback if EG bit is set
            if (isEndOfGroup) {
                const completedGroup = this.active_groups.get(groupId);
                if (completedGroup) {
                     try { groupCallback(groupId, completedGroup); } catch(e) { console.error("[BPG TS ERR] Exception in groupCallback:", e); }
                    this.active_groups.delete(groupId); // Clear the completed group
                }
            }
            
            // Consume the processed packet from the buffer
            this.internal_buffer = this.internal_buffer.slice(totalPacketSize);
        }
    }
}
