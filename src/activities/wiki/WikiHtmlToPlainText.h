#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct ExtractedLink {
    uint32_t startCharIndex;
    uint32_t endCharIndex;
    uint32_t hrefOffset;
    uint16_t hrefLen;
};

struct TOCEntry {
    uint8_t level; // 2 for <h2>, 3 for <h3>
    std::string title;
    uint32_t charIdx; // The plain-text character index where this heading starts
};

struct WikiMetrics {
    size_t exactTextSize = 0;
    size_t exactHrefSize = 0;
    size_t exactLinkCount = 0;
};

class WikiDecompressor;

WikiMetrics measureStreamHtmlToPlainText(WikiDecompressor& decompressor, size_t maxBytes);
#include <atomic>
std::string streamHtmlToPlainTextChunk(WikiDecompressor& decompressor, size_t maxBytes, size_t startCharIdx, size_t maxChars, std::vector<ExtractedLink>* outLinks, std::string* outHrefPool, bool& isEof, std::atomic<bool>* cancelToken = nullptr);
std::vector<TOCEntry> extractArticleTOC(WikiDecompressor& decompressor, size_t maxBytes);
std::string streamHtmlToPlainText(WikiDecompressor& decompressor, size_t maxBytes, size_t exactSize, std::vector<ExtractedLink>* outLinks = nullptr, std::string* outHrefPool = nullptr);
