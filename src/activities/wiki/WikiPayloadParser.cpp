#include "WikiPayloadParser.h"
#include <vector>
#include <algorithm>
#include <cstring>
#include <string_view>
#include <Logging.h>

static const char* TAG = "WikiPayload";

std::string WikiPayloadParser::extractPayload(WikiDecompressor& decompressor, const std::string& marker) {
    std::string startMarker = "===" + marker + "===";
    std::string payload;
    LOG_INF(TAG, "extractPayload searching for '%s'", startMarker.c_str());
    
    // Stack-only search buffer: ZERO heap allocations while scanning through articles
    const size_t CHUNK_SIZE = 1024;
    const size_t MAX_PAYLOAD = 16000; // 16KB safety limit for internal SRAM
    char buf[CHUNK_SIZE + 64];
    size_t overlapLen = 0;
    bool capturing = false;
    size_t totalBytesRead = 0;

    auto trimTruncatedPayload = [](std::string& s) {
        size_t lastNl = s.find_last_of('\n');
        if (lastNl != std::string::npos && lastNl > 0) {
            s.erase(lastNl);
        }
    };
    
    while (!decompressor.isDone()) {
        size_t n = decompressor.read(buf + overlapLen, CHUNK_SIZE);
        if (n == 0) {
            break;
        }
        totalBytesRead += n;
        size_t totalLen = overlapLen + n;
        
        if (!capturing) {
            std::string_view view(buf, totalLen);
            size_t pos = view.find(startMarker);
            if (pos != std::string_view::npos) {
                LOG_INF(TAG, "Found startMarker '%s' at offset %zu (totalRead=%zu)", startMarker.c_str(), pos, totalBytesRead);
                capturing = true;
                size_t payloadStart = pos + startMarker.length();
                if (payloadStart < totalLen && buf[payloadStart] == '\r') payloadStart++;
                if (payloadStart < totalLen && buf[payloadStart] == '\n') payloadStart++;
                
                // Search remainder of current chunk for end marker "==="
                std::string_view rest(buf + payloadStart, totalLen - payloadStart);
                size_t endPos = rest.find("===");
                if (endPos != std::string_view::npos) {
                    payload.reserve(endPos);
                    payload.append(buf + payloadStart, endPos);
                    LOG_INF(TAG, "Found endMarker within chunk, payload len=%zu", payload.length());
                    return payload;
                }
                
                // Pre-reserve MAX_PAYLOAD in a single allocation while payload is still empty
                // so std::string never doubles capacity from 16KB -> 32KB (48KB simultaneous).
                payload.reserve(MAX_PAYLOAD);
                
                // Append safe portion (keeping last 2 bytes for "===" boundary split)
                size_t avail = totalLen - payloadStart;
                if (avail > 2) {
                    size_t safeLen = avail - 2;
                    payload.append(buf + payloadStart, safeLen);
                    overlapLen = 2;
                    memmove(buf, buf + payloadStart + safeLen, overlapLen);
                } else {
                    overlapLen = avail;
                    memmove(buf, buf + payloadStart, overlapLen);
                }
            } else {
                // Keep the tail in case marker spans across chunks
                overlapLen = std::min(totalLen, startMarker.length() - 1);
                if (overlapLen > 0) {
                    memmove(buf, buf + totalLen - overlapLen, overlapLen);
                }
            }
        } else {
            // Capturing payload
            std::string_view view(buf, totalLen);
            size_t endPos = view.find("===");
            if (endPos != std::string_view::npos) {
                size_t toAppend = endPos;
                bool truncated = false;
                if (payload.length() + toAppend > MAX_PAYLOAD) {
                    toAppend = MAX_PAYLOAD - payload.length();
                    truncated = true;
                }
                payload.append(buf, toAppend);
                if (truncated) {
                    trimTruncatedPayload(payload);
                }
                LOG_INF(TAG, "Found endMarker '===', captured payload len=%zu", payload.length());
                return payload;
            } else {
                size_t safeLen = (totalLen > 2) ? (totalLen - 2) : 0;
                size_t toAppend = safeLen;
                if (payload.length() + toAppend > MAX_PAYLOAD) {
                    toAppend = (payload.length() < MAX_PAYLOAD) ? (MAX_PAYLOAD - payload.length()) : 0;
                    if (toAppend > 0) payload.append(buf, toAppend);
                    trimTruncatedPayload(payload);
                    LOG_ERR(TAG, "Payload exceeded MAX_PAYLOAD (%zu), truncating safely to %zu", MAX_PAYLOAD, payload.length());
                    return payload;
                }
                payload.append(buf, toAppend);
                overlapLen = totalLen - safeLen;
                if (overlapLen > 0) {
                    memmove(buf, buf + safeLen, overlapLen);
                }
            }
        }
    }
    
    if (capturing) {
        if (overlapLen > 0 && payload.length() + overlapLen <= MAX_PAYLOAD) {
            payload.append(buf, overlapLen);
        }
        LOG_INF(TAG, "EOF reached while capturing, total payload length=%zu", payload.length());
    } else {
        LOG_ERR(TAG, "Marker '%s' NOT found! Total bytes read: %zu", startMarker.c_str(), totalBytesRead);
    }
    
    return payload;
}

