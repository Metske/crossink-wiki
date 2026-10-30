#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

#include "WikiTableViewerActivity.h"
#include "WikiDecompressor.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace {
constexpr uint32_t IMAGE_DECODE_DEBOUNCE_MS = 65;
constexpr size_t MAX_LOGICAL_COLS = 32;
constexpr size_t MAX_ROWSPAN_CLAMP = 200;
constexpr size_t TARGET_WINDOW_CELLS = 120;
constexpr size_t MIN_ROWS_PER_WINDOW = 6;
constexpr size_t MAX_ROWS_PER_WINDOW = 20;

bool isCheckOrCrossSymbol(const std::string& s) {
    return s == "\xe2\x9c\x93" || s == "\xe2\x9c\x94" ||
           s == "\xe2\x9c\x97" || s == "\xe2\x9c\x98";
}
}

WikiTableViewerActivity::WikiTableViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, SdRawReader& reader,
                                                 uint32_t rootSector, uint32_t chunksSector, std::string title, std::string payload)
    : Activity("WikiTableViewer", renderer, mappedInput),
      searchEngine_(reader, rootSector),
      reader_(reader),
      chunksSector_(chunksSector),
      title_(std::move(title)),
      payload_(std::move(payload)) {
    tableStreamLength_ = static_cast<uint32_t>(payload_.size());
}

WikiTableViewerActivity::WikiTableViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, SdRawReader& reader,
                                                 uint32_t rootSector, uint32_t chunksSector, std::string title,
                                                 uint64_t articleOffset, uint32_t tableStreamOffset, uint32_t tableStreamLength,
                                                 bool isCladePayload)
    : Activity("WikiTableViewer", renderer, mappedInput),
      searchEngine_(reader, rootSector),
      reader_(reader),
      chunksSector_(chunksSector),
      title_(std::move(title)),
      articleOffset_(articleOffset),
      tableStreamOffset_(tableStreamOffset),
      tableStreamLength_(tableStreamLength),
      isClade_(isCladePayload) {
}

void WikiTableViewerActivity::onEnter() {
    Activity::onEnter();
    if (!formatter_) {
        const auto& cfg = WikiConfig::get();
        uint32_t fontSec = (cfg.fontSector == UINT32_MAX) ? 0 : cfg.fontSector;
        const int fontId = WikiFont::getBodyFontId();
        formatter_ = std::make_unique<WikiTextFormatter>(renderer, reader_, fontId, fontId, fontSec);
    }
    parsePayload();
    pageRedrawNeeded_ = true;
    requestUpdate();
}

size_t WikiTableViewerActivity::estimateHeapBytes() const {
    // Report true 32-bit RISC-V (ESP32-C3 GCC libstdc++) heap footprint for active window
    constexpr size_t ESP32_VEC_BYTES = 12;
    constexpr size_t ESP32_STR_BYTES = 24;
    constexpr size_t ESP32_CELLDATA_BYTES = 76;
    constexpr size_t ESP32_CELLLAYOUT_BYTES = 20;

    size_t total = (payload_.size() <= 2048) ? payload_.capacity() : 0;
    for (const auto& hr : stickyHeaderRows_) {
        total += hr.capacity() * ESP32_CELLDATA_BYTES;
    }
    total += rows_.capacity() * ESP32_VEC_BYTES;
    for (const auto& r : rows_) {
        total += r.capacity() * ESP32_CELLDATA_BYTES;
        for (const auto& c : r) {
            if (c.displayText.capacity() > 15) total += c.displayText.capacity();
            if (c.linkTarget.capacity() > 15) total += c.linkTarget.capacity();
        }
    }
    total += cellLayouts_.capacity() * ESP32_VEC_BYTES;
    for (const auto& r : cellLayouts_) {
        total += r.capacity() * ESP32_CELLLAYOUT_BYTES;
        for (const auto& cl : r) {
            total += cl.displayLines.capacity() * ESP32_STR_BYTES;
            for (const auto& l : cl.displayLines) {
                if (l.capacity() > 15) total += l.capacity();
            }
        }
    }
    total += windows_.capacity() * 28;
    for (const auto& w : windows_) {
        total += w.entrySpans.capacity() * 60;
    }
    total += colWidths_.capacity() * sizeof(int);
    total += sectorOffsetsX_.capacity() * sizeof(int);
    total += sectorOffsetsY_.capacity() * sizeof(int);
    total += sectorClipWidth_.capacity() * sizeof(int);
    total += sectorClipHeight_.capacity() * sizeof(int);
    return total;
}

void WikiTableViewerActivity::readStreamSlice(uint32_t relOffset, uint32_t length, std::string& outSlice) const {
    outSlice.clear();
    if (length == 0) return;

    if (!payload_.empty()) {
        if (relOffset >= payload_.size()) return;
        size_t avail = std::min<size_t>(length, payload_.size() - relOffset);
        outSlice.assign(payload_.data() + relOffset, avail);
        return;
    }

    if (tableStreamLength_ == 0 || relOffset >= tableStreamLength_) return;
    uint32_t clampedLen = std::min<uint32_t>(length, tableStreamLength_ - relOffset);
    outSlice.resize(clampedLen);

    {
        WikiDecompressor decomp(reader_);
        if (!decomp.beginCluster(chunksSector_, articleOffset_)) {
            outSlice.clear();
            return;
        }
        uint32_t targetStart = tableStreamOffset_ + relOffset;
        char skipBuf[256];
        uint32_t skipped = 0;
        while (skipped < targetStart && !decomp.isDone()) {
            size_t toSkip = std::min<size_t>(sizeof(skipBuf), targetStart - skipped);
            size_t got = decomp.read(skipBuf, toSkip);
            if (got == 0) break;
            skipped += static_cast<uint32_t>(got);
        }
        if (skipped < targetStart) {
            outSlice.clear();
            return;
        }
        uint32_t totalRead = 0;
        while (totalRead < clampedLen && !decomp.isDone()) {
            size_t got = decomp.read(&outSlice[totalRead], clampedLen - totalRead);
            if (got == 0) break;
            totalRead += static_cast<uint32_t>(got);
        }
        outSlice.resize(totalRead);
    }
}

WikiTableViewerActivity::CellData WikiTableViewerActivity::parseRawCell(const std::string& rawStr, bool resolveImgDimensions) {
    CellData cell;
    std::string t = rawStr;

    // Trim leading horizontal spaces before markers
    size_t pos = 0;
    while (pos < t.size() && (t[pos] == ' ' || t[pos] == '\t')) pos++;
    if (pos > 0) t.erase(0, pos);

    // Parse any prefix [H], [C<n>], [R<n>] markers in any order
    while (!t.empty()) {
        if (t.compare(0, 3, "[H]") == 0) {
            cell.isHeader = true;
            t.erase(0, 3);
        } else if (t.size() >= 4 && (t.compare(0, 2, "[C") == 0 || t.compare(0, 2, "[R") == 0)) {
            char kind = t[1];
            size_t closePos = t.find(']', 2);
            if (closePos != std::string::npos && closePos <= 6) {
                bool allDigits = true;
                for (size_t di = 2; di < closePos; ++di) {
                    if (!std::isdigit(static_cast<unsigned char>(t[di]))) {
                        allDigits = false;
                        break;
                    }
                }
                if (allDigits) {
                    int val = std::atoi(t.substr(2, closePos - 2).c_str());
                    if (kind == 'C') {
                        cell.colSpan = static_cast<uint16_t>(std::clamp(val, 1, static_cast<int>(MAX_LOGICAL_COLS)));
                    } else {
                        cell.rowSpan = static_cast<uint16_t>(std::clamp(val, 1, static_cast<int>(MAX_ROWSPAN_CLAMP)));
                    }
                    t.erase(0, closePos + 1);
                    continue;
                }
            }
            break;
        } else {
            break;
        }
        while (!t.empty() && (t[0] == ' ' || t[0] == '\t')) t.erase(0, 1);
    }

    // Extract first <a href="..."> target if present
    size_t hrefPos = t.find("href=\"");
    if (hrefPos != std::string::npos) {
        size_t hrefEnd = t.find('\"', hrefPos + 6);
        if (hrefEnd != std::string::npos && hrefEnd > hrefPos + 6) {
            cell.linkTarget = t.substr(hrefPos + 6, hrefEnd - hrefPos - 6);
        }
    }

    // Extract [IMG:...] if present and strip all [IMG:...] markers from display text
    size_t imgPos = t.find("[IMG:");
    while (imgPos != std::string::npos) {
        size_t keyStart = imgPos + 5;
        size_t imgEnd = t.find(']', keyStart);
        if (imgEnd != std::string::npos) {
            if (!cell.hasImage) {
                cell.hasImage = true;
                if (resolveImgDimensions) {
                    std::string filename = t.substr(keyStart, imgEnd - keyStart);
                    uint64_t searchVal = 0;
                    if (!filename.empty() && searchEngine_.search(filename.c_str(), searchVal)) {
                        uint64_t absoluteBytes = (searchVal >> 32) * 32;
                        cell.imgSector = chunksSector_ + (absoluteBytes / 512);
                        cell.imgOffsetInSector = absoluteBytes % 512;
                        int iw = 0;
                        int ih = 0;
                        if (WikiJpeg::getDimensions(reader_, cell.imgSector, cell.imgOffsetInSector, iw, ih)) {
                            cell.imgWidth = static_cast<int16_t>(iw);
                            cell.imgHeight = static_cast<int16_t>(ih);
                        }
                    }
                }
            }
            t.erase(imgPos, imgEnd - imgPos + 1);
            imgPos = t.find("[IMG:", imgPos);
        } else {
            break;
        }
    }

    // Clean HTML tags, decode entities, and strip any stray [H]/[C..]/[R..] markers
    std::string disp;
    disp.reserve(t.size());
    for (size_t i = 0; i < t.length();) {
        if (t[i] == '<') {
            size_t end = t.find('>', i);
            if (end != std::string::npos) {
                i = end + 1;
            } else {
                i++;
            }
        } else if (t[i] == '[' && (t.compare(i, 3, "[H]") == 0 || t.compare(i, 2, "[C") == 0 || t.compare(i, 2, "[R") == 0)) {
            size_t closePos = t.find(']', i);
            if (closePos != std::string::npos && closePos - i <= 6) {
                i = closePos + 1;
                continue;
            }
            disp += t[i++];
        } else if (t.compare(i, 3, "\\a0") == 0) {
            disp += ' ';
            i += 3;
        } else if (t[i] == '&') {
            if (t.compare(i, 6, "&quot;") == 0) { disp += '\"'; i += 6; }
            else if (t.compare(i, 5, "&#34;") == 0) { disp += '\"'; i += 5; }
            else if (t.compare(i, 6, "&apos;") == 0) { disp += '\''; i += 6; }
            else if (t.compare(i, 5, "&#39;") == 0) { disp += '\''; i += 5; }
            else if (t.compare(i, 5, "&amp;") == 0) { disp += '&'; i += 5; }
            else if (t.compare(i, 4, "&lt;") == 0) { disp += '<'; i += 4; }
            else if (t.compare(i, 4, "&gt;") == 0) { disp += '>'; i += 4; }
            else if (t.compare(i, 6, "&nbsp;") == 0 || t.compare(i, 6, "&#160;") == 0) { disp += ' '; i += 6; }
            else disp += t[i++];
        } else {
            disp += t[i++];
        }
    }

    if (!isClade_) {
        // Trim per-line whitespace and empty lines inside cell
        std::string cleaned;
        cleaned.reserve(disp.size());
        size_t lineStart = 0;
        while (lineStart <= disp.size()) {
            size_t nl = disp.find('\n', lineStart);
            size_t lineEnd = (nl == std::string::npos) ? disp.size() : nl;
            std::string lineOut;
            bool prevSpace = true;
            for (size_t k = lineStart; k < lineEnd; ++k) {
                char ch = disp[k];
                if (ch == ' ' || ch == '\t' || ch == '\r') {
                    if (!prevSpace) {
                        lineOut += ' ';
                        prevSpace = true;
                    }
                } else {
                    lineOut += ch;
                    prevSpace = false;
                }
            }
            if (!lineOut.empty() && lineOut.back() == ' ') {
                lineOut.pop_back();
            }
            if (!lineOut.empty()) {
                if (!cleaned.empty()) cleaned += '\n';
                cleaned += lineOut;
            }
            if (nl == std::string::npos) break;
            lineStart = nl + 1;
        }
        disp = std::move(cleaned);
    }

    cell.displayText = std::move(disp);
    cell.displayText.shrink_to_fit();
    cell.linkTarget.shrink_to_fit();
    return cell;
}

bool WikiTableViewerActivity::parseRawRowLine(const std::string& line, std::vector<CellData>& outRow, bool resolveImgDimensions) {
    outRow.clear();
    if (line.empty()) return false;
    if (line.rfind("===END===", 0) == 0 || line.rfind("===TABLE:", 0) == 0 || line.rfind("===CLADE:", 0) == 0) {
        return false;
    }

    std::string currentText;
    currentText.reserve(64);
    for (size_t i = 0; i < line.length(); ++i) {
        char c = line[i];
        if (c == '\r') continue;
        if (c == '\\' && i + 1 < line.length()) {
            char next = line[i + 1];
            if (next == 'n') { currentText += '\n'; i++; continue; }
            else if (next == '|') { currentText += '|'; i++; continue; }
            else if (next == '\\') { currentText += '\\'; i++; continue; }
        }
        if (c == '|') {
            outRow.push_back(parseRawCell(currentText, resolveImgDimensions));
            currentText.clear();
        } else {
            currentText += c;
        }
    }
    if (currentText != "===END===") {
        outRow.push_back(parseRawCell(currentText, resolveImgDimensions));
    }

    for (const auto& cell : outRow) {
        if (cell.hasImage || !cell.displayText.empty()) {
            return true;
        }
    }
    outRow.clear();
    return false;
}

