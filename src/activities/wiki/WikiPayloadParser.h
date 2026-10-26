#pragma once

#include <cstdint>
#include <string>
#include "WikiDecompressor.h"

class WikiPayloadParser {
public:
    // Streams through the entire compressed article to find a specific payload block.
    // A payload block starts with "==={marker}===" and ends with either another "===" or "===END===".
    // Example markers: "INFOBOX", "TABLE:1"
    static std::string extractPayload(WikiDecompressor& decompressor, const std::string& marker);

    // Zero-heap scan to locate the uncompressed byte offset and byte length of a payload block
    // without allocating a payload string in RAM.
    static bool findPayloadRange(WikiDecompressor& decompressor, const std::string& marker,
                                 uint32_t& outStartOffset, uint32_t& outLength);
};
