#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <cstring>
#include "SdRawReader.h"
#include <FreeInkUICore.h>
#include "fontIds.h"

class WikiTextFormatter {
    GfxRenderer& renderer;
    SdRawReader& reader;
    int englishFontId;
    int englishHeaderFontId;
    uint32_t fontSector;
    
    int glyphSize = 24;
    int bytesPerGlyph = 72; // (24 + 7) / 8 * 24
    uint8_t nativeFontPx = 0; // 0 = legacy 26px FBF1, >=29 = native 29px FBF1
    static constexpr int HEADER_SIZE = 16;
public:
    WikiTextFormatter(GfxRenderer& r, SdRawReader& reader_, int engId, int engHeadId, uint32_t fontSector_) 
        : renderer(r), reader(reader_), englishFontId(engId), englishHeaderFontId(engHeadId), fontSector((fontSector_ == UINT32_MAX) ? 0 : fontSector_) {
        
        if (fontSector > 0) {
            uint8_t header[512];
            if (reader.readSectors(fontSector, 1, header)) {
                if (header[0] == 'F' && header[1] == 'B' && header[2] == 'F' && header[3] == '1') {
                    glyphSize = header[4]; // width
                    bytesPerGlyph = header[6] | (header[7] << 8); // little endian
                    nativeFontPx = header[8];
                }
            }
        }
    }

    ~WikiTextFormatter() = default;