void WikiTableViewerActivity::expandRawRowIntoGrid(std::vector<CellData>& srcRow, int rowIndex,
                                                   std::vector<ActiveColSpan>& activeCols,
                                                   std::vector<CellData>& outExpandedRow) const {
    outExpandedRow.clear();
    outExpandedRow.reserve(maxLogicalCols_);
    uint16_t logicalCol = 0;

    bool rowHasActiveSpans = false;
    if (!alreadyExpanded_) {
        for (size_t k = 0; k < maxLogicalCols_; ++k) {
            if (activeCols[k].remainingRows > 0) {
                rowHasActiveSpans = true;
                break;
            }
        }
    }

    auto emitActiveSpansAt = [&]() {
        if (alreadyExpanded_) return;
        while (logicalCol < maxLogicalCols_ && activeCols[logicalCol].remainingRows > 0) {
            auto& sp = activeCols[logicalCol];
            CellData cont;
            cont.logicalCol = logicalCol;
            cont.colSpan = sp.isColSpanCont ? 1 : sp.rootColSpan;
            cont.rowSpan = 1;
            cont.isHeader = sp.isHeader;
            cont.isRowSpanCont = true;
            cont.isColSpanCont = sp.isColSpanCont;
            cont.rowSpanRootRow = sp.rootRow;
            cont.colSpanRootCol = sp.rootCol;
            cont.displayText = sp.displayText;
            cont.linkTarget = sp.linkTarget;
            outExpandedRow.push_back(std::move(cont));

            sp.remainingRows--;
            logicalCol++;
        }
    };

    for (size_t ci = 0; ci < srcRow.size(); ++ci) {
        emitActiveSpansAt();
        if (logicalCol >= maxLogicalCols_) break;

        CellData cell = std::move(srcRow[ci]);
        cell.logicalCol = logicalCol;

        size_t nextActiveCol = maxLogicalCols_;
        if (!alreadyExpanded_) {
            for (size_t k = logicalCol; k < maxLogicalCols_; ++k) {
                if (activeCols[k].remainingRows > 0) {
                    nextActiveCol = k;
                    break;
                }
            }
        }
        uint16_t maxAvailSpan = static_cast<uint16_t>(nextActiveCol > logicalCol ? (nextActiveCol - logicalCol) : 1);

        bool isBanner = false;
        if (srcRow.size() == 1 && !rowHasActiveSpans && logicalCol == 0 && maxLogicalCols_ > 1) {
            cell.colSpan = static_cast<uint16_t>(maxLogicalCols_);
            isBanner = true;
        } else if (srcRow.size() == 1 && rowHasActiveSpans) {
            cell.colSpan = maxAvailSpan;
        } else if (cell.colSpan > maxAvailSpan) {
            cell.colSpan = maxAvailSpan;
        }
        if (cell.colSpan == 0) cell.colSpan = 1;

        uint16_t spanW = cell.colSpan;
        int16_t rootCol = static_cast<int16_t>(logicalCol);
        cell.colSpanRootCol = rootCol;
        cell.isFullRowBanner = isBanner;

        if (!alreadyExpanded_ && cell.rowSpan > 1) {
            for (uint16_t k = 0; k < spanW && (logicalCol + k) < maxLogicalCols_; ++k) {
                activeCols[logicalCol + k].remainingRows = cell.rowSpan - 1;
                activeCols[logicalCol + k].rootColSpan = spanW;
                activeCols[logicalCol + k].rootRow = static_cast<int16_t>(rowIndex);
                activeCols[logicalCol + k].rootCol = rootCol;
                activeCols[logicalCol + k].isHeader = cell.isHeader;
                activeCols[logicalCol + k].isColSpanCont = (k > 0);
                activeCols[logicalCol + k].displayText = cell.displayText;
                activeCols[logicalCol + k].linkTarget = cell.linkTarget;
            }
        }

        std::string rootText = cell.displayText;
        std::string rootLink = cell.linkTarget;
        bool rootHeader = cell.isHeader;

        outExpandedRow.push_back(std::move(cell));
        logicalCol++;

        for (uint16_t k = 1; k < spanW && logicalCol < maxLogicalCols_; ++k) {
            CellData cCont;
            cCont.logicalCol = logicalCol;
            cCont.colSpan = 1;
            cCont.rowSpan = 1;
            cCont.isHeader = rootHeader;
            cCont.isColSpanCont = true;
            cCont.isFullRowBanner = isBanner;
            cCont.colSpanRootCol = rootCol;
            if (!isBanner) {
                cCont.displayText = rootText;
                cCont.linkTarget = rootLink;
                cCont.displayText.shrink_to_fit();
                cCont.linkTarget.shrink_to_fit();
            }
            outExpandedRow.push_back(std::move(cCont));
            logicalCol++;
        }
    }

    emitActiveSpansAt();

    while (logicalCol < maxLogicalCols_) {
        CellData pad;
        pad.logicalCol = logicalCol;
        pad.colSpan = 1;
        pad.rowSpan = 1;
        pad.colSpanRootCol = static_cast<int16_t>(logicalCol);
        outExpandedRow.push_back(std::move(pad));
        logicalCol++;
    }
    outExpandedRow.shrink_to_fit();
}

void WikiTableViewerActivity::parsePayload() {
    presetColWidths_.clear();
    bodyStreamStartOffset_ = 0;
    if (payload_.empty() && tableStreamLength_ > 0 && tableStreamLength_ <= 2048) {
        readStreamSlice(0, tableStreamLength_, payload_);
        payload_.shrink_to_fit();
    }
    if (!payload_.empty()) {
        tableStreamLength_ = static_cast<uint32_t>(payload_.size());
    }

    std::string headSlice;
    readStreamSlice(0, std::min<uint32_t>(512, tableStreamLength_), headSlice);

    uint32_t curOff = 0;
    if (headSlice.rfind("===TABLE:", 0) == 0 || headSlice.rfind("===CLADE:", 0) == 0) {
        if (headSlice.rfind("===CLADE:", 0) == 0) {
            isClade_ = true;
        }
        size_t nl = headSlice.find('\n');
        if (nl != std::string::npos) {
            curOff = static_cast<uint32_t>(nl + 1);
            headSlice.erase(0, nl + 1);
        }
    }

    if (headSlice.rfind("[COLS:", 0) == 0) {
        size_t endCols = headSlice.find(']');
        if (endCols != std::string::npos) {
            std::string colsStr = headSlice.substr(6, endCols - 6);
            std::stringstream ss(colsStr);
            std::string item;
            while (std::getline(ss, item, ',')) {
                int w = std::atoi(item.c_str());
                if (w > 0) presetColWidths_.push_back(w);
            }
            size_t adv = endCols + 1;
            if (adv < headSlice.size() && headSlice[adv] == '\r') adv++;
            if (adv < headSlice.size() && headSlice[adv] == '\n') adv++;
            curOff += static_cast<uint32_t>(adv);
            headSlice.erase(0, adv);
        }
    }
    bodyStreamStartOffset_ = curOff;

    isClade_ = isClade_ ||
               (title_.find("Clade") != std::string::npos ||
                title_.find("clade") != std::string::npos ||
                title_.find("Cladogram") != std::string::npos ||
                title_.find("Taxonomy") != std::string::npos ||
                headSlice.find("\xe2\x94") != std::string::npos);

    rebuildWindowsAndLayout();
}