bool WikiPayloadParser::findPayloadRange(WikiDecompressor& decompressor, const std::string& marker,
                                         uint32_t& outStartOffset, uint32_t& outLength) {
    outStartOffset = 0;
    outLength = 0;
    char startMarkerBuf[64];
    int markerLen = std::snprintf(startMarkerBuf, sizeof(startMarkerBuf), "===%s===", marker.c_str());
    if (markerLen <= 0 || static_cast<size_t>(markerLen) >= sizeof(startMarkerBuf)) {
        return false;
    }
    std::string_view startMarker(startMarkerBuf, static_cast<size_t>(markerLen));

    constexpr size_t CHUNK_SIZE = 1024;
    char buf[CHUNK_SIZE + 64];
    size_t overlapLen = 0;
    uint32_t bufStreamStart = 0;
    bool capturing = false;

    while (!decompressor.isDone()) {
        size_t n = decompressor.read(buf + overlapLen, CHUNK_SIZE);
        if (n == 0) {
            break;
        }
        size_t totalLen = overlapLen + n;

        if (!capturing) {
            std::string_view view(buf, totalLen);
            size_t pos = view.find(startMarker);
            if (pos != std::string_view::npos) {
                if (pos + startMarker.length() + 2 > totalLen && !decompressor.isDone()) {
                    overlapLen = totalLen - pos;
                    std::memmove(buf, buf + pos, overlapLen);
                    bufStreamStart += static_cast<uint32_t>(pos);
                    continue;
                }
                capturing = true;
                size_t payloadStart = pos + startMarker.length();
                if (payloadStart < totalLen && buf[payloadStart] == '\r') payloadStart++;
                if (payloadStart < totalLen && buf[payloadStart] == '\n') payloadStart++;
                outStartOffset = bufStreamStart + static_cast<uint32_t>(payloadStart);

                std::string_view rest(buf + payloadStart, totalLen - payloadStart);
                size_t endPos = rest.find("===");
                if (endPos != std::string_view::npos) {
                    outLength = static_cast<uint32_t>(endPos);
                    LOG_INF(TAG, "findPayloadRange '%s': offset=%u len=%u", startMarkerBuf, outStartOffset, outLength);
                    return outLength > 0;
                }

                size_t rem = totalLen - payloadStart;
                overlapLen = std::min<size_t>(rem, 2);
                size_t drop = totalLen - overlapLen;
                if (overlapLen > 0) {
                    std::memmove(buf, buf + drop, overlapLen);
                }
                bufStreamStart += static_cast<uint32_t>(drop);
            } else {
                overlapLen = std::min<size_t>(totalLen, startMarker.length() + 2);
                size_t drop = totalLen - overlapLen;
                if (overlapLen > 0) {
                    std::memmove(buf, buf + drop, overlapLen);
                }
                bufStreamStart += static_cast<uint32_t>(drop);
            }
        } else {
            std::string_view view(buf, totalLen);
            size_t endPos = view.find("===");
            if (endPos != std::string_view::npos) {
                uint32_t endStreamOffset = bufStreamStart + static_cast<uint32_t>(endPos);
                outLength = (endStreamOffset > outStartOffset) ? (endStreamOffset - outStartOffset) : 0;
                LOG_INF(TAG, "findPayloadRange '%s': offset=%u len=%u", startMarkerBuf, outStartOffset, outLength);
                return outLength > 0;
            }
            overlapLen = std::min<size_t>(totalLen, 2);
            size_t drop = totalLen - overlapLen;
            if (overlapLen > 0) {
                std::memmove(buf, buf + drop, overlapLen);
            }
            bufStreamStart += static_cast<uint32_t>(drop);
        }
    }

    if (capturing) {
        uint32_t endStreamOffset = bufStreamStart + static_cast<uint32_t>(overlapLen);
        outLength = (endStreamOffset > outStartOffset) ? (endStreamOffset - outStartOffset) : 0;
        LOG_INF(TAG, "findPayloadRange '%s' (EOF): offset=%u len=%u", startMarkerBuf, outStartOffset, outLength);
        return outLength > 0;
    }
    return false;
}
