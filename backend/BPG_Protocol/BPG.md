# BPG (Binary Packet Group) Protocol Binary Format

This document describes the binary wire format used by the C++ BPG protocol implementation for communication between the application layer (e.g., Web UI) and native plugins via a Link Layer (like Shared Memory, WebSockets, etc.).

## Packet Structure

Each logical message sent over the link layer consists of one or more BPG packets concatenated together. A logical "group" of related packets is identified by a common `group_id`. The **last packet** in a logical group is indicated by a specific bit flag within its header's `prop` field.

Each individual BPG packet follows this structure:

```
+----------------------------------------------------+--------------------------------------------+
|             Packet Header (18 bytes)               |             Packet Data (Variable)         |
+----------------------------------------------------+--------------------------------------------+
| TL:2 | Prop:4 | TargetID:4 | GroupID:4 | DataLen:4 | StrLen:4 | Metadata String | Binary Data   |
+------+--------+------------+-----------+-----------+----------+-----------------+---------------+
```
*Note: `StrLen`, `Metadata String`, and `Binary Data` constitute the Packet Data section.*

## Field Descriptions

### 1. Packet Header (Fixed Size: 18 Bytes)

| Field       | Size (Bytes) | C++ Type      | Description                                     | Network Order |
|-------------|--------------|---------------|-------------------------------------------------|---------------|
| `tl`        | 2            | `char[2]`     | Two-letter ASCII packet type identifier (e.g., "IM", "TX"). | N/A (bytes in order)   |
| `prop`      | 4            | `uint32_t`    | Property bitfield. **See details below.**       | **Big Endian**  |
| `target_id` | 4            | `uint32_t`    | Identifier for the target recipient/context.    | **Big Endian**  |
| `group_id`  | 4            | `uint32_t`    | Identifier for the packet group.                | **Big Endian**  |
| `data_length`| 4           | `uint32_t`    | Total length **in bytes** of the Packet Data section that follows this header. | **Big Endian**  |

#### `prop` Field Details:

The `prop` field is a 4-byte (`uint32_t`) space reserved for flags. Currently, only the **least significant bit (LSB)** is defined:

```
Bit Index: |31...........1|0|
           | Reserved (0) |E|
                           ^ EG Bit
```

*   **EG (End Group) Bit:** `prop & 0x00000001`
    *   If this bit is `1`, this packet is the **last packet** of the logical group identified by `group_id`.
    *   If this bit is `0`, more packets for this `group_id` are expected (or this is a single-packet group where the bit is set).
*   All other bits (1 through 31) are currently **reserved** and **MUST** be set to `0` by the sender.

### 2. Packet Data (Variable Size: `data_length` Bytes)

The format of the data section corresponds to the `HybridData` structure. It always starts with the length of the metadata string, followed by the string data (if any), and finally the raw binary data (if any).

| Field           | Size (Bytes)            | C++ Type (Origin)      | Description                                     | Network Order |
|-----------------|-------------------------|------------------------|-------------------------------------------------|---------------|
| `str_length`    | 4                       | `uint32_t`             | Length **in bytes** of the Metadata String that follows. Can be 0 if no metadata is present. | **Big Endian**  |
| `metadata_str`  | `str_length`            | `std::string`          | Optional UTF-8 encoded string providing metadata about the `binary_data`. Empty if `str_length` is 0. | N/A (bytes)   |
| `binary_bytes`  | `data_length` - 4 - `str_length` | `std::vector<uint8_t>` | The raw binary payload of the packet. Can be empty. | N/A (bytes)   |

## Example: Last Text Packet ("TX") in a Group

Let's say the application sends a text message "Done" as the final packet of Group 301 to Target 11.

*   `tl`: "TX" (0x54, 0x58)
*   `prop`: 1 (EG bit set, other bits 0) -> Network Order: 0x00000001
*   `target_id`: 11 (0x0000000B)
*   `group_id`: 301 (0x0000012D)
*   `metadata_str`: "" (empty) -> `str_length` = 0 (0x00000000)
*   `binary_bytes`: "Done" (0x44, 0x6F, 0x6E, 0x65) -> `binary_bytes_len` = 4
*   `data_length`: 4 (str_length field) + 0 (metadata) + 4 (binary) = 8 (0x00000008)

The resulting byte stream would be:

```
Header:
54 58            (tl = "TX")
00 00 00 01      (prop = 1)
00 00 00 0B      (target_id = 11)
00 00 01 2D      (group_id = 301)
00 00 00 08      (data_length = 8)

Data:
00 00 00 00      (str_length = 0)
44 6F 6E 65      (binary_bytes = "Done")
```

Total Bytes: 18 (Header) + 8 (Data) = 26 bytes.

## Endianness Note

All multi-byte integer fields (`group_id`, `target_id`, `prop`, `data_length`, `str_length`) are encoded and decoded using **Network Byte Order (Big Endian)**. Implementations on different platforms must perform the necessary byte swapping.