void WikiTableViewerActivity::rebuildWindowsAndLayout() {
    rows_.clear();
    cellLayouts_.clear();
    stickyHeaderRows_.clear();
    windows_.clear();
    colWidths_.clear();
    sectorOffsetsX_.clear();
    sectorOffsetsY_.clear();
    sectorClipWidth_.clear();
    sectorClipHeight_.clear();

    const int fontId = WikiFont::getBodyFontId();
    const int lineHeight = renderer.getLineHeight(fontId) + layoutCfg_.lineSpacing;
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int headerH = metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput);
    const int bottomBarH = mappedInput.hasTouchHardware() ? 10 : (WikiGUI::WIKI_HINTS_HEIGHT + 14);
    const int startX = 16;
    const int renderStartY = headerH + 8;

    viewWidth_ = renderer.getScreenWidth() - (startX * 2);
    viewHeight_ = renderer.getScreenHeight() - renderStartY - bottomBarH;

    auto forEachStreamLine = [&](auto&& callback) {
        constexpr uint32_t CHUNK_SIZE = 2048;
        uint32_t pos = bodyStreamStartOffset_;
        uint32_t lineStartOffset = pos;
        std::string pendingLine;
        std::string chunk;
        while (pos < tableStreamLength_) {
            uint32_t toRead = std::min<uint32_t>(CHUNK_SIZE, tableStreamLength_ - pos);
            readStreamSlice(pos, toRead, chunk);
            if (chunk.empty()) break;

            size_t scan = 0;
            while (scan < chunk.size()) {
                size_t nl = chunk.find('\n', scan);
                if (nl == std::string::npos) {
                    pendingLine.append(chunk, scan, chunk.size() - scan);
                    break;
                }
                pendingLine.append(chunk, scan, nl - scan);
                uint32_t nextLineOffset = pos + static_cast<uint32_t>(nl + 1);
                if (!pendingLine.empty() && pendingLine.back() == '\r') {
                    pendingLine.pop_back();
                }
                if (!callback(pendingLine, lineStartOffset, nextLineOffset)) {
                    return;
                }
                pendingLine.clear();
                lineStartOffset = nextLineOffset;
                scan = nl + 1;
            }
            pos += static_cast<uint32_t>(chunk.size());
        }
        if (!pendingLine.empty()) {
            if (pendingLine.back() == '\r') pendingLine.pop_back();
            callback(pendingLine, lineStartOffset, tableStreamLength_);
        }
    };

    if (isClade_) {
        std::vector<CellData> rawRow;
        uint32_t winStartOff = bodyStreamStartOffset_;
        uint32_t winEndOff = bodyStreamStartOffset_;
        uint16_t winStartRow = 0;
        uint16_t winRowCount = 0;
        size_t totalRows = 0;
        size_t cladeCols = 1;

        forEachStreamLine([&](const std::string& line, uint32_t lineStart, uint32_t lineEnd) {
            if (line.rfind("===END===", 0) == 0) return false;
            if (!parseRawRowLine(line, rawRow, true)) return true;
            if (rawRow.size() > cladeCols) cladeCols = rawRow.size();
            if (winRowCount == 0) {
                winStartOff = lineStart;
                winStartRow = static_cast<uint16_t>(totalRows);
            }
            winEndOff = lineEnd;
            winRowCount++;
            totalRows++;
            if (winRowCount >= 40) {
                RowWindowInfo wi;
                wi.streamByteOffset = winStartOff;
                wi.streamByteLength = winEndOff - winStartOff;
                wi.startDataRow = winStartRow;
                wi.rowCount = winRowCount;
                windows_.push_back(std::move(wi));
                winRowCount = 0;
            }
            return true;
        });

        if (winRowCount > 0) {
            RowWindowInfo wi;
            wi.streamByteOffset = winStartOff;
            wi.streamByteLength = winEndOff - winStartOff;
            wi.startDataRow = winStartRow;
            wi.rowCount = winRowCount;
            windows_.push_back(std::move(wi));
        }

        maxLogicalCols_ = cladeCols;
        stickyTopRowCount_ = 0;
        hasStickyTopRow_ = false;
        hasStickyLeftCol_ = false;
        totalLogicalRows_ = static_cast<int>(totalRows);
        currentWindowIdx_ = 0;
        if (!windows_.empty()) {
            loadRowWindow(0);
        } else {
            computeLayout();
        }
        totalSectorsY_ = 0;
        for (auto& w : windows_) {
            w.startGlobalSecY = static_cast<uint16_t>(totalSectorsY_);
            totalSectorsY_ += std::max<int>(1, w.numSectorsY);
        }
        if (totalSectorsY_ <= 0) totalSectorsY_ = 1;
        return;
    }

    // Pass 1: Lightweight scan to determine maxLogicalCols_, alreadyExpanded_, totalLogicalRows_, and stickyTopRowCount_
    maxLogicalCols_ = 1;
    alreadyExpanded_ = false;
    totalLogicalRows_ = 0;
    stickyTopRowCount_ = 0;
    hasStickyTopRow_ = false;
    {
        std::vector<int> simSpanRem(MAX_LOGICAL_COLS, 0);
        std::vector<CellData> rawRow;
        size_t maxRawRowCols = 1;
        bool hasAnyRowSpan = false;
        bool prevHadRowSpan = false;
        size_t minColsAfterRowSpan = MAX_LOGICAL_COLS;
        bool r0AllHdr = false;
        bool r1AllHdr = false;
        size_t r0RawSize = 0;
        uint16_t r0ColSpan = 1;
        size_t rIdx = 0;

        forEachStreamLine([&](const std::string& line, uint32_t, uint32_t) {
            if (line.rfind("===END===", 0) == 0) return false;
            if (!parseRawRowLine(line, rawRow, false)) return true;

            if (rawRow.size() > maxRawRowCols) maxRawRowCols = rawRow.size();
            size_t rowRawSpanSum = 0;
            bool thisRowHasRowSpan = false;
            bool thisRowAllHdr = true;
            for (const auto& c : rawRow) {
                rowRawSpanSum += (rawRow.size() == 1) ? 1 : c.colSpan;
                if (c.rowSpan > 1) {
                    hasAnyRowSpan = true;
                    thisRowHasRowSpan = true;
                }
                if (!c.isHeader) {
                    thisRowAllHdr = false;
                }
            }
            if (rIdx == 0) {
                r0RawSize = rawRow.size();
                r0ColSpan = rawRow.empty() ? 1 : rawRow[0].colSpan;
                r0AllHdr = thisRowAllHdr && !rawRow.empty();
            } else if (rIdx == 1) {
                r1AllHdr = thisRowAllHdr && r0AllHdr && (rawRow.size() > 1);
            }
            if (prevHadRowSpan && rowRawSpanSum < minColsAfterRowSpan) {
                minColsAfterRowSpan = rowRawSpanSum;
            }
            prevHadRowSpan = thisRowHasRowSpan;

            size_t col = 0;
            for (size_t ci = 0; ci < rawRow.size(); ++ci) {
                while (col < MAX_LOGICAL_COLS && simSpanRem[col] > 0) {
                    simSpanRem[col]--;
                    col++;
                }
                if (col >= MAX_LOGICAL_COLS) break;
                uint16_t cSpan = (rawRow.size() == 1 && rawRow[0].colSpan > 1) ? 1 : rawRow[ci].colSpan;
                uint16_t rSpan = rawRow[ci].rowSpan;
                for (uint16_t s = 0; s < cSpan && (col + s) < MAX_LOGICAL_COLS; ++s) {
                    if (rSpan > 1) {
                        simSpanRem[col + s] = rSpan - 1;
                    }
                }
                col += cSpan;
            }
            while (col < MAX_LOGICAL_COLS && simSpanRem[col] > 0) {
                simSpanRem[col]--;
                col++;
            }
            if (rawRow.size() > 1 && col > maxLogicalCols_) {
                maxLogicalCols_ = col;
            }
            rIdx++;
            return true;
        });

        totalLogicalRows_ = static_cast<int>(rIdx);
        if (maxLogicalCols_ == 1 && maxRawRowCols > 1) {
            maxLogicalCols_ = maxRawRowCols;
        }
        maxLogicalCols_ = std::clamp<size_t>(maxLogicalCols_, 1, MAX_LOGICAL_COLS);
        alreadyExpanded_ = hasAnyRowSpan && (minColsAfterRowSpan >= maxLogicalCols_);
        if (r0RawSize == 1 && maxLogicalCols_ > 1) {
            r0AllHdr = false;
            r1AllHdr = false;
        }

        if (layoutCfg_.stickyMode == 1 && totalLogicalRows_ > 1) {
            stickyTopRowCount_ = (r0AllHdr && r1AllHdr && totalLogicalRows_ > 2) ? 2 : 1;
            hasStickyTopRow_ = true;
        } else if (layoutCfg_.stickyMode == 0 && r0AllHdr && totalLogicalRows_ > 1) {
            stickyTopRowCount_ = (r1AllHdr && totalLogicalRows_ > 2) ? 2 : 1;
            hasStickyTopRow_ = true;
        }
    }

    if (totalLogicalRows_ == 0) {
        computeLayout();
        return;
    }

    // Pass 2: Scoped stream pass to measure global column widths, capture sticky headers, and build sparse RowWindowInfo blocks
    {
        const size_t numCols = maxLogicalCols_;
        const int padX2 = layoutCfg_.cellPadX * 2;
        std::vector<int> colWidths(numCols, layoutCfg_.minColWidth);
        std::vector<int> longestWordW(numCols, layoutCfg_.minColWidth);

        auto measureAdvance = [&](const std::string& s, bool isBold) -> int {
            if (s.empty()) return 0;
            return formatter_ ? formatter_->getTextAdvance(s.c_str(), isBold)
                              : renderer.getTextAdvanceX(fontId, s.c_str(), isBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
        };

        struct SpanWidthReq {
            uint16_t c0 = 0;
            uint16_t cEnd = 0;
            int neededW = 0;
        };
        std::vector<SpanWidthReq> spanReqs;
        std::vector<ActiveColSpan> activeCols(numCols);
        std::vector<CellData> rawRow;
        std::vector<CellData> expandedRow;

        const size_t rowsPerWindow = std::clamp<size_t>(TARGET_WINDOW_CELLS / std::max<size_t>(1, numCols),
                                                        MIN_ROWS_PER_WINDOW, MAX_ROWS_PER_WINDOW);
        bool allLeftHeader = true;
        size_t nonBannerDataRows = 0;
        size_t validFirstColRows = 0;
        size_t globalRowIdx = 0;

        forEachStreamLine([&](const std::string& line, uint32_t lineStart, uint32_t lineEnd) {
            if (line.rfind("===END===", 0) == 0) return false;
            if (!parseRawRowLine(line, rawRow, true)) return true;

            if (globalRowIdx < static_cast<size_t>(stickyTopRowCount_) && rawRow.size() == 1 && numCols > 1) {
                bool anyActiveSpan = false;
                for (size_t k = 0; k < numCols; ++k) {
                    if (activeCols[k].remainingRows > 0) {
                        anyActiveSpan = true;
                        break;
                    }
                }
                if (!anyActiveSpan) {
                    if (globalRowIdx == 0) {
                        hasStickyTopRow_ = false;
                        stickyTopRowCount_ = 0;
                        stickyHeaderRows_.clear();
                    } else {
                        stickyTopRowCount_ = static_cast<int>(globalRowIdx);
                    }
                }
            }

            if (globalRowIdx >= static_cast<size_t>(stickyTopRowCount_)) {
                size_t dataRowIdx = globalRowIdx - static_cast<size_t>(stickyTopRowCount_);
                size_t remRows = (static_cast<size_t>(totalLogicalRows_) > globalRowIdx)
                                     ? (static_cast<size_t>(totalLogicalRows_) - globalRowIdx)
                                     : 0;
                if (((dataRowIdx % rowsPerWindow) == 0 && remRows > 2) || windows_.empty()) {
                    RowWindowInfo wi;
                    wi.streamByteOffset = lineStart;
                    wi.streamByteLength = lineEnd - lineStart;
                    wi.startDataRow = static_cast<uint16_t>(dataRowIdx);
                    wi.rowCount = 1;
                    for (size_t k = 0; k < numCols; ++k) {
                        if (activeCols[k].remainingRows > 0) {
                            ActiveColSpan sp = activeCols[k];
                            sp.col = static_cast<uint16_t>(k);
                            if (sp.rootRow >= stickyTopRowCount_) {
                                sp.rootRow = -1;
                            }
                            wi.entrySpans.push_back(std::move(sp));
                        }
                    }
                    wi.entrySpans.shrink_to_fit();
                    windows_.push_back(std::move(wi));
                } else if (!windows_.empty()) {
                    windows_.back().streamByteLength = lineEnd - windows_.back().streamByteOffset;
                    windows_.back().rowCount++;
                }
            }

            expandRawRowIntoGrid(rawRow, static_cast<int>(globalRowIdx), activeCols, expandedRow);

            if (globalRowIdx < static_cast<size_t>(stickyTopRowCount_)) {
                stickyHeaderRows_.push_back(expandedRow);
            }

            if (!expandedRow.empty() && !expandedRow[0].isFullRowBanner) {
                if (expandedRow[0].colSpan > 1 || !expandedRow[0].isHeader) {
                    allLeftHeader = false;
                }
                if (globalRowIdx > 0) {
                    nonBannerDataRows++;
                    if (expandedRow[0].colSpan == 1 && !expandedRow[0].displayText.empty()) {
                        validFirstColRows++;
                    }
                }
            }

            for (size_t ci = 0; ci < expandedRow.size(); ++ci) {
                const auto& cell = expandedRow[ci];
                if (cell.isRowSpanCont || cell.isColSpanCont || cell.isFullRowBanner) continue;
                size_t c = cell.logicalCol;
                if (c >= numCols) continue;

                if (cell.colSpan > 1 && cell.colSpan < numCols) {
                    if (spanReqs.size() < 32) {
                        int nw = std::min(viewWidth_, measureAdvance(cell.displayText, cell.isHeader) + padX2);
                        spanReqs.push_back(SpanWidthReq{
                            static_cast<uint16_t>(c),
                            static_cast<uint16_t>(std::min(numCols, c + cell.colSpan)),
                            nw});
                    }
                    continue;
                }
                if (cell.colSpan != 1) continue;

                const int colMin = (c == 0 && numCols > 1) ? layoutCfg_.minFirstColWidth : layoutCfg_.minColWidth;
                const int colMax = (c == 0 && numCols > 1) ? layoutCfg_.maxFirstColWidth : layoutCfg_.maxColWidth;

                if (cell.hasImage) {
                    int iw = (cell.imgWidth > 0) ? (cell.imgWidth + padX2) : 140;
                    iw = std::clamp(iw, colMin, colMax);
                    if (iw > colWidths[c]) colWidths[c] = iw;
                    if (iw > longestWordW[c]) longestWordW[c] = iw;
                }
                if (!cell.displayText.empty()) {
                    size_t pos = 0;
                    while (pos < cell.displayText.size()) {
                        while (pos < cell.displayText.size() &&
                               (cell.displayText[pos] == ' ' || cell.displayText[pos] == '\n' || cell.displayText[pos] == '\t')) {
                            pos++;
                        }
                        if (pos >= cell.displayText.size()) break;
                        size_t end = pos;
                        while (end < cell.displayText.size() &&
                               cell.displayText[end] != ' ' && cell.displayText[end] != '\n' && cell.displayText[end] != '\t') {
                            end++;
                        }
                        std::string word = cell.displayText.substr(pos, end - pos);
                        int ww = measureAdvance(word, cell.isHeader) + padX2;
                        int cappedWw = std::min(ww, std::min(colMax, layoutCfg_.maxUnbreakableWordPx + padX2));
                        if (cappedWw > longestWordW[c]) longestWordW[c] = cappedWw;
                        pos = end;
                    }

                    int wrapTarget = std::max(40, colMax - padX2);
                    auto lines = wrapText(cell.displayText, wrapTarget, cell.isHeader);
                    for (const auto& l : lines) {
                        int w = measureAdvance(l, cell.isHeader) + padX2;
                        w = std::clamp(w, colMin, colMax);
                        if (w > colWidths[c]) colWidths[c] = w;
                    }
                }
            }

            globalRowIdx++;
            return true;
        });

        if (windows_.empty()) {
            RowWindowInfo wi;
            wi.streamByteOffset = bodyStreamStartOffset_;
            wi.streamByteLength = 0;
            wi.startDataRow = 0;
            wi.rowCount = 0;
            windows_.push_back(std::move(wi));
        }
        windows_.shrink_to_fit();

        if (!layoutCfg_.ignorePresetCols && presetColWidths_.size() == numCols) {
            colWidths = presetColWidths_;
        } else {
            for (size_t c = 0; c < numCols; ++c) {
                const int colMin = (c == 0 && numCols > 1) ? layoutCfg_.minFirstColWidth : layoutCfg_.minColWidth;
                const int colMax = (c == 0 && numCols > 1) ? layoutCfg_.maxFirstColWidth : layoutCfg_.maxColWidth;
                colWidths[c] = std::clamp(std::max(colWidths[c], longestWordW[c]), colMin, colMax);
            }
            for (const auto& req : spanReqs) {
                if (req.cEnd <= req.c0) continue;
                int spanW = 0;
                for (size_t k = req.c0; k < req.cEnd; ++k) spanW += colWidths[k];
                if (req.neededW > spanW) {
                    int addPerCol = (req.neededW - spanW + static_cast<int>(req.cEnd - req.c0) - 1) / static_cast<int>(req.cEnd - req.c0);
                    for (size_t k = req.c0; k < req.cEnd; ++k) {
                        const int colMax = (k == 0 && numCols > 1) ? layoutCfg_.maxFirstColWidth : layoutCfg_.maxColWidth;
                        colWidths[k] = std::min(colMax, colWidths[k] + addPerCol);
                    }
                }
            }
        }

        int totalTableW = 0;
        for (size_t c = 0; c < numCols; ++c) totalTableW += colWidths[c];

        if (totalTableW > viewWidth_ && numCols > 0) {
            int totalMinSafeW = 0;
            for (size_t c = 0; c < numCols; ++c) {
                totalMinSafeW += std::min(colWidths[c], longestWordW[c]);
            }
            if (totalMinSafeW <= viewWidth_) {
                int excessNeeded = totalTableW - viewWidth_;
                int totalSlack = totalTableW - totalMinSafeW;
                if (totalSlack > 0) {
                    int removed = 0;
                    for (size_t c = 0; c < numCols; ++c) {
                        int minSafe = std::min(colWidths[c], longestWordW[c]);
                        int slack = colWidths[c] - minSafe;
                        int cut = (slack * excessNeeded) / totalSlack;
                        colWidths[c] -= cut;
                        removed += cut;
                    }
                    for (size_t c = numCols; c > 0 && removed < excessNeeded; --c) {
                        int minSafe = std::min(colWidths[c - 1], longestWordW[c - 1]);
                        if (colWidths[c - 1] > minSafe) {
                            int d = std::min(colWidths[c - 1] - minSafe, excessNeeded - removed);
                            colWidths[c - 1] -= d;
                            removed += d;
                        }
                    }
                    totalTableW = viewWidth_;
                }
            }
        } else if (totalTableW < viewWidth_ && numCols > 0) {
            int extraPerCol = (viewWidth_ - totalTableW) / static_cast<int>(numCols);
            int rem = (viewWidth_ - totalTableW) % static_cast<int>(numCols);
            for (size_t c = 0; c < numCols; ++c) {
                colWidths[c] += extraPerCol + (c < static_cast<size_t>(rem) ? 1 : 0);
            }
            totalTableW = viewWidth_;
        }

        hasStickyLeftCol_ = false;
        if (layoutCfg_.stickyMode == 1) {
            hasStickyLeftCol_ = (numCols > 1);
        } else if (layoutCfg_.stickyMode == 0 && numCols > 1) {
            bool firstColIsIdentifier = (nonBannerDataRows > 0 && validFirstColRows * 5 >= nonBannerDataRows * 4);
            if (allLeftHeader || (totalTableW > viewWidth_ && firstColIsIdentifier)) {
                hasStickyLeftCol_ = true;
            }
        }

        int maxStickyW = (viewWidth_ * layoutCfg_.stickyLeftMaxRatioPct) / 100;
        if (hasStickyLeftCol_ && numCols > 1) {
            if (colWidths[0] > maxStickyW) {
                int colMin = std::min(layoutCfg_.minFirstColWidth, maxStickyW);
                colWidths[0] = std::max(colMin, maxStickyW);
            }
            stickyLeftWidth_ = colWidths[0];
        } else {
            stickyLeftWidth_ = 0;
        }

        totalTableW = 0;
        for (size_t c = 0; c < numCols; ++c) totalTableW += colWidths[c];

        if (layoutCfg_.snapSectors && totalTableW > viewWidth_ && numCols > 1) {
            int stickyW = (hasStickyLeftCol_ && numCols > 1) ? stickyLeftWidth_ : 0;
            int availW = std::max(60, viewWidth_ - stickyW);
            size_t startC = (hasStickyLeftCol_ && numCols > 1) ? 1 : 0;
            size_t colIdx = startC;
            while (colIdx < numCols) {
                size_t nextCol = colIdx;
                int sumW = 0;
                while (nextCol < numCols && sumW + colWidths[nextCol] <= availW) {
                    sumW += colWidths[nextCol];
                    nextCol++;
                }
                if (nextCol == colIdx) {
                    colWidths[colIdx] = availW;
                    nextCol = colIdx + 1;
                } else if (sumW < availW) {
                    int extra = availW - sumW;
                    size_t count = nextCol - colIdx;
                    for (size_t k = colIdx; k < nextCol; ++k) {
                        colWidths[k] += extra / static_cast<int>(count) + ((k - colIdx) < (extra % count) ? 1 : 0);
                    }
                }
                colIdx = nextCol;
            }
        }
        colWidths_ = std::move(colWidths);
        colWidths_.shrink_to_fit();
    }

    currentWindowIdx_ = 0;
    loadRowWindow(0);

    // Estimate global vertical sector count across windows
    int win0Sectors = std::max(1, numSectorsY_);
    int win0Rows = (!windows_.empty() && windows_[0].rowCount > 0) ? windows_[0].rowCount : 1;
    totalSectorsY_ = 0;
    for (size_t wi = 0; wi < windows_.size(); ++wi) {
        windows_[wi].startGlobalSecY = static_cast<uint16_t>(totalSectorsY_);
        if (wi == 0) {
            windows_[wi].numSectorsY = static_cast<uint16_t>(win0Sectors);
        } else {
            int est = std::max(1, (static_cast<int>(windows_[wi].rowCount) * win0Sectors + win0Rows - 1) / win0Rows);
            windows_[wi].numSectorsY = static_cast<uint16_t>(est);
        }
        totalSectorsY_ += windows_[wi].numSectorsY;
    }
    if (totalSectorsY_ <= 0) totalSectorsY_ = 1;
}