    static bool isFullWidthCjk(uint32_t cp) {
        return (cp >= 0x3000 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF) ||
               (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
               (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
               (cp >= 0xFF00 && cp <= 0xFFEF);
    }

    static bool isCombiningCodepoint(uint32_t cp) {
        return (cp >= 0x0300 && cp <= 0x036F) ||
               (cp >= 0x0591 && cp <= 0x05BD) || cp == 0x05BF ||
               (cp >= 0x05C1 && cp <= 0x05C2) || (cp >= 0x05C4 && cp <= 0x05C5) || cp == 0x05C7 ||
               (cp >= 0x0610 && cp <= 0x061A) || (cp >= 0x064B && cp <= 0x065F) || cp == 0x0670 ||
               (cp >= 0x06D6 && cp <= 0x06DC) || (cp >= 0x06DF && cp <= 0x06E4) ||
               (cp >= 0x06E7 && cp <= 0x06E8) || (cp >= 0x06EA && cp <= 0x06ED) ||
               cp == 0x0711 || (cp >= 0x0730 && cp <= 0x074A) ||
               (cp >= 0x07A6 && cp <= 0x07B0) ||
               (cp >= 0x08D3 && cp <= 0x08E1) || (cp >= 0x08E3 && cp <= 0x08FF) ||
               (cp >= 0x0E31 && cp <= 0x0E3A) || (cp >= 0x0E47 && cp <= 0x0E4E) ||
               (cp >= 0x0EB1 && cp <= 0x0EBC) || (cp >= 0x0EC8 && cp <= 0x0ECD);
    }

    static bool isArabicPresentationForm(uint32_t cp) {
        return (cp >= 0xFB50 && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFC);
    }

    // Returns true if cp is an Arabic/Persian/Urdu Presentation Form (Final or Medial, or Tatweel)
    // whose right-hand baseline stroke connects directly into the next visual glyph on its right.
    static bool isRightConnectingArabicForm(uint32_t cp) {
        if (cp == 0x0640) return true; // Tatweel
        if (cp >= 0xFE81 && cp <= 0xFEFC) return (cp & 1u) == 0u;
        if (cp >= 0xFB50 && cp <= 0xFBB1) return (cp & 1u) == 1u;
        if (cp >= 0xFBD3 && cp <= 0xFBDC) return (cp & 1u) == 0u;
        if (cp >= 0xFBDE && cp <= 0xFBFF) return (cp & 1u) == 1u;
        return false;
    }

    bool shouldUseFallback(uint32_t cp, int engFont, EpdFontFamily::Style style) const {
        auto it = renderer.getFontMap().find(engFont);
        if (it != renderer.getFontMap().end()) {
            // Built-in reader fonts (Bookerly / NotoSans) include U+FFFD (replacement glyph),
            // so getGlyph(cp) returns the U+FFFD diamond for any unmapped codepoint.
            // Check hasCodepoint() first so unmapped codepoints route to fallback_noto.bin.
            if (!it->second.hasCodepoint(cp, style)) {
                return true;
            }
            const EpdGlyph* glyph = it->second.getGlyph(cp, style);
            if (!glyph) {
                return true;
            }
            if (fontSector > 0 && cp >= 0x0100) {
                const EpdFontData* fontData = it->second.getData(style);
                if (!renderer.getGlyphBitmap(fontData, glyph)) {
                    return true;
                }
            }
            return false;
        }
        return isFullWidthCjk(cp);
    }

    int getGlyphAdvance(uint32_t cp) {
        if (cp == 0x2713 || cp == 0x2714 || cp == 0x2717 || cp == 0x2718) {
            return 14;
        }
        // Unicode box drawing characters (U+2500 to U+257F) have fixed 16px advance
        if (cp >= 0x2500 && cp <= 0x257F) {
            return 16;
        }
        if (fontSector == 0 || cp >= 131072) return glyphSize;

        // CJK is full width
        if (isFullWidthCjk(cp)) {
            return glyphSize;
        }

        uint8_t bmp[256];
        int readBytes = bytesPerGlyph > 256 ? 256 : bytesPerGlyph;
        
        uint32_t byteOffset = HEADER_SIZE + (cp * bytesPerGlyph);
        uint32_t targetSector = fontSector + (byteOffset / 512);
        uint32_t sectorOffset = byteOffset % 512;
        
        uint8_t sectorBuf[512];
        if (!reader.readSectors(targetSector, 1, sectorBuf)) return glyphSize;
        
        if (sectorOffset + readBytes <= 512) {
            memcpy(bmp, sectorBuf + sectorOffset, readBytes);
        } else {
            int firstPart = 512 - sectorOffset;
            memcpy(bmp, sectorBuf + sectorOffset, firstPart);
            uint8_t nextSector[512];
            if (reader.readSectors(targetSector + 1, 1, nextSector)) {
                memcpy(bmp + firstPart, nextSector, readBytes - firstPart);
            }
        }

        int bytesPerLine = readBytes / glyphSize;
        int minCol = glyphSize;
        int maxCol = -1;
        for (int row = 0; row < glyphSize; row++) {
            for (int col = 0; col < glyphSize; col++) {
                int byteIdx = row * bytesPerLine + (col / 8);
                int bitIdx = 7 - (col % 8);
                if (bmp[byteIdx] & (1 << bitIdx)) {
                    if (col > maxCol) maxCol = col;
                    if (col < minCol) minCol = col;
                }
            }
        }
        
        bool isControl = (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || 
                         (cp >= 0x2060 && cp <= 0x206F) || (cp == 0xFEFF);
        if (isControl || isCombiningCodepoint(cp)) return 0;

        if (maxCol == -1) return glyphSize / 3; // Space
        
        int srcW = (maxCol - minCol) + 1;
        int dstW = (nativeFontPx < 29) ? ((srcW * 8 + 3) / 7) : srcW;
        if (isRightConnectingArabicForm(cp)) {
            int adv = dstW;
            if (adv > glyphSize) adv = glyphSize;
            if (adv < 2) adv = 2;
            return adv;
        }
        int pad = isArabicPresentationForm(cp) ? 1 : 2;
        int advance = dstW + pad; // small padding
        if (advance > glyphSize) advance = glyphSize;
        if (advance < 4) advance = 4;
        return advance;
    }

    int getTextAdvance(const char* text, bool isHeader = false) {
        int total = 0;
        const char* p = text;
        std::string currentChunk = "";
        bool isCurrentChunkCjk = false;
        int engFont = isHeader ? englishHeaderFontId : englishFontId;
        EpdFontFamily::Style style = isHeader ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;

        auto flushChunk = [&]() {
            if (!currentChunk.empty()) {
                if (isCurrentChunkCjk) {
                    const char* cpPtr = currentChunk.c_str();
                    while (*cpPtr) {
                        uint32_t cp = 0;
                        int len = 0;
                        const uint8_t* u8p = reinterpret_cast<const uint8_t*>(cpPtr);
                        unsigned char c = u8p[0];
                        if (c < 0x80) { cp = c; len = 1; }
                        else if ((c & 0xE0) == 0xC0) { cp = ((c & 0x1F) << 6) | (u8p[1] & 0x3F); len = 2; }
                        else if ((c & 0xF0) == 0xE0) { cp = ((c & 0x0F) << 12) | ((u8p[1] & 0x3F) << 6) | (u8p[2] & 0x3F); len = 3; }
                        else if ((c & 0xF8) == 0xF0) { cp = ((c & 0x07) << 18) | ((u8p[1] & 0x3F) << 12) | ((u8p[2] & 0x3F) << 6) | (u8p[3] & 0x3F); len = 4; }
                        else { len = 1; }
                        if (len <= 0) break;
                        cpPtr += len;
                        total += getGlyphAdvance(cp);
                    }
                } else {
                    total += renderer.getTextAdvanceX(engFont, currentChunk.c_str(), style);
                }
                currentChunk.clear();
            }
        };

        while (*p) {
            uint32_t cp = 0;
            const uint8_t* u8p = reinterpret_cast<const uint8_t*>(p);
            int len = 0;
            unsigned char c = u8p[0];
            if (c < 0x80) { cp = c; len = 1; }
            else if ((c & 0xE0) == 0xC0) { cp = ((c & 0x1F) << 6) | (u8p[1] & 0x3F); len = 2; }
            else if ((c & 0xF0) == 0xE0) { cp = ((c & 0x0F) << 12) | ((u8p[1] & 0x3F) << 6) | (u8p[2] & 0x3F); len = 3; }
            else if ((c & 0xF8) == 0xF0) { cp = ((c & 0x07) << 18) | ((u8p[1] & 0x3F) << 12) | ((u8p[2] & 0x3F) << 6) | (u8p[3] & 0x3F); len = 4; }
            else { len = 1; }
            
            if (len <= 0) break;
            
            bool isControl = (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || 
                             (cp >= 0x2060 && cp <= 0x206F) || (cp == 0xFEFF);
            if (isControl) {
                p += len;
                continue;
            }

            bool isCjk = (isCombiningCodepoint(cp) && isCurrentChunkCjk && !currentChunk.empty())
                             ? true
                             : shouldUseFallback(cp, engFont, style);

            if (currentChunk.empty()) {
                isCurrentChunkCjk = isCjk;
                if (cp == '<' && len > 1) currentChunk.push_back('<');
                else if (cp == '>' && len > 1) currentChunk.push_back('>');
                else currentChunk.append(p, len);
            } else if (isCjk == isCurrentChunkCjk) {
                if (cp == '<' && len > 1) currentChunk.push_back('<');
                else if (cp == '>' && len > 1) currentChunk.push_back('>');
                else currentChunk.append(p, len);
            } else {
                flushChunk();
                isCurrentChunkCjk = isCjk;
                if (cp == '<' && len > 1) currentChunk.push_back('<');
                else if (cp == '>' && len > 1) currentChunk.push_back('>');
                else currentChunk.append(p, len);
            }
            p += len;
        }
        flushChunk();
        return total;
    }

    std::vector<std::string> wrapLines(const char* text, int maxWidth, int maxLines) {
        std::vector<std::string> lines;
        if (!text || *text == '\0' || maxLines <= 0) return lines;

        std::string currentLine;
        const char* p = text;
        while (*p && static_cast<int>(lines.size()) < maxLines) {
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
            if (!*p) break;
            const char* wordStart = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
            std::string token(wordStart, p - wordStart);

            if (currentLine.empty()) {
                currentLine = std::move(token);
            } else {
                std::string candidate = currentLine + " " + token;
                if (getTextAdvance(candidate.c_str(), false) <= maxWidth) {
                    currentLine = std::move(candidate);
                } else {
                    lines.push_back(std::move(currentLine));
                    if (static_cast<int>(lines.size()) >= maxLines) {
                        currentLine.clear();
                        break;
                    }
                    currentLine = std::move(token);
                }
            }
        }
        if (!currentLine.empty() && static_cast<int>(lines.size()) < maxLines) {
            lines.push_back(std::move(currentLine));
        }
        return lines;
    }

    void drawText(int x, int y, const char* text, bool black = true, bool isHeader = false) {
        int currentX = x;
        const char* p = text;
        std::string currentChunk = "";
        bool isCurrentChunkCjk = false;
        int engFont = isHeader ? englishHeaderFontId : englishFontId;
        EpdFontFamily::Style style = isHeader ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;

        auto flushChunk = [&]() {
            if (!currentChunk.empty()) {
                if (isCurrentChunkCjk) {
                    std::vector<std::vector<uint32_t>> groups;
                    bool isRtl = false;
                    const char* cpPtr = currentChunk.c_str();
                    while (*cpPtr) {
                        uint32_t cp = 0;
                        int len = 0;
                        const uint8_t* u8p = reinterpret_cast<const uint8_t*>(cpPtr);
                        unsigned char c = u8p[0];
                        if (c < 0x80) { cp = c; len = 1; }
                        else if ((c & 0xE0) == 0xC0) { cp = ((c & 0x1F) << 6) | (u8p[1] & 0x3F); len = 2; }
                        else if ((c & 0xF0) == 0xE0) { cp = ((c & 0x0F) << 12) | ((u8p[1] & 0x3F) << 6) | (u8p[2] & 0x3F); len = 3; }
                        else if ((c & 0xF8) == 0xF0) { cp = ((c & 0x07) << 18) | ((u8p[1] & 0x3F) << 12) | ((u8p[2] & 0x3F) << 6) | (u8p[3] & 0x3F); len = 4; }
                        else { len = 1; }
                        
                        if (len <= 0) break;
                        cpPtr += len;

                        bool isControl = (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || 
                                         (cp >= 0x2060 && cp <= 0x206F) || (cp == 0xFEFF);
                        if (isControl) continue;

                        if (cp >= 0x0590 && cp <= 0x08FF) isRtl = true;

                        bool isCombining = isCombiningCodepoint(cp);
                        
                        if (isCombining && !groups.empty()) {
                            groups.back().push_back(cp);
                        } else {
                            groups.push_back({cp});
                        }
                    }

                    for (const auto& g : groups) {
                        int lastBaseDrawX = currentX;
                        int lastBaseDstW = 10;
                        for (uint32_t cp : g) {
                            if (cp == 0x2713 || cp == 0x2714) {
                                int baselineY = y + renderer.getFontAscenderSize(engFont);
                                int bx = currentX + 1;
                                int by = baselineY - 12;
                                renderer.drawLine(bx, by + 7, bx + 4, by + 11, black);
                                renderer.drawLine(bx, by + 8, bx + 4, by + 12, black);
                                renderer.drawLine(bx + 1, by + 7, bx + 4, by + 10, black);
                                renderer.drawLine(bx + 4, by + 11, bx + 11, by + 1, black);
                                renderer.drawLine(bx + 4, by + 12, bx + 11, by + 2, black);
                                renderer.drawLine(bx + 5, by + 11, bx + 12, by + 1, black);
                                currentX += 14;
                                continue;
                            } else if (cp == 0x2717 || cp == 0x2718) {
                                int baselineY = y + renderer.getFontAscenderSize(engFont);
                                int bx = currentX + 2;
                                int by = baselineY - 11;
                                renderer.drawLine(bx, by, bx + 9, by + 9, black);
                                renderer.drawLine(bx + 1, by, bx + 10, by + 9, black);
                                renderer.drawLine(bx, by + 1, bx + 9, by + 10, black);
                                renderer.drawLine(bx + 9, by, bx, by + 9, black);
                                renderer.drawLine(bx + 10, by, bx + 1, by + 9, black);
                                renderer.drawLine(bx + 9, by + 1, bx, by + 10, black);
                                currentX += 14;
                                continue;
                            } else if (fontSector == 0 && cp >= 0x2500 && cp <= 0x257F) {
                                int cy = y + 6;
                                int cx = currentX + 7;
                                if (cp == 0x2500) {
                                    renderer.drawLine(currentX, cy, currentX + 15, cy, black);
                                    renderer.drawLine(currentX, cy + 1, currentX + 15, cy + 1, black);
                                } else if (cp == 0x251C || cp == 0x2514) {
                                    renderer.drawLine(cx, cy, currentX + 15, cy, black);
                                    renderer.drawLine(cx, cy + 1, currentX + 15, cy + 1, black);
                                }
                                currentX += 16;
                                continue;
                            }
                            if (fontSector > 0 && cp < 131072) {
                                uint8_t glyphBmp[256];
                                int readBytes = bytesPerGlyph > 256 ? 256 : bytesPerGlyph;
                                
                                uint32_t byteOffset = HEADER_SIZE + (cp * bytesPerGlyph);
                                uint32_t targetSector = fontSector + (byteOffset / 512);
                                uint32_t sectorOffset = byteOffset % 512;
                                
                                uint8_t sectorBuf[512];
                                if (reader.readSectors(targetSector, 1, sectorBuf)) {
                                    if (sectorOffset + readBytes <= 512) {
                                        memcpy(glyphBmp, sectorBuf + sectorOffset, readBytes);
                                    } else {
                                        int firstPart = 512 - sectorOffset;
                                        memcpy(glyphBmp, sectorBuf + sectorOffset, firstPart);
                                        uint8_t nextSector[512];
                                        if (reader.readSectors(targetSector + 1, 1, nextSector)) {
                                            memcpy(glyphBmp + firstPart, nextSector, readBytes - firstPart);
                                        }
                                    }
                                }
                                
                                // Align fallback glyph baseline (row 25 in 32x32 FBF1) directly to the primary font's ascender baseline
                                int engAscender = renderer.getFontAscenderSize(engFont);
                                int fallbackBaselineRow = (glyphSize == 32) ? 25 : ((glyphSize * 25) / 32);
                                int drawY = y + engAscender - fallbackBaselineRow;
                                int bytesPerLine = readBytes / glyphSize;
                                int minCol = glyphSize;
                                int maxCol = -1;

                                for (int row = 0; row < glyphSize; row++) {
                                    for (int col = 0; col < glyphSize; col++) {
                                        int byteIdx = row * bytesPerLine + (col / 8);
                                        int bitIdx = 7 - (col % 8);
                                        if ((glyphBmp[byteIdx] & (1 << bitIdx)) != 0) {
                                            if (col < minCol) minCol = col;
                                            if (col > maxCol) maxCol = col;
                                        }
                                    }
                                }
                                if (minCol == glyphSize) minCol = 0;

                                int advance = glyphSize;
                                bool isBoxDrawing = (cp >= 0x2500 && cp <= 0x257F);
                                bool isCjk = isFullWidthCjk(cp);
                                bool isCombining = isCombiningCodepoint(cp);
                                int drawX = currentX;
                                int srcW = (maxCol >= minCol) ? (maxCol - minCol + 1) : 0;
                                int dstW = srcW;
                                bool scaleUpGlyph = (!isBoxDrawing && !isCjk && nativeFontPx < 29 && srcW > 0);
                                if (scaleUpGlyph) {
                                    dstW = (srcW * 8 + 3) / 7;
                                }

                                if (isBoxDrawing) {
                                    advance = 16;
                                    drawX = currentX - 8;
                                } else if (!isCjk) {
                                    if (isCombining) {
                                        drawX = lastBaseDrawX + (lastBaseDstW - dstW) / 2;
                                        advance = 0;
                                    } else if (maxCol == -1) {
                                        advance = glyphSize / 3;
                                    } else if (isRightConnectingArabicForm(cp)) {
                                        advance = dstW;
                                        if (advance > glyphSize) advance = glyphSize;
                                        if (advance < 2) advance = 2;
                                        lastBaseDrawX = drawX;
                                        lastBaseDstW = dstW;
                                    } else {
                                        int pad = isArabicPresentationForm(cp) ? 1 : 2;
                                        advance = dstW + pad;
                                        if (advance > glyphSize) advance = glyphSize;
                                        if (advance < 4) advance = 4;
                                        lastBaseDrawX = drawX;
                                        lastBaseDstW = dstW;
                                    }
                                }

                                if (scaleUpGlyph) {
                                    // Baseline-anchored 8/7 scaling so legacy 26px fallback glyphs match the 14pt article font x-height/cap-height
                                    const int baseRow = fallbackBaselineRow - 1;
                                    for (int dstRelY = -26; dstRelY <= 8; ++dstRelY) {
                                        int srcRelY = (dstRelY >= 0) ? ((dstRelY * 7 + 3) / 8) : -(((-dstRelY) * 7 + 3) / 8);
                                        int srcRow = baseRow + srcRelY;
                                        if (srcRow < 0 || srcRow >= glyphSize) continue;
                                        for (int dstX = 0; dstX < dstW; ++dstX) {
                                            int srcCol = minCol + (dstX * 7 + 3) / 8;
                                            if (srcCol < 0 || srcCol >= glyphSize) continue;
                                            int byteIdx = srcRow * bytesPerLine + (srcCol / 8);
                                            int bitIdx = 7 - (srcCol % 8);
                                            if ((glyphBmp[byteIdx] & (1 << bitIdx)) != 0) {
                                                renderer.drawPixel(drawX + dstX, drawY + baseRow + dstRelY, black);
                                            }
                                        }
                                    }
                                } else {
                                    for (int row = 0; row < glyphSize; row++) {
                                        for (int col = 0; col < glyphSize; col++) {
                                            int byteIdx = row * bytesPerLine + (col / 8);
                                            int bitIdx = 7 - (col % 8);
                                            bool isInk = (glyphBmp[byteIdx] & (1 << bitIdx)) != 0;
                                            if (isInk) {
                                                int px = isBoxDrawing ? (drawX + col) : (drawX + (isCjk ? col : (col - minCol)));
                                                renderer.drawPixel(px, drawY + row, black);
                                            }
                                        }
                                    }
                                }

                                currentX += advance;
                            } else {
                                currentX += glyphSize;
                            }
                        }
                    }
                } else {
                    renderer.drawText(engFont, currentX, y, currentChunk.c_str(), black, style);
                    currentX += renderer.getTextAdvanceX(engFont, currentChunk.c_str(), style);
                }
                currentChunk.clear();
            }
        };

        while (*p) {
            uint32_t cp = 0;
            const uint8_t* u8p = reinterpret_cast<const uint8_t*>(p);
            int len = 0;
            unsigned char c = u8p[0];
            if (c < 0x80) { cp = c; len = 1; }
            else if ((c & 0xE0) == 0xC0) { cp = ((c & 0x1F) << 6) | (u8p[1] & 0x3F); len = 2; }
            else if ((c & 0xF0) == 0xE0) { cp = ((c & 0x0F) << 12) | ((u8p[1] & 0x3F) << 6) | (u8p[2] & 0x3F); len = 3; }
            else if ((c & 0xF8) == 0xF0) { cp = ((c & 0x07) << 18) | ((u8p[1] & 0x3F) << 12) | ((u8p[2] & 0x3F) << 6) | (u8p[3] & 0x3F); len = 4; }
            else { len = 1; }
            
            if (len <= 0) break;

            bool isControl = (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || 
                             (cp >= 0x2060 && cp <= 0x206F) || (cp == 0xFEFF);
            if (isControl) {
                p += len;
                continue;
            }

            bool isCjk = (isCombiningCodepoint(cp) && isCurrentChunkCjk && !currentChunk.empty())
                             ? true
                             : shouldUseFallback(cp, engFont, style);

            if (currentChunk.empty()) {
                isCurrentChunkCjk = isCjk;
                if (cp == '<' && len > 1) currentChunk.push_back('<');
                else if (cp == '>' && len > 1) currentChunk.push_back('>');
                else currentChunk.append(p, len);
            } else if (isCjk == isCurrentChunkCjk) {
                if (cp == '<' && len > 1) currentChunk.push_back('<');
                else if (cp == '>' && len > 1) currentChunk.push_back('>');
                else currentChunk.append(p, len);
            } else {
                flushChunk();
                isCurrentChunkCjk = isCjk;
                if (cp == '<' && len > 1) currentChunk.push_back('<');
                else if (cp == '>' && len > 1) currentChunk.push_back('>');
                else currentChunk.append(p, len);
            }
            p += len;
        }
        flushChunk();
    }
};