void WikiTableViewerActivity::loadRowWindow(size_t windowIdx) {
    if (windows_.empty()) {
        computeLayout();
        return;
    }
    if (windowIdx >= windows_.size()) {
        windowIdx = windows_.size() - 1;
    }
    currentWindowIdx_ = windowIdx;
    const auto& win = windows_[windowIdx];

    cellLayouts_.clear();
    cellLayouts_.shrink_to_fit();
    rows_.clear();
    if (isClade_) {
        {
            std::string slice;
            readStreamSlice(win.streamByteOffset, win.streamByteLength, slice);
            std::vector<CellData> rawRow;
            size_t pos = 0;
            while (pos < slice.size()) {
                size_t nl = slice.find('\n', pos);
                size_t end = (nl == std::string::npos) ? slice.size() : nl;
                std::string line = slice.substr(pos, end - pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (parseRawRowLine(line, rawRow, true)) {
                    for (size_t c = 0; c < rawRow.size(); ++c) {
                        rawRow[c].logicalCol = static_cast<uint16_t>(c);
                        rawRow[c].colSpan = 1;
                        rawRow[c].rowSpan = 1;
                    }
                    rows_.push_back(std::move(rawRow));
                }
                if (nl == std::string::npos) break;
                pos = nl + 1;
            }
        }
        rows_.shrink_to_fit();
        computeLayout();
        windows_[windowIdx].numSectorsY = static_cast<uint16_t>(std::max(1, numSectorsY_));
        return;
    }

    rows_.reserve(static_cast<size_t>(stickyTopRowCount_) + win.rowCount);
    for (int hr = 0; hr < stickyTopRowCount_ && static_cast<size_t>(hr) < stickyHeaderRows_.size(); ++hr) {
        rows_.push_back(stickyHeaderRows_[hr]);
    }

    if (win.streamByteLength > 0 && win.rowCount > 0) {
        std::string slice;
        readStreamSlice(win.streamByteOffset, win.streamByteLength, slice);
        std::vector<ActiveColSpan> activeCols(maxLogicalCols_);
        for (const auto& sp : win.entrySpans) {
            if (sp.col < maxLogicalCols_) {
                activeCols[sp.col] = sp;
            }
        }
        std::vector<CellData> rawRow;
        std::vector<CellData> expandedRow;
        size_t pos = 0;
        while (pos < slice.size()) {
            size_t nl = slice.find('\n', pos);
            size_t end = (nl == std::string::npos) ? slice.size() : nl;
            std::string line = slice.substr(pos, end - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (parseRawRowLine(line, rawRow, true)) {
                int localRowIdx = static_cast<int>(rows_.size());
                expandRawRowIntoGrid(rawRow, localRowIdx, activeCols, expandedRow);
                rows_.push_back(std::move(expandedRow));
            }
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }

    rows_.shrink_to_fit();
    computeLayout();

    // Update exact sector count for this window and recalculate totalSectorsY_
    windows_[windowIdx].numSectorsY = static_cast<uint16_t>(std::max(1, numSectorsY_));
    totalSectorsY_ = 0;
    for (auto& w : windows_) {
        w.startGlobalSecY = static_cast<uint16_t>(totalSectorsY_);
        totalSectorsY_ += std::max<int>(1, w.numSectorsY);
    }
    if (totalSectorsY_ <= 0) totalSectorsY_ = 1;
}

void WikiTableViewerActivity::setFocusAndSector(int row, int col, int secX, int secY) {
    // Switch row window if global row or global secY falls in a different window
    if (!windows_.empty()) {
        if (row >= stickyTopRowCount_) {
            int targetDataRow = row - stickyTopRowCount_;
            for (size_t wi = 0; wi < windows_.size(); ++wi) {
                int startR = windows_[wi].startDataRow;
                int endR = startR + windows_[wi].rowCount;
                if (targetDataRow >= startR && targetDataRow < endR) {
                    if (wi != currentWindowIdx_) {
                        loadRowWindow(wi);
                    }
                    row = stickyTopRowCount_ + (targetDataRow - startR);
                    break;
                }
            }
        } else if (secY >= 0 && secY >= numSectorsY_ && windows_.size() > 1) {
            for (size_t wi = 0; wi < windows_.size(); ++wi) {
                int startSy = windows_[wi].startGlobalSecY;
                int endSy = startSy + windows_[wi].numSectorsY;
                if (secY >= startSy && secY < endSy) {
                    if (wi != currentWindowIdx_) {
                        loadRowWindow(wi);
                    }
                    secY -= windows_[wi].startGlobalSecY;
                    break;
                }
            }
        }
    }

    if (!rows_.empty()) {
        focusedRow_ = std::clamp(row, 0, static_cast<int>(rows_.size()) - 1);
        if (!rows_[focusedRow_].empty()) {
            focusedCol_ = std::clamp(col, 0, static_cast<int>(rows_[focusedRow_].size()) - 1);
        } else {
            focusedCol_ = 0;
        }
    } else {
        focusedRow_ = 0;
        focusedCol_ = 0;
    }
    if (secX >= 0 && secY >= 0) {
        currentSecX_ = std::clamp(secX, 0, std::max(0, numSectorsX_ - 1));
        currentSecY_ = std::clamp(secY, 0, std::max(0, numSectorsY_ - 1));
        scrollX_ = (currentSecX_ < static_cast<int>(sectorOffsetsX_.size())) ? sectorOffsetsX_[currentSecX_] : 0;
        scrollY_ = (currentSecY_ < static_cast<int>(sectorOffsetsY_.size())) ? sectorOffsetsY_[currentSecY_] : 0;
    } else {
        updateScrollForFocusedCell();
    }
    pageRedrawNeeded_ = true;
}

std::vector<std::string> WikiTableViewerActivity::wrapText(const std::string& text, int maxWidth, bool isBold) {
    std::vector<std::string> lines;
    if (text.empty()) {
        lines.push_back("");
        return lines;
    }

    if (isClade_) {
        std::string cur;
        for (char c : text) {
            if (c == '\n') {
                lines.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        lines.push_back(cur);
        return lines;
    }

    const int fontId = WikiFont::getBodyFontId();
    auto getAdvance = [&](const std::string& s) {
        return formatter_ ? formatter_->getTextAdvance(s.c_str(), isBold)
                          : renderer.getTextAdvanceX(fontId, s.c_str(), isBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    };

    std::string currentLine;
    std::string currentWord;

    auto pushWord = [&](const std::string& word) {
        if (word.empty()) return;
        std::string testLine = currentLine.empty() ? word : currentLine + " " + word;
        if (getAdvance(testLine) <= maxWidth) {
            currentLine = testLine;
        } else {
            if (!currentLine.empty()) {
                lines.push_back(currentLine);
                currentLine.clear();
            }
            if (getAdvance(word) <= maxWidth) {
                currentLine = word;
            } else {
                std::string rem = word;
                while (!rem.empty()) {
                    if (getAdvance(rem) <= maxWidth) {
                        currentLine = rem;
                        break;
                    }
                    size_t charLen = 1;
                    while (charLen < rem.length() && (static_cast<uint8_t>(rem[charLen]) & 0xC0) == 0x80) {
                        charLen++;
                    }
                    while (charLen < rem.length()) {
                        size_t nextLen = charLen + 1;
                        while (nextLen < rem.length() && (static_cast<uint8_t>(rem[nextLen]) & 0xC0) == 0x80) {
                            nextLen++;
                        }
                        std::string sub = rem.substr(0, nextLen);
                        if (getAdvance(sub) > maxWidth) break;
                        charLen = nextLen;
                    }
                    lines.push_back(rem.substr(0, charLen));
                    rem = rem.substr(charLen);
                }
            }
        }
    };

    for (size_t i = 0; i < text.length(); ++i) {
        char c = text[i];
        if (c == '\n') {
            pushWord(currentWord);
            currentWord.clear();
            if (!currentLine.empty()) {
                lines.push_back(currentLine);
                currentLine.clear();
            }
        } else if (c == ' ') {
            pushWord(currentWord);
            currentWord.clear();
        } else {
            currentWord += c;
        }
    }
    pushWord(currentWord);
    if (!currentLine.empty()) {
        lines.push_back(currentLine);
    }
    if (lines.empty()) {
        lines.push_back("");
    }
    return lines;
}

void WikiTableViewerActivity::computeLayout() {
    cellLayouts_.clear();
    cellLayouts_.resize(rows_.size());
    sectorOffsetsX_.clear();
    sectorOffsetsY_.clear();
    sectorClipWidth_.clear();
    sectorClipHeight_.clear();

    const int fontId = WikiFont::getBodyFontId();
    const int lineHeight = renderer.getLineHeight(fontId) + layoutCfg_.lineSpacing;
    const auto& metrics = UITheme::getInstance().getMetrics();

    const int headerH = metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput);
    const int bottomBarH = mappedInput.hasTouchHardware() ? 10 : (WikiGUI::WIKI_HINTS_HEIGHT + 14);
    const int startX = 16;
    const int renderStartY = headerH + 8;

    viewWidth_ = renderer.getScreenWidth() - (startX * 2);
    viewHeight_ = renderer.getScreenHeight() - renderStartY - bottomBarH;

    if (rows_.empty()) {
        maxScrollX_ = 0;
        maxScrollY_ = 0;
        numSectorsX_ = 1;
        numSectorsY_ = 1;
        sectorOffsetsX_.push_back(0);
        sectorOffsetsY_.push_back(0);
        sectorClipWidth_.push_back(viewWidth_);
        sectorClipHeight_.push_back(viewHeight_);
        return;
    }

    auto measureAdvance = [&](const std::string& s, bool isBold) -> int {
        if (s.empty()) return 0;
        return formatter_ ? formatter_->getTextAdvance(s.c_str(), isBold)
                          : renderer.getTextAdvanceX(fontId, s.c_str(), isBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    };

    if (isClade_) {
        size_t cladeCols = 0;
        for (const auto& r : rows_) {
            if (r.size() > cladeCols) cladeCols = r.size();
        }
        colWidths_.assign(cladeCols, 60);

        int y = 0;
        for (size_t r = 0; r < rows_.size(); ++r) {
            cellLayouts_[r].resize(rows_[r].size());
            int maxRowHeight = lineHeight;
            for (size_t c = 0; c < rows_[r].size(); ++c) {
                auto& cellData = rows_[r][c];
                auto& layout = cellLayouts_[r][c];
                layout.y = y;
                if (cellData.hasImage) {
                    int iw = (cellData.imgWidth > 0) ? (cellData.imgWidth + 10) : 150;
                    int ih = (cellData.imgHeight > 0) ? (cellData.imgHeight + 10) : 150;
                    layout.width = std::max(60, iw);
                    if (ih > maxRowHeight) maxRowHeight = ih;
                } else {
                    layout.displayLines = wrapText(cellData.displayText, 4000, cellData.isHeader);
                    layout.displayLines.shrink_to_fit();
                    int maxLineWidth = 60;
                    for (const auto& line : layout.displayLines) {
                        int w1 = measureAdvance(line, cellData.isHeader);
                        int w2 = renderer.getTextAdvanceX(fontId, line.c_str(),
                                                          cellData.isHeader ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
                        int w = std::max(w1, w2) + 16;
                        if (w > maxLineWidth) maxLineWidth = w;
                    }
                    layout.width = maxLineWidth;
                    int h = static_cast<int>(layout.displayLines.size()) * lineHeight + 4;
                    if (h > maxRowHeight) maxRowHeight = h;
                }
                if (layout.width > colWidths_[c]) {
                    colWidths_[c] = layout.width;
                }
            }
            for (size_t c = 0; c < rows_[r].size(); ++c) cellLayouts_[r][c].height = maxRowHeight;
            y += maxRowHeight;
        }
        std::vector<int> cladeColX(cladeCols, 0);
        int curCladeX = 0;
        for (size_t c = 0; c < cladeCols; ++c) {
            cladeColX[c] = curCladeX;
            curCladeX += colWidths_[c];
        }
        for (size_t r = 0; r < rows_.size(); ++r) {
            for (size_t c = 0; c < rows_[r].size(); ++c) {
                cellLayouts_[r][c].x = cladeColX[c];
                cellLayouts_[r][c].width = colWidths_[c];
            }
        }
        maxScrollX_ = curCladeX;
        maxScrollY_ = y;
        hasStickyTopRow_ = false;
        hasStickyLeftCol_ = false;
        stickyTopHeight_ = 0;
        stickyLeftWidth_ = 0;

        int scrollableW = std::max(10, viewWidth_);
        int scrollableH = std::max(10, viewHeight_);
        numSectorsX_ = (maxScrollX_ > scrollableW) ? ((maxScrollX_ - 1) / scrollableW + 1) : 1;
        for (int sx = 0; sx < numSectorsX_; ++sx) {
            sectorOffsetsX_.push_back(sx * scrollableW);
            sectorClipWidth_.push_back(scrollableW);
        }

        if (layoutCfg_.snapSectors && !rows_.empty()) {
            size_t rowIdx = 0;
            while (rowIdx < rows_.size()) {
                int rowY = cellLayouts_[rowIdx][0].y;
                int rowH = cellLayouts_[rowIdx][0].height;
                size_t nextRow = rowIdx;
                while (nextRow < rows_.size()) {
                    int rY = cellLayouts_[nextRow][0].y;
                    int rH = cellLayouts_[nextRow][0].height;
                    if ((rY + rH) - rowY <= scrollableH) nextRow++;
                    else break;
                }
                if (nextRow > rowIdx) {
                    int fittedH = (cellLayouts_[nextRow - 1][0].y + cellLayouts_[nextRow - 1][0].height) - rowY;
                    sectorOffsetsY_.push_back(rowY);
                    sectorClipHeight_.push_back(fittedH);
                    rowIdx = nextRow;
                } else {
                    int stepH = std::max(lineHeight, ((scrollableH - 4) / lineHeight) * lineHeight);
                    for (int subOff = 0; subOff < rowH; subOff += stepH) {
                        int remH = rowH - subOff;
                        int sliceH = (remH > scrollableH) ? (stepH + 4) : remH;
                        sliceH = std::min(scrollableH, sliceH);
                        sectorOffsetsY_.push_back(rowY + subOff);
                        sectorClipHeight_.push_back(sliceH);
                    }
                    rowIdx++;
                }
            }
            numSectorsY_ = std::max(1, static_cast<int>(sectorOffsetsY_.size()));
        } else {
            numSectorsY_ = (maxScrollY_ > scrollableH) ? ((maxScrollY_ - 1) / scrollableH + 1) : 1;
            for (int sy = 0; sy < numSectorsY_; ++sy) {
                sectorOffsetsY_.push_back(sy * scrollableH);
                sectorClipHeight_.push_back(scrollableH);
            }
        }
        return;
    }

    // Multi-Column Data Table:
    const size_t numCols = colWidths_.size();
    if (numCols == 0) return;

    for (size_t r = 0; r < rows_.size(); ++r) {
        cellLayouts_[r].resize(rows_[r].size());
    }

    const int padX2 = layoutCfg_.cellPadX * 2;
    std::vector<int> colX(numCols, 0);
    int curX = 0;
    for (size_t c = 0; c < numCols; ++c) {
        colX[c] = curX;
        curX += colWidths_[c];
    }
    maxScrollX_ = curX;

    int stickyW = (hasStickyLeftCol_ && numCols > 1) ? stickyLeftWidth_ : 0;
    int availW = std::max(60, viewWidth_ - stickyW);
    int totalScrollableW = std::max(0, maxScrollX_ - stickyW);
    std::vector<int> colSector(numCols, 0);
    if (hasStickyLeftCol_ && numCols > 1) {
        colSector[0] = -1;
    }

    if (layoutCfg_.snapSectors && numCols > 1) {
        size_t startC = (hasStickyLeftCol_ && numCols > 1) ? 1 : 0;
        size_t colIdx = startC;
        int secIdx = 0;
        while (colIdx < numCols) {
            int secStartX = colX[colIdx] - stickyW;
            size_t nextCol = colIdx;
            while (nextCol < numCols && (colX[nextCol] + colWidths_[nextCol] - stickyW) - secStartX <= availW) {
                colSector[nextCol] = secIdx;
                nextCol++;
            }
            if (nextCol == colIdx) {
                colSector[colIdx] = secIdx;
                sectorOffsetsX_.push_back(secStartX);
                sectorClipWidth_.push_back(availW);
                colIdx++;
            } else {
                int fittedW = (colX[nextCol - 1] + colWidths_[nextCol - 1] - stickyW) - secStartX;
                sectorOffsetsX_.push_back(secStartX);
                sectorClipWidth_.push_back(fittedW);
                colIdx = nextCol;
            }
            secIdx++;
        }
    } else {
        int nX = (totalScrollableW > availW) ? ((totalScrollableW - 1) / availW + 1) : 1;
        for (int sx = 0; sx < nX; ++sx) {
            sectorOffsetsX_.push_back(sx * availW);
            sectorClipWidth_.push_back(availW);
        }
        size_t startC = (hasStickyLeftCol_ && numCols > 1) ? 1 : 0;
        for (size_t c = startC; c < numCols; ++c) {
            colSector[c] = (availW > 0) ? ((colX[c] - stickyW) / availW) : 0;
        }
    }
    if (sectorOffsetsX_.empty()) {
        sectorOffsetsX_.push_back(0);
        sectorClipWidth_.push_back(availW);
    }
    numSectorsX_ = static_cast<int>(sectorOffsetsX_.size());

    int minVisibleTableW = viewWidth_;
    for (int sx = 0; sx < numSectorsX_; ++sx) {
        int visW = stickyW + std::min(availW, sectorClipWidth_[sx]);
        if (visW > 0 && visW < minVisibleTableW) minVisibleTableW = visW;
    }

    const int maxStickyH = (viewHeight_ * layoutCfg_.stickyTopMaxRatioPct) / 100;
    int curY = 0;
    stickyTopHeight_ = 0;
    for (size_t r = 0; r < rows_.size(); ++r) {
        int maxRowH = lineHeight + layoutCfg_.cellPadY * 2;
        bool rowIsBanner = (!rows_[r].empty() && rows_[r][0].isFullRowBanner);

        for (size_t ci = 0; ci < rows_[r].size(); ++ci) {
            const auto& cellData = rows_[r][ci];
            auto& layout = cellLayouts_[r][ci];
            size_t logCol = std::min(static_cast<size_t>(cellData.logicalCol), numCols - 1);
            layout.x = colX[logCol];

            if (rowIsBanner) {
                if (ci == 0) {
                    layout.width = minVisibleTableW;
                    if (!cellData.displayText.empty()) {
                        layout.displayLines = wrapText(cellData.displayText, std::max(36, minVisibleTableW - padX2), cellData.isHeader);
                        layout.displayLines.shrink_to_fit();
                        int h = static_cast<int>(layout.displayLines.size()) * lineHeight + layoutCfg_.cellPadY * 2 + 2;
                        if (h > maxRowH) maxRowH = h;
                    } else {
                        layout.displayLines.clear();
                    }
                } else {
                    layout.width = 0;
                    layout.displayLines.clear();
                }
                continue;
            }

            size_t rootC = cellData.isColSpanCont
                               ? std::min(static_cast<size_t>(std::max<int16_t>(0, cellData.colSpanRootCol)), numCols - 1)
                               : logCol;
            size_t rootSpan = (rootC < rows_[r].size()) ? std::max<size_t>(1, rows_[r][rootC].colSpan) : 1;
            size_t spanEndC = std::min(numCols, rootC + rootSpan);

            bool isSectorSpanStart = (logCol == rootC) || (logCol > 0 && colSector[logCol] != colSector[logCol - 1]);
            if (!isSectorSpanStart) {
                layout.width = 0;
                layout.displayLines.clear();
                continue;
            }

            int mergedSliceW = 0;
            for (size_t k = logCol; k < spanEndC && colSector[k] == colSector[logCol]; ++k) {
                mergedSliceW += colWidths_[k];
            }
            if (mergedSliceW <= 0) mergedSliceW = colWidths_[logCol];
            layout.width = mergedSliceW;

            if (cellData.hasImage) {
                int ih = (cellData.imgHeight > 0) ? (cellData.imgHeight + layoutCfg_.cellPadY * 2 + 2) : 140;
                if (ih > maxRowH) maxRowH = ih;
            }
            if (!cellData.displayText.empty()) {
                layout.displayLines = wrapText(cellData.displayText, std::max(36, mergedSliceW - padX2), cellData.isHeader);
                layout.displayLines.shrink_to_fit();
                bool sharedAcrossStickyRows = (hasStickyTopRow_ && stickyTopRowCount_ > 1 && r == 0 && cellData.rowSpan > 1);
                if (!cellData.isRowSpanCont && !sharedAcrossStickyRows) {
                    int h = static_cast<int>(layout.displayLines.size()) * lineHeight + layoutCfg_.cellPadY * 2 + 2;
                    if (h > maxRowH) maxRowH = h;
                }
            } else {
                layout.displayLines.clear();
            }
        }
        for (size_t ci = 0; ci < rows_[r].size(); ++ci) {
            cellLayouts_[r][ci].y = curY;
            cellLayouts_[r][ci].height = maxRowH;
        }
        curY += maxRowH;
    }
    maxScrollY_ = curY;

    if (hasStickyTopRow_ && stickyTopRowCount_ > 0 && !cellLayouts_.empty()) {
        int h0 = cellLayouts_[0][0].height;
        int hTotal = 0;
        for (int sr = 0; sr < stickyTopRowCount_ && static_cast<size_t>(sr) < cellLayouts_.size(); ++sr) {
            hTotal += cellLayouts_[sr][0].height;
        }
        if (hTotal <= maxStickyH || layoutCfg_.stickyMode == 1) {
            stickyTopHeight_ = hTotal;
        } else if (stickyTopRowCount_ > 1 && h0 <= maxStickyH) {
            stickyTopRowCount_ = 1;
            stickyTopHeight_ = h0;
        } else {
            hasStickyTopRow_ = false;
            stickyTopRowCount_ = 0;
            stickyTopHeight_ = 0;
        }
    } else {
        hasStickyTopRow_ = false;
        stickyTopRowCount_ = 0;
        stickyTopHeight_ = 0;
    }

    // Build Vertical Sectors (sectorOffsetsY_ and sectorClipHeight_)
    int stickyH = hasStickyTopRow_ ? stickyTopHeight_ : 0;
    int availH = std::max(lineHeight * 2, viewHeight_ - stickyH);
    int totalScrollableH = std::max(0, maxScrollY_ - stickyH);

    if (layoutCfg_.snapSectors && !rows_.empty()) {
        size_t startR = hasStickyTopRow_ ? static_cast<size_t>(stickyTopRowCount_) : 0;
        size_t rowIdx = startR;
        while (rowIdx < rows_.size()) {
            int rowY = cellLayouts_[rowIdx][0].y - stickyH;
            int rowH = cellLayouts_[rowIdx][0].height;
            size_t nextRow = rowIdx;
            while (nextRow < rows_.size()) {
                int rY = cellLayouts_[nextRow][0].y - stickyH;
                int rH = cellLayouts_[nextRow][0].height;
                if ((rY + rH) - rowY <= availH) {
                    nextRow++;
                } else {
                    break;
                }
            }
            if (nextRow > rowIdx) {
                int lastRowY = cellLayouts_[nextRow - 1][0].y - stickyH;
                int lastRowH = cellLayouts_[nextRow - 1][0].height;
                int fittedH = (lastRowY + lastRowH) - rowY;
                sectorOffsetsY_.push_back(rowY);
                sectorClipHeight_.push_back(fittedH);
                rowIdx = nextRow;
            } else {
                int stepH = std::max(lineHeight, ((availH - layoutCfg_.cellPadY * 2) / lineHeight) * lineHeight);
                for (int subOff = 0; subOff < rowH; subOff += stepH) {
                    int remH = rowH - subOff;
                    int sliceH = (remH > availH) ? (stepH + layoutCfg_.cellPadY * 2) : remH;
                    sliceH = std::min(availH, sliceH);
                    sectorOffsetsY_.push_back(rowY + subOff);
                    sectorClipHeight_.push_back(sliceH);
                }
                rowIdx++;
            }
        }
    } else {
        int nY = (totalScrollableH > availH) ? ((totalScrollableH - 1) / availH + 1) : 1;
        for (int sy = 0; sy < nY; ++sy) {
            sectorOffsetsY_.push_back(sy * availH);
            sectorClipHeight_.push_back(availH);
        }
    }
    if (sectorOffsetsY_.empty()) {
        sectorOffsetsY_.push_back(0);
        sectorClipHeight_.push_back(availH);
    }
    numSectorsY_ = static_cast<int>(sectorOffsetsY_.size());
}

int WikiTableViewerActivity::getEffectiveSectorClipW(int secX) const {
    int stickyW = (hasStickyLeftCol_ && !isClade_ && !rows_.empty() && rows_[0].size() > 1) ? stickyLeftWidth_ : 0;
    int availW = std::max(10, viewWidth_ - stickyW);
    if (secX >= 0 && secX < static_cast<int>(sectorClipWidth_.size())) {
        return std::min(availW, sectorClipWidth_[secX]);
    }
    return availW;
}

int WikiTableViewerActivity::getEffectiveSectorClipH(int secY) const {
    int stickyH = (hasStickyTopRow_ && !isClade_) ? stickyTopHeight_ : 0;
    int availH = std::max(10, viewHeight_ - stickyH);
    if (secY >= 0 && secY < static_cast<int>(sectorClipHeight_.size())) {
        return std::min(availH, sectorClipHeight_[secY]);
    }
    return availH;
}

void WikiTableViewerActivity::updateScrollForFocusedCell() {
    if (focusedRow_ < 0 || focusedRow_ >= static_cast<int>(cellLayouts_.size())) return;
    if (focusedCol_ < 0 || focusedCol_ >= static_cast<int>(cellLayouts_[focusedRow_].size())) return;
    while (focusedCol_ > 0 && cellLayouts_[focusedRow_][focusedCol_].width == 0) {
        focusedCol_--;
    }

    const auto& cell = cellLayouts_[focusedRow_][focusedCol_];
    const auto& data = rows_[focusedRow_][focusedCol_];
    int stickyW = (hasStickyLeftCol_ && !isClade_ && !rows_.empty() && rows_[0].size() > 1) ? stickyLeftWidth_ : 0;
    int stickyH = (hasStickyTopRow_ && !isClade_) ? stickyTopHeight_ : 0;

    int secX = std::clamp(currentSecX_, 0, std::max(0, numSectorsX_ - 1));
    if (!data.isFullRowBanner && cell.x >= stickyW && !sectorOffsetsX_.empty()) {
        int relX = cell.x - stickyW;
        int curOffX = sectorOffsetsX_[secX];
        int curClipW = getEffectiveSectorClipW(secX);
        if (relX < curOffX || relX + cell.width > curOffX + curClipW) {
            for (int sx = 0; sx < numSectorsX_; ++sx) {
                int ox = sectorOffsetsX_[sx];
                int cw = getEffectiveSectorClipW(sx);
                if (relX >= ox && relX < ox + cw) {
                    secX = sx;
                    break;
                }
            }
        }
    }

    int secY = std::clamp(currentSecY_, 0, std::max(0, numSectorsY_ - 1));
    if (cell.y >= stickyH && !sectorOffsetsY_.empty()) {
        int relY = cell.y - stickyH;
        int curOffY = sectorOffsetsY_[secY];
        int curClipH = getEffectiveSectorClipH(secY);
        if (relY + cell.height <= curOffY || relY >= curOffY + curClipH) {
            for (int sy = 0; sy < numSectorsY_; ++sy) {
                int oy = sectorOffsetsY_[sy];
                int ch = getEffectiveSectorClipH(sy);
                if (relY >= oy && relY < oy + ch) {
                    secY = sy;
                    break;
                }
                if (relY < oy + ch && relY + cell.height > oy) {
                    secY = sy;
                    break;
                }
            }
        }
    }

    currentSecX_ = std::clamp(secX, 0, std::max(0, numSectorsX_ - 1));
    currentSecY_ = std::clamp(secY, 0, std::max(0, numSectorsY_ - 1));
    scrollX_ = (currentSecX_ < static_cast<int>(sectorOffsetsX_.size())) ? sectorOffsetsX_[currentSecX_] : 0;
    scrollY_ = (currentSecY_ < static_cast<int>(sectorOffsetsY_.size())) ? sectorOffsetsY_[currentSecY_] : 0;
}

void WikiTableViewerActivity::loop() {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) { finish(); return; }

    auto activateFocusedLink = [&]() -> bool {
        if (focusedRow_ >= 0 && focusedRow_ < static_cast<int>(rows_.size()) &&
            focusedCol_ >= 0 && focusedCol_ < static_cast<int>(rows_[focusedRow_].size())) {
            const std::string& target = rows_[focusedRow_][focusedCol_].linkTarget;
            if (!target.empty()) {
                ActivityResult result;
                result.isCancelled = false;
                result.data = FilePathResult{target};
                setResult(std::move(result));
                finish();
                return true;
            }
        }
        return false;
    };

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        if (activateFocusedLink()) {
            return;
        }
    }

    int stickyH = (hasStickyTopRow_ && !isClade_) ? stickyTopHeight_ : 0;

    bool moved = false;
    bool sectorScrolled = false;

    auto stepDown = [&]() {
        bool scrolledInsideCell = false;
        if (focusedRow_ >= 0 && focusedRow_ < static_cast<int>(cellLayouts_.size()) &&
            focusedCol_ >= 0 && focusedCol_ < static_cast<int>(cellLayouts_[focusedRow_].size())) {
            const auto& cell = cellLayouts_[focusedRow_][focusedCol_];
            int relY = cell.y - stickyH;
            int curClipH = getEffectiveSectorClipH(currentSecY_);
            if (cell.y >= stickyH && relY + cell.height > scrollY_ + curClipH && currentSecY_ + 1 < numSectorsY_) {
                currentSecY_++;
                scrollY_ = sectorOffsetsY_[currentSecY_];
                sectorScrolled = true;
                scrolledInsideCell = true;
            }
        }
        if (!scrolledInsideCell) {
            if (focusedRow_ + 1 < static_cast<int>(rows_.size())) {
                focusedRow_++;
                moved = true;
            } else if (currentSecY_ + 1 < numSectorsY_) {
                currentSecY_++;
                scrollY_ = sectorOffsetsY_[currentSecY_];
                sectorScrolled = true;
            } else if (currentWindowIdx_ + 1 < windows_.size()) {
                int savedCol = focusedCol_;
                int savedSecX = currentSecX_;
                loadRowWindow(currentWindowIdx_ + 1);
                focusedRow_ = std::min(stickyTopRowCount_, std::max(0, static_cast<int>(rows_.size()) - 1));
                focusedCol_ = std::clamp(savedCol, 0, std::max(0, static_cast<int>(rows_[focusedRow_].size()) - 1));
                currentSecX_ = std::clamp(savedSecX, 0, std::max(0, numSectorsX_ - 1));
                currentSecY_ = 0;
                scrollX_ = (currentSecX_ < static_cast<int>(sectorOffsetsX_.size())) ? sectorOffsetsX_[currentSecX_] : 0;
                scrollY_ = (currentSecY_ < static_cast<int>(sectorOffsetsY_.size())) ? sectorOffsetsY_[currentSecY_] : 0;
                pageRedrawNeeded_ = true;
                sectorScrolled = true;
            }
        }
    };

    auto stepUp = [&]() {
        bool scrolledInsideCell = false;
        if (focusedRow_ >= 0 && focusedRow_ < static_cast<int>(cellLayouts_.size()) &&
            focusedCol_ >= 0 && focusedCol_ < static_cast<int>(cellLayouts_[focusedRow_].size())) {
            const auto& cell = cellLayouts_[focusedRow_][focusedCol_];
            int relY = cell.y - stickyH;
            if (cell.y >= stickyH && relY < scrollY_ && currentSecY_ > 0) {
                currentSecY_--;
                scrollY_ = sectorOffsetsY_[currentSecY_];
                sectorScrolled = true;
                scrolledInsideCell = true;
            }
        }
        if (!scrolledInsideCell) {
            int minDataRowInWin = (currentWindowIdx_ > 0) ? stickyTopRowCount_ : 0;
            if (focusedRow_ > minDataRowInWin) {
                focusedRow_--;
                moved = true;
            } else if (currentSecY_ > 0) {
                currentSecY_--;
                scrollY_ = sectorOffsetsY_[currentSecY_];
                sectorScrolled = true;
            } else if (currentWindowIdx_ > 0) {
                int savedCol = focusedCol_;
                int savedSecX = currentSecX_;
                loadRowWindow(currentWindowIdx_ - 1);
                focusedRow_ = std::max(0, static_cast<int>(rows_.size()) - 1);
                focusedCol_ = std::clamp(savedCol, 0, std::max(0, static_cast<int>(rows_[focusedRow_].size()) - 1));
                currentSecX_ = std::clamp(savedSecX, 0, std::max(0, numSectorsX_ - 1));
                currentSecY_ = std::max(0, numSectorsY_ - 1);
                scrollX_ = (currentSecX_ < static_cast<int>(sectorOffsetsX_.size())) ? sectorOffsetsX_[currentSecX_] : 0;
                scrollY_ = (currentSecY_ < static_cast<int>(sectorOffsetsY_.size())) ? sectorOffsetsY_[currentSecY_] : 0;
                pageRedrawNeeded_ = true;
                sectorScrolled = true;
            }
        }
    };

    auto stepLeft = [&]() {
        int prevCol = focusedCol_ - 1;
        while (prevCol > 0 && focusedRow_ >= 0 && focusedRow_ < static_cast<int>(cellLayouts_.size()) &&
               prevCol < static_cast<int>(cellLayouts_[focusedRow_].size()) &&
               cellLayouts_[focusedRow_][prevCol].width == 0) {
            prevCol--;
        }
        if (prevCol >= 0) {
            focusedCol_ = prevCol;
            moved = true;
        } else if (currentSecX_ > 0) {
            currentSecX_--;
            scrollX_ = sectorOffsetsX_[currentSecX_];
            sectorScrolled = true;
        }
    };

    auto stepRight = [&]() {
        int nextCol = focusedCol_ + 1;
        while (focusedRow_ >= 0 && focusedRow_ < static_cast<int>(cellLayouts_.size()) &&
               nextCol < static_cast<int>(cellLayouts_[focusedRow_].size()) &&
               cellLayouts_[focusedRow_][nextCol].width == 0) {
            nextCol++;
        }
        if (focusedRow_ < static_cast<int>(rows_.size()) && nextCol < static_cast<int>(rows_[focusedRow_].size())) {
            focusedCol_ = nextCol;
            moved = true;
        } else if (currentSecX_ + 1 < numSectorsX_) {
            currentSecX_++;
            scrollX_ = sectorOffsetsX_[currentSecX_];
            sectorScrolled = true;
        }
    };

#if CROSSINK_APP_CAP_TOUCH
    if (mappedInput.hasTouchHardware()) {
        if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
            finish();
            return;
        }
        int tx = 0;
        int ty = 0;
        if (mappedInput.wasScreenTapped(tx, ty)) {
            const auto& metrics = UITheme::getInstance().getMetrics();
            if (ty < metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)) {
                return;
            }
            for (size_t r = 0; r < rows_.size(); ++r) {
                for (size_t c = 0; c < rows_[r].size(); ++c) {
                    int screenX = 0, screenY = 0;
                    int clipMinX = 0, clipMaxX = 0, clipMinY = 0, clipMaxY = 0;
                    if (!getCellScreenGeometry(r, c, screenX, screenY, clipMinX, clipMaxX, clipMinY, clipMaxY)) {
                        continue;
                    }
                    const auto& cell = cellLayouts_[r][c];
                    const int cellW = (!isClade_ && !rows_[r].empty() && rows_[r][0].isFullRowBanner)
                                          ? (clipMaxX - clipMinX)
                                          : cell.width;
                    const int bx = std::max(clipMinX, screenX);
                    const int by = std::max(clipMinY, screenY);
                    const int br = std::min(clipMaxX, screenX + cellW);
                    const int bb = std::min(clipMaxY, screenY + cell.height);
                    if (tx >= bx && tx < br && ty >= by && ty < bb) {
                        focusedRow_ = static_cast<int>(r);
                        focusedCol_ = static_cast<int>(c);
                        if (activateFocusedLink()) {
                            return;
                        }
                        return;
                    }
                }
            }
        } else {
            const int stickyW = (!isClade_ && hasStickyLeftCol_ && !rows_.empty() && rows_[0].size() > 1) ? stickyLeftWidth_ : 0;
            const size_t minScrollCol = (stickyW > 0) ? 1 : 0;
            const size_t minScrollRow = (!isClade_ && hasStickyTopRow_) ? static_cast<size_t>(stickyTopRowCount_) : 0;
            const auto swipe = mappedInput.wasSwipe();
            if (swipe == MappedInputManager::SwipeDir::Left) {
                if (currentSecX_ + 1 < numSectorsX_) {
                    currentSecX_++;
                    scrollX_ = sectorOffsetsX_[currentSecX_];
                    if (focusedRow_ >= 0 && focusedRow_ < static_cast<int>(cellLayouts_.size())) {
                        for (size_t c = minScrollCol; c < cellLayouts_[focusedRow_].size(); ++c) {
                            const auto& cell = cellLayouts_[focusedRow_][c];
                            if (cell.width > 0 && (cell.x - stickyW) >= scrollX_) {
                                focusedCol_ = static_cast<int>(c);
                                break;
                            }
                        }
                    }
                    sectorScrolled = true;
                } else {
                    stepRight();
                }
            } else if (swipe == MappedInputManager::SwipeDir::Right) {
                if (currentSecX_ > 0) {
                    currentSecX_--;
                    scrollX_ = sectorOffsetsX_[currentSecX_];
                    if (focusedRow_ >= 0 && focusedRow_ < static_cast<int>(cellLayouts_.size())) {
                        for (size_t c = minScrollCol; c < cellLayouts_[focusedRow_].size(); ++c) {
                            const auto& cell = cellLayouts_[focusedRow_][c];
                            if (cell.width > 0 && (cell.x - stickyW) >= scrollX_) {
                                focusedCol_ = static_cast<int>(c);
                                break;
                            }
                        }
                    }
                    sectorScrolled = true;
                } else {
                    stepLeft();
                }
            } else if (swipe == MappedInputManager::SwipeDir::Up) {
                if (currentSecY_ + 1 < numSectorsY_) {
                    currentSecY_++;
                    scrollY_ = sectorOffsetsY_[currentSecY_];
                    for (size_t r = minScrollRow; r < cellLayouts_.size(); ++r) {
                        if (focusedCol_ >= 0 && focusedCol_ < static_cast<int>(cellLayouts_[r].size())) {
                            const auto& cell = cellLayouts_[r][focusedCol_];
                            if (cell.height > 0 && (cell.y - stickyH) >= scrollY_) {
                                focusedRow_ = static_cast<int>(r);
                                break;
                            }
                        }
                    }
                    sectorScrolled = true;
                } else if (currentWindowIdx_ + 1 < windows_.size()) {
                    int savedCol = focusedCol_;
                    int savedSecX = currentSecX_;
                    loadRowWindow(currentWindowIdx_ + 1);
                    focusedRow_ = std::min(stickyTopRowCount_, std::max(0, static_cast<int>(rows_.size()) - 1));
                    focusedCol_ = std::clamp(savedCol, 0, std::max(0, static_cast<int>(rows_[focusedRow_].size()) - 1));
                    currentSecX_ = std::clamp(savedSecX, 0, std::max(0, numSectorsX_ - 1));
                    currentSecY_ = 0;
                    scrollX_ = (currentSecX_ < static_cast<int>(sectorOffsetsX_.size())) ? sectorOffsetsX_[currentSecX_] : 0;
                    scrollY_ = (currentSecY_ < static_cast<int>(sectorOffsetsY_.size())) ? sectorOffsetsY_[currentSecY_] : 0;
                    pageRedrawNeeded_ = true;
                    sectorScrolled = true;
                } else {
                    stepDown();
                }
            } else if (swipe == MappedInputManager::SwipeDir::Down) {
                if (currentSecY_ > 0) {
                    currentSecY_--;
                    scrollY_ = sectorOffsetsY_[currentSecY_];
                    for (size_t r = minScrollRow; r < cellLayouts_.size(); ++r) {
                        if (focusedCol_ >= 0 && focusedCol_ < static_cast<int>(cellLayouts_[r].size())) {
                            const auto& cell = cellLayouts_[r][focusedCol_];
                            if (cell.height > 0 && (cell.y - stickyH) >= scrollY_) {
                                focusedRow_ = static_cast<int>(r);
                                break;
                            }
                        }
                    }
                    sectorScrolled = true;
                } else if (currentWindowIdx_ > 0) {
                    int savedCol = focusedCol_;
                    int savedSecX = currentSecX_;
                    loadRowWindow(currentWindowIdx_ - 1);
                    focusedRow_ = std::max(0, static_cast<int>(rows_.size()) - 1);
                    focusedCol_ = std::clamp(savedCol, 0, std::max(0, static_cast<int>(rows_[focusedRow_].size()) - 1));
                    currentSecX_ = std::clamp(savedSecX, 0, std::max(0, numSectorsX_ - 1));
                    currentSecY_ = std::max(0, numSectorsY_ - 1);
                    scrollX_ = (currentSecX_ < static_cast<int>(sectorOffsetsX_.size())) ? sectorOffsetsX_[currentSecX_] : 0;
                    scrollY_ = (currentSecY_ < static_cast<int>(sectorOffsetsY_.size())) ? sectorOffsetsY_[currentSecY_] : 0;
                    pageRedrawNeeded_ = true;
                    sectorScrolled = true;
                } else {
                    stepUp();
                }
            }
        }
    }
#endif

    if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
        mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
        stepDown();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::PageBack)) {
        stepUp();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
        stepLeft();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
        stepRight();
    }

    if (moved) {
        if (focusedCol_ >= static_cast<int>(rows_[focusedRow_].size())) {
            focusedCol_ = std::max(0, static_cast<int>(rows_[focusedRow_].size()) - 1);
        }
        updateScrollForFocusedCell();
    }

    if (moved || sectorScrolled) {
        if (scrollX_ != renderedScrollX_ || scrollY_ != renderedScrollY_ || pageRedrawNeeded_) {
            pendingImageRender_ = false;
            imageStage2Triggered_ = false;
            pendingImages_.clear();
        } else if (pendingImageRender_) {
            imageStage2Triggered_ = false;
            stage1RenderCompleteMs_ = millis();
        }
        requestUpdate();
        return;
    }

    if (pendingImageRender_) {
        const bool anyNavActive =
            mappedInput.isPressed(MappedInputManager::Button::PageBack) ||
            mappedInput.isPressed(MappedInputManager::Button::PageForward) ||
            mappedInput.isPressed(MappedInputManager::Button::Up) ||
            mappedInput.isPressed(MappedInputManager::Button::Down) ||
            mappedInput.isPressed(MappedInputManager::Button::Left) ||
            mappedInput.isPressed(MappedInputManager::Button::Right) ||
            mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
            mappedInput.isPressed(MappedInputManager::Button::Back);
        if (anyNavActive) {
            stage1RenderCompleteMs_ = millis();
        } else if (!imageStage2Triggered_ &&
                   (millis() - stage1RenderCompleteMs_) >= IMAGE_DECODE_DEBOUNCE_MS) {
            imageStage2Triggered_ = true;
            requestUpdate();
        }
    }
}

static std::vector<std::pair<int, uint32_t>> getCladePrefixStems(GfxRenderer& renderer, int fontId, const std::string& line) {
    std::vector<std::pair<int, uint32_t>> stems;
    int relX = 0;
    const char* p = line.c_str();
    while (*p) {
        uint32_t cp = 0;
        int len = 0;
        const uint8_t* u8p = reinterpret_cast<const uint8_t*>(p);
        unsigned char c = u8p[0];
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = ((c & 0x1F) << 6) | (u8p[1] & 0x3F); len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = ((c & 0x0F) << 12) | ((u8p[1] & 0x3F) << 6) | (u8p[2] & 0x3F); len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = ((c & 0x07) << 18) | ((u8p[1] & 0x3F) << 12) | ((u8p[2] & 0x3F) << 6) | (u8p[3] & 0x3F); len = 4; }
        else { len = 1; }

        if (len <= 0) break;

        if (cp == ' ') {
            relX += renderer.getTextAdvanceX(fontId, " ", EpdFontFamily::REGULAR);
        } else if (cp >= 0x2500 && cp <= 0x257F) {
            if (cp == 0x2502 || cp == 0x251C || cp == 0x2514) {
                stems.push_back({relX, cp});
            }
            relX += 16;
        } else {
            break;
        }
        p += len;
    }
    return stems;
}

bool WikiTableViewerActivity::getCellScreenGeometry(size_t r, size_t c, int& screenX, int& screenY,
                                                    int& clipMinX, int& clipMaxX, int& clipMinY, int& clipMaxY) const {
    if (r >= cellLayouts_.size() || c >= cellLayouts_[r].size()) return false;
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int headerH = metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput);
    const int startX = 16;
    const int renderStartY = headerH + 8;
    const auto& cell = cellLayouts_[r][c];
    if (cell.width <= 0) return false;

    if (isClade_) {
        int effH = getEffectiveSectorClipH(currentSecY_);
        screenX = startX + cell.x - scrollX_;
        screenY = renderStartY + cell.y - scrollY_;
        clipMinX = startX;
        clipMaxX = startX + viewWidth_;
        clipMinY = renderStartY;
        clipMaxY = renderStartY + effH;
    } else {
        int stickyW = (hasStickyLeftCol_ && !rows_.empty() && rows_[0].size() > 1) ? stickyLeftWidth_ : 0;
        int stickyH = hasStickyTopRow_ ? stickyTopHeight_ : 0;
        int effW = getEffectiveSectorClipW(currentSecX_);
        int effH = getEffectiveSectorClipH(currentSecY_);
        size_t startR = hasStickyTopRow_ ? static_cast<size_t>(stickyTopRowCount_) : 0;
        size_t startC = (hasStickyLeftCol_ && !rows_.empty() && rows_[0].size() > 1) ? 1 : 0;
        bool isBanner = (!rows_[r].empty() && rows_[r][0].isFullRowBanner);

        if (isBanner) {
            screenX = startX;
            screenY = (r < startR) ? (renderStartY + cell.y) : (renderStartY + stickyH + (cell.y - stickyH) - scrollY_);
            clipMinX = startX;
            clipMaxX = startX + stickyW + effW;
            clipMinY = (r < startR) ? renderStartY : (renderStartY + stickyH);
            clipMaxY = (r < startR) ? (renderStartY + stickyH) : (renderStartY + stickyH + effH);
        } else if (r < startR && c < startC) {
            screenX = startX;
            screenY = renderStartY + cell.y;
            clipMinX = startX;
            clipMaxX = startX + stickyW;
            clipMinY = renderStartY;
            clipMaxY = renderStartY + stickyH;
        } else if (r < startR && c >= startC) {
            screenX = startX + stickyW + (cell.x - stickyW) - scrollX_;
            screenY = renderStartY + cell.y;
            clipMinX = startX + stickyW;
            clipMaxX = startX + stickyW + effW;
            clipMinY = renderStartY;
            clipMaxY = renderStartY + stickyH;
        } else if (r >= startR && c < startC) {
            screenX = startX;
            screenY = renderStartY + stickyH + (cell.y - stickyH) - scrollY_;
            clipMinX = startX;
            clipMaxX = startX + stickyW;
            clipMinY = renderStartY + stickyH;
            clipMaxY = renderStartY + stickyH + effH;
        } else {
            screenX = startX + stickyW + (cell.x - stickyW) - scrollX_;
            screenY = renderStartY + stickyH + (cell.y - stickyH) - scrollY_;
            clipMinX = startX + stickyW;
            clipMaxX = startX + stickyW + effW;
            clipMinY = renderStartY + stickyH;
            clipMaxY = renderStartY + stickyH + effH;
        }
    }

    int cellW = (!isClade_ && !rows_[r].empty() && rows_[r][0].isFullRowBanner) ? (clipMaxX - clipMinX) : cell.width;
    int bx = std::max(clipMinX, screenX);
    int by = std::max(clipMinY, screenY);
    int br = std::min(clipMaxX, screenX + cellW);
    int bb = std::min(clipMaxY, screenY + cell.height);
    return (br > bx && bb > by);
}

void WikiTableViewerActivity::renderPendingImages() {
    pendingImageRender_ = false;
    imageStage2Triggered_ = false;
    if (pendingImages_.empty()) {
        return;
    }

    const auto& metrics = UITheme::getInstance().getMetrics();
    const int screenW = renderer.getScreenWidth();
    const int screenH = renderer.getScreenHeight();
    const int headerH = metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput);
    const int startX = 16;
    const int renderStartY = headerH + 8;

    int minX = screenW;
    int minY = screenH;
    int maxX = 0;
    int maxY = 0;

    for (const auto& img : pendingImages_) {
        if (img.imgSector == 0) continue;
        if (img.placeholderW > 0 && img.placeholderH > 0) {
            renderer.fillRect(img.placeholderX, img.placeholderY, img.placeholderW, img.placeholderH, false);
        }
        WikiJpeg::renderCenteredInBox(renderer, reader_, img.imgSector, img.imgOffsetInSector,
                                      img.screenX, img.cellW, img.screenX + layoutCfg_.cellPadX, img.screenY + layoutCfg_.cellPadY,
                                      img.clipMinX, img.clipMaxX, img.clipMinY, img.clipMaxY);

        int bx = std::max<int>(img.clipMinX, img.screenX);
        int by = std::max<int>(img.clipMinY, img.screenY);
        int br = std::min<int>(img.clipMaxX, img.screenX + img.cellW);
        int bb = std::min<int>(img.clipMaxY, img.screenY + img.cellH);
        if (br > bx && bb > by) {
            if (!isClade_) {
                renderer.drawRect(bx, by, br - bx, bb - by, true);
                if (!mappedInput.hasTouchHardware() &&
                    static_cast<int>(img.row) == focusedRow_ && static_cast<int>(img.col) == focusedCol_) {
                    int fx = std::max<int>(bx + 1, img.screenX + 1);
                    int fy = std::max<int>(by + 1, img.screenY + 1);
                    int fr = std::min<int>(br - 1, img.screenX + img.cellW - 2);
                    int fb = std::min<int>(bb - 1, img.screenY + img.cellH - 2);
                    if (fr > fx && fb > fy) {
                        renderer.drawRect(fx, fy, fr - fx + 1, fb - fy + 1, true);
                        renderer.drawRect(fx + 1, fy + 1, fr - fx - 1, fb - fy - 1, true);
                    }
                }
            } else if (!mappedInput.hasTouchHardware() &&
                       static_cast<int>(img.row) == focusedRow_ && static_cast<int>(img.col) == focusedCol_) {
                int fx = std::max<int>(startX, img.screenX + 1);
                int fy = std::max<int>(renderStartY, img.screenY + 1);
                int fr = std::min<int>(startX + viewWidth_, img.screenX + img.cellW - 1);
                int fb = std::min<int>(renderStartY + viewHeight_, img.screenY + img.cellH - 1);
                if (fr > fx && fb > fy) {
                    renderer.drawRect(fx, fy, fr - fx, fb - fy, true);
                }
            }
            minX = std::min(minX, bx);
            minY = std::min(minY, by);
            maxX = std::max(maxX, br);
            maxY = std::max(maxY, bb);
        }
    }
    pendingImages_.clear();

    if (!isClade_) {
        int stickyW = (hasStickyLeftCol_ && !rows_.empty() && rows_[0].size() > 1) ? stickyLeftWidth_ : 0;
        int stickyH = hasStickyTopRow_ ? stickyTopHeight_ : 0;
        int effW = getEffectiveSectorClipW(currentSecX_);
        int effH = getEffectiveSectorClipH(currentSecY_);
        renderer.drawRect(startX, renderStartY, stickyW + effW, stickyH + effH, true);
        if (hasStickyLeftCol_ && stickyW > 0) {
            renderer.drawLine(startX + stickyW, renderStartY, startX + stickyW, renderStartY + stickyH + effH, true);
            renderer.drawLine(startX + stickyW + 1, renderStartY, startX + stickyW + 1, renderStartY + stickyH + effH, true);
        }
        if (hasStickyTopRow_ && stickyH > 0) {
            renderer.drawLine(startX, renderStartY + stickyH, startX + stickyW + effW, renderStartY + stickyH, true);
            renderer.drawLine(startX, renderStartY + stickyH + 1, startX + stickyW + effW, renderStartY + stickyH + 1, true);
        }
    }

    if (maxX > minX && maxY > minY) {
        int wx = std::max(0, (minX / 8) * 8);
        int wxEnd = std::min(screenW, ((maxX + 7) / 8) * 8);
        int wy = std::max(0, minY);
        int wyEnd = std::min(screenH, maxY);
        if (wxEnd > wx && wyEnd > wy) {
            renderer.displayWindow(wx, wy, wxEnd - wx, wyEnd - wy);
        }
    }
}

void WikiTableViewerActivity::renderFocusUpdate(int oldRow, int oldCol, int newRow, int newCol) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int screenW = renderer.getScreenWidth();
    const int screenH = renderer.getScreenHeight();
    const int headerH = metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput);
    const int startX = 16;
    const int renderStartY = headerH + 8;
    const int fontId = WikiFont::getBodyFontId();
    const int lineHeight = renderer.getLineHeight(fontId) + layoutCfg_.lineSpacing;

    int minX = screenW;
    int minY = screenH;
    int maxX = 0;
    int maxY = 0;

    auto measureAdvance = [&](const std::string& s, bool isBold) -> int {
        if (s.empty()) return 0;
        return formatter_ ? formatter_->getTextAdvance(s.c_str(), isBold)
                          : renderer.getTextAdvanceX(fontId, s.c_str(), isBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    };

    auto updateCellFocus = [&](int r, int c, bool focused) {
        if (r < 0 || c < 0) return;
        int screenX = 0, screenY = 0, clipMinX = 0, clipMaxX = 0, clipMinY = 0, clipMaxY = 0;
        if (!getCellScreenGeometry(static_cast<size_t>(r), static_cast<size_t>(c),
                                   screenX, screenY, clipMinX, clipMaxX, clipMinY, clipMaxY)) {
            return;
        }
        const auto& cell = cellLayouts_[r][c];
        const auto& data = rows_[r][c];
        int bx = std::max(clipMinX, screenX);
        int by = std::max(clipMinY, screenY);
        int br = std::min(clipMaxX, screenX + cell.width);
        int bb = std::min(clipMaxY, screenY + cell.height);
        if (br <= bx || bb <= by) return;

        if (isClade_) {
            int fx = std::max(startX, screenX + 1);
            int fy = std::max(renderStartY, screenY + 1);
            int fr = std::min(startX + viewWidth_, screenX + cell.width - 1);
            int fb = std::min(clipMaxY, screenY + cell.height - 1);
            if (fr > fx && fb > fy) {
                renderer.drawRect(fx, fy, fr - fx, fb - fy, focused);
            }
            if (!focused && !data.hasImage) {
                int textY = screenY + 2;
                int textPadX = 4;
                if (c == 0 && !cell.displayLines.empty()) {
                    int branchCenterY = textY + 8;
                    int cellTopY = std::max(renderStartY, screenY);
                    int cellBottomY = std::min(clipMaxY, screenY + cell.height);
                    auto currentStems = getCladePrefixStems(renderer, fontId, cell.displayLines[0]);
                    std::vector<std::pair<int, uint32_t>> prevStems;
                    if (r > 0 && !cellLayouts_[r - 1].empty() && !cellLayouts_[r - 1][0].displayLines.empty()) {
                        prevStems = getCladePrefixStems(renderer, fontId, cellLayouts_[r - 1][0].displayLines[0]);
                    }
                    for (const auto& stem : currentStems) {
                        int sx = screenX + textPadX + stem.first + 7;
                        uint32_t cp = stem.second;
                        bool hasTrunkAbove = false;
                        for (const auto& ps : prevStems) {
                            if (std::abs(ps.first - stem.first) <= 4 && (ps.second == 0x251C || ps.second == 0x2502)) {
                                hasTrunkAbove = true;
                                break;
                            }
                        }
                        if (cp == 0x2502) {
                            if (cellBottomY > cellTopY) {
                                renderer.drawLine(sx, cellTopY, sx, cellBottomY - 1, true);
                                renderer.drawLine(sx + 1, cellTopY, sx + 1, cellBottomY - 1, true);
                            }
                        } else if (cp == 0x251C) {
                            int yStart = std::max(cellTopY, branchCenterY);
                            if (cellBottomY > yStart) {
                                renderer.drawLine(sx, yStart, sx, cellBottomY - 1, true);
                                renderer.drawLine(sx + 1, yStart, sx + 1, cellBottomY - 1, true);
                            }
                            if (hasTrunkAbove) {
                                int yEnd = std::min(cellBottomY, branchCenterY);
                                if (yEnd > cellTopY) {
                                    renderer.drawLine(sx, cellTopY, sx, yEnd, true);
                                    renderer.drawLine(sx + 1, cellTopY, sx + 1, yEnd, true);
                                }
                            }
                        } else if (cp == 0x2514) {
                            if (hasTrunkAbove) {
                                int yEnd = std::min(cellBottomY, branchCenterY);
                                if (yEnd > cellTopY) {
                                    renderer.drawLine(sx, cellTopY, sx, yEnd, true);
                                    renderer.drawLine(sx + 1, cellTopY, sx + 1, yEnd, true);
                                }
                            }
                        }
                    }
                }
                for (const auto& line : cell.displayLines) {
                    if (textY >= renderStartY && textY + lineHeight <= clipMaxY) {
                        int drawX = screenX + textPadX;
                        if (formatter_) {
                            formatter_->drawText(drawX, textY, line.c_str(), true, data.isHeader);
                        } else {
                            renderer.drawText(fontId, drawX, textY, line.c_str(), true);
                        }
                        if (!data.linkTarget.empty() && !line.empty()) {
                            int lineW = measureAdvance(line, data.isHeader);
                            int ulY = std::min(clipMaxY - 1, textY + lineHeight - 3);
                            int ulX0 = std::max(clipMinX, drawX);
                            int ulX1 = std::min(clipMaxX - 1, drawX + lineW - 1);
                            if (ulX1 > ulX0 && ulY >= renderStartY && ulY < clipMaxY) {
                                renderer.drawLine(ulX0, ulY, ulX1, ulY, true);
                            }
                        }
                    }
                    textY += lineHeight;
                }
            }
        } else {
            int fx = std::max(bx + 1, screenX + 1);
            int fy = std::max(by + 1, screenY + 1);
            int fr = std::min(br - 1, screenX + cell.width - 2);
            int fb = std::min(bb - 1, screenY + cell.height - 2);
            if (fr > fx && fb > fy) {
                renderer.drawRect(fx, fy, fr - fx + 1, fb - fy + 1, focused);
                renderer.drawRect(fx + 1, fy + 1, fr - fx - 1, fb - fy - 1, focused);
            }
            renderer.drawRect(bx, by, br - bx, bb - by, true);
        }

        minX = std::min(minX, bx);
        minY = std::min(minY, by);
        maxX = std::max(maxX, br + 1);
        maxY = std::max(maxY, bb + 1);
    };

    updateCellFocus(oldRow, oldCol, false);
    updateCellFocus(newRow, newCol, true);

    if (!isClade_) {
        int stickyW = (hasStickyLeftCol_ && !rows_.empty() && rows_[0].size() > 1) ? stickyLeftWidth_ : 0;
        int stickyH = hasStickyTopRow_ ? stickyTopHeight_ : 0;
        int effW = getEffectiveSectorClipW(currentSecX_);
        int effH = getEffectiveSectorClipH(currentSecY_);
        renderer.drawRect(startX, renderStartY, stickyW + effW, stickyH + effH, true);
        if (hasStickyLeftCol_ && stickyW > 0) {
            renderer.drawLine(startX + stickyW, renderStartY, startX + stickyW, renderStartY + stickyH + effH, true);
            renderer.drawLine(startX + stickyW + 1, renderStartY, startX + stickyW + 1, renderStartY + stickyH + effH, true);
        }
        if (hasStickyTopRow_ && stickyH > 0) {
            renderer.drawLine(startX, renderStartY + stickyH, startX + stickyW + effW, renderStartY + stickyH, true);
            renderer.drawLine(startX, renderStartY + stickyH + 1, startX + stickyW + effW, renderStartY + stickyH + 1, true);
        }
    }

    const bool oldHasLink = (oldRow >= 0 && oldRow < static_cast<int>(rows_.size()) &&
                             oldCol >= 0 && oldCol < static_cast<int>(rows_[oldRow].size()) &&
                             !rows_[oldRow][oldCol].linkTarget.empty());
    const bool newHasLink = (newRow >= 0 && newRow < static_cast<int>(rows_.size()) &&
                             newCol >= 0 && newCol < static_cast<int>(rows_[newRow].size()) &&
                             !rows_[newRow][newCol].linkTarget.empty());

    renderedFocusedRow_ = newRow;
    renderedFocusedCol_ = newCol;

    if (maxX > minX && maxY > minY) {
        int wx = std::max(0, (minX / 8) * 8);
        int wxEnd = std::min(screenW, ((maxX + 7) / 8) * 8);
        int wy = std::max(0, minY);
        int wyEnd = std::min(screenH, maxY);
        if (wxEnd > wx && wyEnd > wy) {
            renderer.displayWindow(wx, wy, wxEnd - wx, wyEnd - wy);
        }
    }

    if (!mappedInput.hasTouchHardware() && oldHasLink != newHasLink) {
        const int bottomBarH = WikiGUI::WIKI_HINTS_HEIGHT + 14;
        const int bottomBarY = screenH - bottomBarH;
        renderer.fillRect(0, bottomBarY, screenW, bottomBarH, false);
        renderer.drawLine(0, bottomBarY, screenW - 1, bottomBarY, true);
        WikiGUI::drawDoubleButtonHints(renderer, {"Back", ""}, {newHasLink ? "Select link" : "", ""},
                                       {"Up/Down", ""}, {"Left/Right", ""});
        renderer.displayWindow(0, bottomBarY, screenW, bottomBarH);
    }
}

void WikiTableViewerActivity::render(RenderLock&& lock) {
    if (!pageRedrawNeeded_ && scrollX_ == renderedScrollX_ && scrollY_ == renderedScrollY_) {
        if (!mappedInput.hasTouchHardware() && (focusedRow_ != renderedFocusedRow_ || focusedCol_ != renderedFocusedCol_)) {
            renderFocusUpdate(renderedFocusedRow_, renderedFocusedCol_, focusedRow_, focusedCol_);
            if (pendingImageRender_ && imageStage2Triggered_) {
                renderPendingImages();
            }
            return;
        }
        renderedFocusedRow_ = focusedRow_;
        renderedFocusedCol_ = focusedCol_;
        if (pendingImageRender_ && imageStage2Triggered_) {
            renderPendingImages();
            return;
        }
        return;
    }

    pendingImages_.clear();
    pendingImageRender_ = false;
    imageStage2Triggered_ = false;

    renderer.clearScreen();
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int screenW = renderer.getScreenWidth();
    const int screenH = renderer.getScreenHeight();
    const int headerH = metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput);
    const int bottomBarH = mappedInput.hasTouchHardware() ? 10 : (WikiGUI::WIKI_HINTS_HEIGHT + 14);
    const int startX = 16;
    const int renderStartY = headerH + 8;
    const int fontId = WikiFont::getBodyFontId();
    const int lineHeight = renderer.getLineHeight(fontId) + layoutCfg_.lineSpacing;

    auto measureAdvance = [&](const std::string& s, bool isBold) -> int {
        if (s.empty()) return 0;
        return formatter_ ? formatter_->getTextAdvance(s.c_str(), isBold)
                          : renderer.getTextAdvanceX(fontId, s.c_str(), isBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    };

    const Rect headerRect = TouchHeaderBackButton::headerRect(renderer, mappedInput);
    const char* headerType = isClade_ ? "Cladogram" : "Table Viewer";
    if (mappedInput.hasTouchHardware()) {
        const int subtitleReserve = std::min(screenW / 2, renderer.getTextWidth(SMALL_FONT_ID, title_.c_str()) + 32);
        TouchHeaderBackButton::draw(renderer, headerRect, headerType, false, subtitleReserve, title_.c_str());
    } else {
        GUI.drawHeader(renderer, headerRect, headerType, title_.c_str());
    }

    // Draw Minimap: Fixed-size 24x14 viewport thumbnail centered in header
    const int globalTotalSecY = getNumSectorsY();
    const int globalCurSecY = getCurrentSecY();
    if (numSectorsX_ > 1 || globalTotalSecY > 1) {
        const int frameW = 24;
        const int frameH = 14;
        int mapX = (screenW - frameW) / 2;
        int mapY = mappedInput.hasTouchHardware()
                       ? (headerRect.y + 4)
                       : (metrics.topPadding + (metrics.headerHeight - frameH) / 2);
        if (mapY < 0) mapY = 0;

        renderer.drawRect(mapX, mapY, frameW, frameH, true);

        int thumbW = std::max(4, frameW / numSectorsX_);
        int thumbH = std::max(3, frameH / globalTotalSecY);
        int thumbX = mapX + (numSectorsX_ > 1 ? (currentSecX_ * (frameW - thumbW)) / (numSectorsX_ - 1) : 0);
        int thumbY = mapY + (globalTotalSecY > 1 ? (std::min(globalCurSecY, globalTotalSecY - 1) * (frameH - thumbH)) / (globalTotalSecY - 1) : 0);

        renderer.fillRect(thumbX, thumbY, thumbW, thumbH, true);
    }

    auto queueCellImagePlaceholder = [&](size_t r, size_t c, int screenX, int screenY,
                                         int clipMinX, int clipMaxX, int clipMinY, int clipMaxY) {
        const auto& cell = cellLayouts_[r][c];
        const auto& data = rows_[r][c];
        if (!data.hasImage || data.imgSector == 0) return;

        int iw = (data.imgWidth > 0) ? data.imgWidth : std::max(20, cell.width - 12);
        int ih = (data.imgHeight > 0) ? data.imgHeight : std::max(20, cell.height - 12);
        int imgDrawX = screenX + (cell.width / 2) - (iw / 2);
        if (imgDrawX < screenX + layoutCfg_.cellPadX) imgDrawX = screenX + layoutCfg_.cellPadX;
        int imgDrawY = screenY + layoutCfg_.cellPadY;

        int px = std::max(clipMinX, imgDrawX);
        int py = std::max(clipMinY, imgDrawY);
        int pr = std::min(clipMaxX, imgDrawX + iw);
        int pb = std::min(clipMaxY, imgDrawY + ih);
        if (pr > px && pb > py) {
            renderer.drawRect(px, py, pr - px, pb - py, true);
            pendingImages_.push_back(PendingCellImage{
                static_cast<uint16_t>(r),
                static_cast<uint16_t>(c),
                data.imgSector,
                data.imgOffsetInSector,
                static_cast<int16_t>(screenX),
                static_cast<int16_t>(screenY),
                static_cast<int16_t>(cell.width),
                static_cast<int16_t>(cell.height),
                static_cast<int16_t>(clipMinX),
                static_cast<int16_t>(clipMaxX),
                static_cast<int16_t>(clipMinY),
                static_cast<int16_t>(clipMaxY),
                static_cast<int16_t>(px),
                static_cast<int16_t>(py),
                static_cast<int16_t>(pr - px),
                static_cast<int16_t>(pb - py)});
        }
    };

    if (isClade_) {
        int effH = getEffectiveSectorClipH(currentSecY_);
        int cladeClipMaxY = renderStartY + effH;
        for (size_t r = 0; r < rows_.size(); ++r) {
            for (size_t c = 0; c < rows_[r].size(); ++c) {
                auto& cell = cellLayouts_[r][c];
                auto& data = rows_[r][c];

                int screenX = startX + cell.x - scrollX_;
                int screenY = renderStartY + cell.y - scrollY_;

                if (screenX + cell.width <= startX || screenX >= startX + viewWidth_) continue;
                if (screenY + cell.height <= renderStartY || screenY >= cladeClipMaxY) continue;

                if (!mappedInput.hasTouchHardware() &&
                    static_cast<int>(r) == focusedRow_ && static_cast<int>(c) == focusedCol_) {
                    int fx = std::max(startX, screenX + 1);
                    int fy = std::max(renderStartY, screenY + 1);
                    int fr = std::min(startX + viewWidth_, screenX + cell.width - 1);
                    int fb = std::min(cladeClipMaxY, screenY + cell.height - 1);
                    if (fr > fx && fb > fy) {
                        renderer.drawRect(fx, fy, fr - fx, fb - fy, true);
                    }
                }

                if (data.hasImage) {
                    int clipMinX = std::max(startX, screenX);
                    int clipMaxX = std::min(startX + viewWidth_, screenX + cell.width);
                    int clipMinY = std::max(renderStartY, screenY);
                    int clipMaxY = std::min(cladeClipMaxY, screenY + cell.height);
                    queueCellImagePlaceholder(r, c, screenX, screenY, clipMinX, clipMaxX, clipMinY, clipMaxY);
                } else {
                    int textY = screenY + 2;
                    int textPadX = 4;

                    if (c == 0 && !cell.displayLines.empty()) {
                        int branchCenterY = textY + 8;
                        int cellTopY = std::max(renderStartY, screenY);
                        int cellBottomY = std::min(cladeClipMaxY, screenY + cell.height);

                        auto currentStems = getCladePrefixStems(renderer, fontId, cell.displayLines[0]);
                        std::vector<std::pair<int, uint32_t>> prevStems;
                        if (r > 0 && !cellLayouts_[r - 1].empty() && !cellLayouts_[r - 1][0].displayLines.empty()) {
                            prevStems = getCladePrefixStems(renderer, fontId, cellLayouts_[r - 1][0].displayLines[0]);
                        }

                        for (const auto& stem : currentStems) {
                            int sx = screenX + textPadX + stem.first + 7;
                            uint32_t cp = stem.second;

                            bool hasTrunkAbove = false;
                            for (const auto& ps : prevStems) {
                                if (std::abs(ps.first - stem.first) <= 4 && (ps.second == 0x251C || ps.second == 0x2502)) {
                                    hasTrunkAbove = true;
                                    break;
                                }
                            }

                            if (cp == 0x2502) {
                                if (cellBottomY > cellTopY) {
                                    renderer.drawLine(sx, cellTopY, sx, cellBottomY - 1, true);
                                    renderer.drawLine(sx + 1, cellTopY, sx + 1, cellBottomY - 1, true);
                                }
                            } else if (cp == 0x251C) {
                                int yStart = std::max(cellTopY, branchCenterY);
                                if (cellBottomY > yStart) {
                                    renderer.drawLine(sx, yStart, sx, cellBottomY - 1, true);
                                    renderer.drawLine(sx + 1, yStart, sx + 1, cellBottomY - 1, true);
                                }
                                if (hasTrunkAbove) {
                                    int yEnd = std::min(cellBottomY, branchCenterY);
                                    if (yEnd > cellTopY) {
                                        renderer.drawLine(sx, cellTopY, sx, yEnd, true);
                                        renderer.drawLine(sx + 1, cellTopY, sx + 1, yEnd, true);
                                    }
                                }
                            } else if (cp == 0x2514) {
                                if (hasTrunkAbove) {
                                    int yEnd = std::min(cellBottomY, branchCenterY);
                                    if (yEnd > cellTopY) {
                                        renderer.drawLine(sx, cellTopY, sx, yEnd, true);
                                        renderer.drawLine(sx + 1, cellTopY, sx + 1, yEnd, true);
                                    }
                                }
                            }
                        }
                    }

                    for (const auto& line : cell.displayLines) {
                        if (textY >= renderStartY && textY + lineHeight <= cladeClipMaxY) {
                            int drawX = screenX + textPadX;
                            if (formatter_) {
                                formatter_->drawText(drawX, textY, line.c_str(), true, data.isHeader);
                            } else {
                                renderer.drawText(fontId, drawX, textY, line.c_str(), true);
                            }
                            if (!data.linkTarget.empty() && !line.empty()) {
                                int lineW = measureAdvance(line, data.isHeader);
                                int ulY = std::min(cladeClipMaxY - 1, textY + lineHeight - 3);
                                int ulX0 = std::max(startX, drawX);
                                int ulX1 = std::min(startX + viewWidth_ - 1, drawX + lineW - 1);
                                if (ulX1 > ulX0 && ulY >= renderStartY && ulY < cladeClipMaxY) {
                                    renderer.drawLine(ulX0, ulY, ulX1, ulY, true);
                                }
                            }
                        }
                        textY += lineHeight;
                    }
                }
            }
        }
    } else {
        // Multi-Column Data Table: 4-Quadrant Rendering
        int stickyW = (hasStickyLeftCol_ && rows_.size() > 0 && rows_[0].size() > 1) ? stickyLeftWidth_ : 0;
        int stickyH = hasStickyTopRow_ ? stickyTopHeight_ : 0;
        int effW = getEffectiveSectorClipW(currentSecX_);
        int effH = getEffectiveSectorClipH(currentSecY_);
        size_t startR = hasStickyTopRow_ ? static_cast<size_t>(stickyTopRowCount_) : 0;
        size_t startC = (hasStickyLeftCol_ && rows_.size() > 0 && rows_[0].size() > 1) ? 1 : 0;

        auto renderCell = [&](size_t r, size_t c, int screenX, int screenY,
                              int clipMinX, int clipMaxX, int clipMinY, int clipMaxY) {
            auto& cell = cellLayouts_[r][c];
            auto& data = rows_[r][c];
            int cellW = data.isFullRowBanner ? (stickyW + effW) : cell.width;
            if (cellW <= 0) return;

            int cellH = cell.height;
            if (r < startR && !data.isRowSpanCont && data.rowSpan > 1) {
                int spannedStickyH = 0;
                for (size_t sr = r; sr < std::min<size_t>(startR, r + data.rowSpan); ++sr) {
                    spannedStickyH += cellLayouts_[sr][c].height;
                }
                if (spannedStickyH > cellH) cellH = spannedStickyH;
            }

            int bx = std::max(clipMinX, screenX);
            int by = std::max(clipMinY, screenY);
            int br = std::min(clipMaxX, screenX + cellW);
            int bb = std::min(clipMaxY, screenY + cellH);
            if (br <= bx || bb <= by) return;

            if (data.isRowSpanCont && r < startR && data.rowSpanRootRow >= 0 &&
                static_cast<size_t>(data.rowSpanRootRow) < startR) {
                return; // Drawn as part of merged sticky root cell above
            }

            renderer.fillRect(bx, by, br - bx, bb - by, false);
            renderer.drawRect(bx, by, br - bx, bb - by, true);

            bool rootVisibleInSector = false;
            if (data.isRowSpanCont && data.rowSpanRootRow >= 0 &&
                static_cast<size_t>(data.rowSpanRootRow) < cellLayouts_.size() &&
                !cellLayouts_[data.rowSpanRootRow].empty()) {
                if (hasStickyTopRow_ && static_cast<size_t>(data.rowSpanRootRow) < startR) {
                    rootVisibleInSector = true;
                } else {
                    int rootRelY = cellLayouts_[data.rowSpanRootRow][0].y - stickyH;
                    if (rootRelY >= scrollY_) {
                        rootVisibleInSector = true;
                    }
                }
            }
            if (data.isRowSpanCont && rootVisibleInSector && by == screenY && br > bx + 2) {
                renderer.drawLine(bx + 1, by, br - 2, by, false);
            }

            if (!mappedInput.hasTouchHardware() &&
                static_cast<int>(r) == focusedRow_ && static_cast<int>(c) == focusedCol_) {
                int fx = std::max(bx + 1, screenX + 1);
                int fy = std::max(by + 1, screenY + 1);
                int fr = std::min(br - 1, screenX + cellW - 2);
                int fb = std::min(bb - 1, screenY + cellH - 2);
                if (fr > fx && fb > fy) {
                    renderer.drawRect(fx, fy, fr - fx + 1, fb - fy + 1, true);
                    renderer.drawRect(fx + 1, fy + 1, fr - fx - 1, fb - fy - 1, true);
                }
            }

            if (data.hasImage) {
                queueCellImagePlaceholder(r, c, screenX, screenY, bx, br, by, bb);
            } else if (!data.isRowSpanCont || !rootVisibleInSector) {
                bool isSingleSymbol = (cell.displayLines.size() == 1 && isCheckOrCrossSymbol(cell.displayLines[0]));
                int textY = isSingleSymbol
                                ? (screenY + std::max(layoutCfg_.cellPadY, (cellH - lineHeight) / 2))
                                : (screenY + layoutCfg_.cellPadY);
                int textPadX = layoutCfg_.cellPadX;
                for (const auto& line : cell.displayLines) {
                    if (textY >= clipMinY && textY + lineHeight <= clipMaxY) {
                        int lineAdv = measureAdvance(line, data.isHeader);
                        int drawX = isSingleSymbol
                                        ? (screenX + std::max(textPadX, (cellW - lineAdv) / 2))
                                        : (screenX + textPadX);
                        if (drawX >= clipMinX && drawX < clipMaxX) {
                            if (formatter_) {
                                formatter_->drawText(drawX, textY, line.c_str(), true, data.isHeader);
                            } else {
                                renderer.drawText(fontId, drawX, textY, line.c_str(), true,
                                                  data.isHeader ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
                            }
                            if (!data.linkTarget.empty() && !line.empty() && !isSingleSymbol) {
                                int ulY = std::min(clipMaxY - 1, textY + lineHeight - 3);
                                int ulX0 = std::max(clipMinX, drawX);
                                int ulX1 = std::min(clipMaxX - 1, drawX + lineAdv - 1);
                                if (ulX1 > ulX0 && ulY >= clipMinY && ulY < clipMaxY) {
                                    renderer.drawLine(ulX0, ulY, ulX1, ulY, true);
                                }
                            }
                        }
                    }
                    textY += lineHeight;
                }
            }
        };

        // Quadrant 1: Scrolled Data Grid (r >= startR, c >= startC)
        int gridClipMinX = startX + stickyW;
        int gridClipMaxX = startX + stickyW + effW;
        int gridClipMinY = renderStartY + stickyH;
        int gridClipMaxY = renderStartY + stickyH + effH;

        for (size_t r = startR; r < rows_.size(); ++r) {
            if (!rows_[r].empty() && rows_[r][0].isFullRowBanner) continue;
            for (size_t c = startC; c < rows_[r].size(); ++c) {
                int sx = startX + stickyW + (cellLayouts_[r][c].x - stickyW) - scrollX_;
                int sy = renderStartY + stickyH + (cellLayouts_[r][c].y - stickyH) - scrollY_;
                renderCell(r, c, sx, sy, gridClipMinX, gridClipMaxX, gridClipMinY, gridClipMaxY);
            }
        }

        // Quadrant 2: Sticky Left Column (r >= startR, c = 0)
        if (hasStickyLeftCol_ && stickyW > 0) {
            renderer.fillRect(startX, renderStartY + stickyH, stickyW, effH, false);
            int leftClipMinX = startX;
            int leftClipMaxX = startX + stickyW;
            int leftClipMinY = renderStartY + stickyH;
            int leftClipMaxY = renderStartY + stickyH + effH;
            for (size_t r = startR; r < rows_.size(); ++r) {
                if (!rows_[r].empty() && !rows_[r][0].isFullRowBanner) {
                    int sx = startX;
                    int sy = renderStartY + stickyH + (cellLayouts_[r][0].y - stickyH) - scrollY_;
                    renderCell(r, 0, sx, sy, leftClipMinX, leftClipMaxX, leftClipMinY, leftClipMaxY);
                }
            }
        }

        // Quadrant 3: Sticky Top Header (r < startR, c >= startC)
        if (hasStickyTopRow_ && stickyH > 0 && !rows_.empty()) {
            renderer.fillRect(startX + stickyW, renderStartY, effW, stickyH, false);
            int topClipMinX = startX + stickyW;
            int topClipMaxX = startX + stickyW + effW;
            int topClipMinY = renderStartY;
            int topClipMaxY = renderStartY + stickyH;
            for (size_t r = 0; r < startR && r < rows_.size(); ++r) {
                if (!rows_[r].empty() && !rows_[r][0].isFullRowBanner) {
                    for (size_t c = startC; c < rows_[r].size(); ++c) {
                        int sx = startX + stickyW + (cellLayouts_[r][c].x - stickyW) - scrollX_;
                        int sy = renderStartY + cellLayouts_[r][c].y;
                        renderCell(r, c, sx, sy, topClipMinX, topClipMaxX, topClipMinY, topClipMaxY);
                    }
                }
            }
        }

        // Quadrant 4: Top-Left Frozen Corner (r < startR, c = 0)
        if (hasStickyTopRow_ && hasStickyLeftCol_ && stickyW > 0 && stickyH > 0 && !rows_.empty()) {
            renderer.fillRect(startX, renderStartY, stickyW, stickyH, false);
            int cornerClipMinX = startX;
            int cornerClipMaxX = startX + stickyW;
            int cornerClipMinY = renderStartY;
            int cornerClipMaxY = renderStartY + stickyH;
            for (size_t r = 0; r < startR && r < rows_.size(); ++r) {
                if (!rows_[r].empty() && !rows_[r][0].isFullRowBanner) {
                    renderCell(r, 0, startX, renderStartY + cellLayouts_[r][0].y,
                               cornerClipMinX, cornerClipMaxX, cornerClipMinY, cornerClipMaxY);
                }
            }
        }

        // Clean dividers & outer borders
        renderer.drawRect(startX, renderStartY, stickyW + effW, stickyH + effH, true);
        if (hasStickyLeftCol_ && stickyW > 0) {
            renderer.drawLine(startX + stickyW, renderStartY, startX + stickyW, renderStartY + stickyH + effH, true);
            renderer.drawLine(startX + stickyW + 1, renderStartY, startX + stickyW + 1, renderStartY + stickyH + effH, true);
        }

        // Full-row banners span across both Sticky Left and Scrolled Grid without a vertical divider line
        for (size_t r = 0; r < rows_.size(); ++r) {
            if (!rows_[r].empty() && rows_[r][0].isFullRowBanner) {
                if (r < startR) {
                    renderCell(r, 0, startX, renderStartY + cellLayouts_[r][0].y,
                               startX, startX + stickyW + effW, renderStartY, renderStartY + stickyH);
                } else {
                    int sy = renderStartY + stickyH + (cellLayouts_[r][0].y - stickyH) - scrollY_;
                    renderCell(r, 0, startX, sy, startX, startX + stickyW + effW, renderStartY + stickyH, renderStartY + stickyH + effH);
                }
            }
        }

        if (hasStickyTopRow_ && stickyH > 0) {
            renderer.drawLine(startX, renderStartY + stickyH, startX + stickyW + effW, renderStartY + stickyH, true);
            renderer.drawLine(startX, renderStartY + stickyH + 1, startX + stickyW + effW, renderStartY + stickyH + 1, true);
        }

        renderer.fillRect(0, renderStartY, startX, viewHeight_, false);
        renderer.fillRect(startX + stickyW + effW, renderStartY, screenW - (startX + stickyW + effW), viewHeight_, false);
    }

    if (!mappedInput.hasTouchHardware()) {
        int bottomBarY = screenH - bottomBarH;
        renderer.fillRect(0, bottomBarY, screenW, bottomBarH, false);
        renderer.drawLine(0, bottomBarY, screenW - 1, bottomBarY, true);
        bool focusedHasLink = (focusedRow_ >= 0 && focusedRow_ < static_cast<int>(rows_.size()) &&
                               focusedCol_ >= 0 && focusedCol_ < static_cast<int>(rows_[focusedRow_].size()) &&
                               !rows_[focusedRow_][focusedCol_].linkTarget.empty());
        WikiGUI::drawDoubleButtonHints(renderer, {"Back", ""}, {focusedHasLink ? "Select link" : "", ""},
                                       {"Up/Down", ""}, {"Left/Right", ""});
    }

    pageRedrawNeeded_ = false;
    renderedScrollX_ = scrollX_;
    renderedScrollY_ = scrollY_;
    renderedFocusedRow_ = focusedRow_;
    renderedFocusedCol_ = focusedCol_;

    renderer.displayBuffer(HalDisplay::FAST_REFRESH);

    if (!pendingImages_.empty()) {
        pendingImageRender_ = true;
        imageStage2Triggered_ = false;
        stage1RenderCompleteMs_ = millis();
    }
}
