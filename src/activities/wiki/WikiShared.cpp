#include "WikiShared.h"

#include <FS.h>
#define File fs::File
#include <JPEGDEC.h>
#undef File
#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include "ImageToneMapper.h"
#include "SdCardFontSystem.h"
#include "WikiGUI.h"
#include "WikiSearchEngine.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"

static const char* TAG = "WikiShared";
static constexpr const char* WIKI_CFG_PATH = "/.crosspoint/wiki.cfg";

namespace {
uint32_t* s_alignedBuffer = nullptr;
size_t s_allocatedBytes = 0;
int s_refCount = 0;
int s_busyCount = 0;
uint32_t s_generation = 1;
WikiConfigData* s_config = nullptr;
GfxRenderer* s_renderer = nullptr;

uint32_t* allocateScratchBufferWithReclaim(size_t& outAllocatedBytes) {
  outAllocatedBytes = 0;
  auto tryAlloc = [&](size_t bytes) -> uint32_t* {
    uint32_t* ptr = new (std::nothrow) uint32_t[(bytes + 3) / 4];
    if (ptr) {
      outAllocatedBytes = bytes;
    }
    return ptr;
  };

  if (uint32_t* buf = tryAlloc(WikiScratchBuffer::BUFFER_SIZE)) {
    return buf;
  }
  if (s_renderer) {
    if (auto* fcm = s_renderer->getFontCacheManager()) {
      fcm->clearCache();
      fcm->releaseSdFontCaches();
    }
    if (uint32_t* buf = tryAlloc(WikiScratchBuffer::BUFFER_SIZE)) {
      return buf;
    }
    sdFontSystem.releaseLoadedFont(*s_renderer);
    sdFontSystem.releaseRegistry();
    if (uint32_t* buf = tryAlloc(WikiScratchBuffer::BUFFER_SIZE)) {
      return buf;
    }
  }
  return tryAlloc(WikiScratchBuffer::MIN_BUFFER_SIZE);
}

void parseSettingsJsonMigration(WikiConfigData& cfg) {
  String json = Storage.readFile("/.crosspoint/settings.json");
  if (json.isEmpty()) return;
  const char* s = json.c_str();

  auto extractUint = [&](const char* key, uint32_t& outVal) -> bool {
    const char* p = std::strstr(s, key);
    if (!p) return false;
    p = std::strchr(p, ':');
    if (!p) return false;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p < '0' || *p > '9') return false;
    outVal = static_cast<uint32_t>(std::strtoul(p, nullptr, 10));
    return true;
  };

  const char* pathKey = std::strstr(s, "\"wikiImagePath\"");
  if (pathKey) {
    const char* colon = std::strchr(pathKey, ':');
    const char* q1 = colon ? std::strchr(colon, '\"') : nullptr;
    const char* q2 = q1 ? std::strchr(q1 + 1, '\"') : nullptr;
    if (q1 && q2 && q2 > q1 + 1) {
      size_t len = std::min<size_t>(q2 - (q1 + 1), sizeof(cfg.imagePath) - 1);
      std::memcpy(cfg.imagePath, q1 + 1, len);
      cfg.imagePath[len] = '\0';
    }
  }

  uint32_t idxSec = UINT32_MAX;
  if (extractUint("\"wikiIndexSector\"", idxSec) && idxSec != UINT32_MAX && idxSec > 0) {
    cfg.indexSector = idxSec;
    uint32_t chunksSec = 0;
    if (extractUint("\"wikiChunksSector\"", chunksSec) && chunksSec != UINT32_MAX) {
      cfg.chunksSector = chunksSec;
    }
    uint32_t fontSec = 0;
    if (extractUint("\"wikiFontSector\"", fontSec) && fontSec != UINT32_MAX) {
      cfg.fontSector = fontSec;
    }
    uint32_t hasImg = 1;
    if (extractUint("\"wikiHasImages\"", hasImg)) {
      cfg.hasImages = hasImg ? 1 : 0;
    }
  }
}

void parseRecentLine(const char* val, std::vector<WikiRecentEntry>& outList) {
  if (outList.size() >= WikiConfigData::MAX_RECENT_ITEMS) return;
  const char* bar = std::strchr(val, '|');
  if (!bar || bar == val || *(bar + 1) == '\0') return;
  WikiRecentEntry entry{};
  entry.offset = std::strtoull(val, nullptr, 10);
  std::strncpy(entry.title, bar + 1, sizeof(entry.title) - 1);
  entry.title[sizeof(entry.title) - 1] = '\0';
  if (entry.title[0] != '\0') {
    outList.push_back(entry);
  }
}

void formatDisplayTitle(const char* rawTitle, char* outBuf, size_t outSize) {
  if (!rawTitle || outSize == 0) return;
  std::strncpy(outBuf, rawTitle, outSize - 1);
  outBuf[outSize - 1] = '\0';

  bool hasUpper = false;
  for (size_t i = 0; outBuf[i] != '\0'; ++i) {
    if (outBuf[i] == '_') outBuf[i] = ' ';
    if (outBuf[i] >= 'A' && outBuf[i] <= 'Z') hasUpper = true;
  }

  if (!hasUpper) {
    bool newWord = true;
    for (size_t i = 0; outBuf[i] != '\0'; ++i) {
      if (newWord && outBuf[i] >= 'a' && outBuf[i] <= 'z') {
        outBuf[i] = static_cast<char>(outBuf[i] - 'a' + 'A');
        newWord = false;
      } else if (outBuf[i] == ' ' || outBuf[i] == '-') {
        newWord = true;
      } else {
        newWord = false;
      }
    }
  } else if (outBuf[0] >= 'a' && outBuf[0] <= 'z') {
    outBuf[0] = static_cast<char>(outBuf[0] - 'a' + 'A');
  }
}

bool equalsIgnoreCase(const char* a, const char* b) {
  while (*a && *b) {
    unsigned char ca = static_cast<unsigned char>(*a);
    unsigned char cb = static_cast<unsigned char>(*b);
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb - 'A' + 'a');
    if (ca != cb) return false;
    ++a;
    ++b;
  }
  return *a == *b;
}

void addRecentEntryToList(std::vector<WikiRecentEntry>& list, const char* rawTitle, uint64_t offset) {
  if (!rawTitle || rawTitle[0] == '\0') return;
  WikiRecentEntry entry{};
  formatDisplayTitle(rawTitle, entry.title, sizeof(entry.title));
  entry.offset = offset;
  if (entry.title[0] == '\0') return;

  for (auto it = list.begin(); it != list.end();) {
    if ((offset != 0 && it->offset == offset) || equalsIgnoreCase(it->title, entry.title)) {
      it = list.erase(it);
    } else {
      ++it;
    }
  }
  list.insert(list.begin(), entry);
  if (list.size() > WikiConfigData::MAX_RECENT_ITEMS) {
    list.resize(WikiConfigData::MAX_RECENT_ITEMS);
  }
}

void loadConfigFromDisk(WikiConfigData& cfg) {
  cfg = WikiConfigData{};
  char buf[2560];
  size_t n = Storage.readFileToBuffer(WIKI_CFG_PATH, buf, sizeof(buf));
  if (n > 0) {
    char* saveptr = nullptr;
    for (char* line = strtok_r(buf, "\r\n", &saveptr); line != nullptr; line = strtok_r(nullptr, "\r\n", &saveptr)) {
      if (std::strncmp(line, "IMAGE_PATH=", 11) == 0) {
        std::strncpy(cfg.imagePath, line + 11, sizeof(cfg.imagePath) - 1);
        cfg.imagePath[sizeof(cfg.imagePath) - 1] = '\0';
      } else if (std::strncmp(line, "INDEX_SECTOR=", 13) == 0) {
        cfg.indexSector = static_cast<uint32_t>(std::strtoul(line + 13, nullptr, 10));
      } else if (std::strncmp(line, "CHUNKS_SECTOR=", 14) == 0) {
        cfg.chunksSector = static_cast<uint32_t>(std::strtoul(line + 14, nullptr, 10));
      } else if (std::strncmp(line, "FONT_SECTOR=", 12) == 0) {
        cfg.fontSector = static_cast<uint32_t>(std::strtoul(line + 12, nullptr, 10));
      } else if (std::strncmp(line, "HAS_IMAGES=", 11) == 0) {
        cfg.hasImages = static_cast<uint8_t>(std::strtoul(line + 11, nullptr, 10) ? 1 : 0);
      } else if (std::strncmp(line, "HAS_DATES=", 10) == 0) {
        cfg.hasDates = static_cast<uint8_t>(std::strtoul(line + 10, nullptr, 10) ? WikiConfigData::DATES_PRESENT
                                                                                   : WikiConfigData::DATES_ABSENT);
      } else if (std::strncmp(line, "RESUME_TITLE=", 13) == 0) {
        std::strncpy(cfg.resumeTitle, line + 13, sizeof(cfg.resumeTitle) - 1);
        cfg.resumeTitle[sizeof(cfg.resumeTitle) - 1] = '\0';
      } else if (std::strncmp(line, "RESUME_OFFSET=", 14) == 0) {
        cfg.resumeOffset = std::strtoull(line + 14, nullptr, 10);
      } else if (std::strncmp(line, "RESUME_PAGE=", 12) == 0) {
        cfg.resumePage = static_cast<int32_t>(std::strtol(line + 12, nullptr, 10));
      } else if (std::strncmp(line, "RESUME_LINK=", 12) == 0) {
        cfg.resumeLink = static_cast<int32_t>(std::strtol(line + 12, nullptr, 10));
      } else if (std::strncmp(line, "RESUME_OFFSETS=", 15) == 0) {
        cfg.resumePageOffsets.clear();
        const char* p = line + 15;
        while (*p) {
          char* endp = nullptr;
          unsigned long v = std::strtoul(p, &endp, 10);
          if (endp == p) break;
          cfg.resumePageOffsets.push_back(static_cast<uint32_t>(v));
          p = endp;
          if (*p == ',') ++p;
        }
      } else if (std::strncmp(line, "RECENT_SEARCH=", 14) == 0) {
        parseRecentLine(line + 14, cfg.recentSearches);
      } else if (std::strncmp(line, "RECENT_VISITED=", 15) == 0) {
        parseRecentLine(line + 15, cfg.recentVisited);
      }
    }
    return;
  }

  // Transparent one-time fallback migration from legacy settings.json if present
  parseSettingsJsonMigration(cfg);
  if (cfg.indexSector > 0) {
    WikiConfig::save();
  }
}
}  // namespace

void WikiConfigData::addRecentSearch(const char* title, uint64_t offset) {
  addRecentEntryToList(recentSearches, title, offset);
}

void WikiConfigData::addRecentVisited(const char* title, uint64_t offset) {
  addRecentEntryToList(recentVisited, title, offset);
}

void WikiScratchBuffer::registerRenderer(GfxRenderer* renderer) {
  if (renderer) {
    s_renderer = renderer;
    sdFontSystem.ensureBuiltInReaderFont(*renderer);
  }
}

uint8_t* WikiScratchBuffer::acquire() {
  if (s_refCount == 0) {
    WikiConfig::onAcquire();
  }
  ++s_refCount;
  FontDecompressor::setScratchProvider(&WikiScratchBuffer::borrowForFontDecompressor);
  if (!s_alignedBuffer) {
    s_alignedBuffer = allocateScratchBufferWithReclaim(s_allocatedBytes);
    if (!s_alignedBuffer) {
      LOG_ERR(TAG, "Failed to allocate WikiScratchBuffer!");
    }
  }
  return reinterpret_cast<uint8_t*>(s_alignedBuffer);
}

void WikiScratchBuffer::release() {
  if (s_refCount > 0) {
    --s_refCount;
  }
  if (s_refCount == 0) {
    FontDecompressor::setScratchProvider(nullptr);
    delete[] s_alignedBuffer;
    s_alignedBuffer = nullptr;
    s_allocatedBytes = 0;
    s_busyCount = 0;
    ++s_generation;
    WikiConfig::onRelease();
  }
}

uint8_t* WikiScratchBuffer::get() {
  if (!s_alignedBuffer) {
    s_alignedBuffer = allocateScratchBufferWithReclaim(s_allocatedBytes);
    if (s_alignedBuffer) {
      FontDecompressor::setScratchProvider(&WikiScratchBuffer::borrowForFontDecompressor);
    }
  }
  return reinterpret_cast<uint8_t*>(s_alignedBuffer);
}

size_t WikiScratchBuffer::capacity() { return s_allocatedBytes; }

void WikiScratchBuffer::beginUse() {
  ++s_busyCount;
  ++s_generation;
}

void WikiScratchBuffer::endUse() {
  if (s_busyCount > 0) {
    --s_busyCount;
  }
  ++s_generation;
}

uint8_t* WikiScratchBuffer::borrowForFontDecompressor(uint32_t neededBytes, uint32_t* outGeneration) {
  if (s_busyCount > 0 || neededBytes > BUFFER_SIZE) {
    return nullptr;
  }
  if (!s_alignedBuffer) {
    s_alignedBuffer = allocateScratchBufferWithReclaim(s_allocatedBytes);
    if (!s_alignedBuffer) {
      return nullptr;
    }
  }
  if (neededBytes > s_allocatedBytes) {
    return nullptr;
  }
  if (outGeneration) {
    *outGeneration = s_generation;
  }
  return reinterpret_cast<uint8_t*>(s_alignedBuffer);
}

void WikiConfig::onAcquire() {
  if (!s_config) {
    s_config = new (std::nothrow) WikiConfigData();
    if (s_config) {
      loadConfigFromDisk(*s_config);
    }
  }
}

void WikiConfig::onRelease() {
  delete s_config;
  s_config = nullptr;
}

WikiConfigData& WikiConfig::get() {
  if (!s_config) {
    onAcquire();
  }
  static WikiConfigData* s_fallback = nullptr;
  if (!s_config) {
    if (!s_fallback) s_fallback = new WikiConfigData();
    return *s_fallback;
  }
  return *s_config;
}

bool WikiConfig::save() {
  const WikiConfigData& cfg = get();
  Storage.ensureDirectoryExists("/.crosspoint");
  char content[2560];
  int pos = std::snprintf(
      content, sizeof(content),
      "IMAGE_PATH=%s\nINDEX_SECTOR=%u\nCHUNKS_SECTOR=%u\nFONT_SECTOR=%u\nHAS_IMAGES=%u\n",
      cfg.imagePath, cfg.indexSector, cfg.chunksSector, cfg.fontSector, static_cast<unsigned>(cfg.hasImages));
  if (pos > 0 && static_cast<size_t>(pos) < sizeof(content) && cfg.hasDates != WikiConfigData::DATES_UNKNOWN) {
    int dn = std::snprintf(content + pos, sizeof(content) - pos, "HAS_DATES=%u\n",
                           static_cast<unsigned>(cfg.hasDates == WikiConfigData::DATES_PRESENT ? 1 : 0));
    if (dn > 0 && static_cast<size_t>(pos + dn) < sizeof(content)) {
      pos += dn;
    }
  }
  if (pos > 0 && static_cast<size_t>(pos) < sizeof(content) && cfg.resumeTitle[0] != '\0') {
    int n = std::snprintf(content + pos, sizeof(content) - pos,
                          "RESUME_TITLE=%s\nRESUME_OFFSET=%llu\nRESUME_PAGE=%d\nRESUME_LINK=%d\nRESUME_OFFSETS=",
                          cfg.resumeTitle, static_cast<unsigned long long>(cfg.resumeOffset),
                          static_cast<int>(cfg.resumePage), static_cast<int>(cfg.resumeLink));
    if (n > 0 && static_cast<size_t>(pos + n) < sizeof(content)) {
      pos += n;
      for (size_t i = 0; i < cfg.resumePageOffsets.size(); ++i) {
        int m = std::snprintf(content + pos, sizeof(content) - pos, "%s%u", (i == 0 ? "" : ","),
                              cfg.resumePageOffsets[i]);
        if (m <= 0 || static_cast<size_t>(pos + m + 2) >= sizeof(content)) break;
        pos += m;
      }
      if (static_cast<size_t>(pos + 2) < sizeof(content)) {
        content[pos++] = '\n';
        content[pos] = '\0';
      }
    }
  }
  for (const auto& item : cfg.recentSearches) {
    if (pos <= 0 || static_cast<size_t>(pos) >= sizeof(content)) break;
    int m = std::snprintf(content + pos, sizeof(content) - pos, "RECENT_SEARCH=%llu|%s\n",
                          static_cast<unsigned long long>(item.offset), item.title);
    if (m <= 0 || static_cast<size_t>(pos + m) >= sizeof(content)) break;
    pos += m;
  }
  for (const auto& item : cfg.recentVisited) {
    if (pos <= 0 || static_cast<size_t>(pos) >= sizeof(content)) break;
    int m = std::snprintf(content + pos, sizeof(content) - pos, "RECENT_VISITED=%llu|%s\n",
                          static_cast<unsigned long long>(item.offset), item.title);
    if (m <= 0 || static_cast<size_t>(pos + m) >= sizeof(content)) break;
    pos += m;
  }
  return Storage.writeFile(WIKI_CFG_PATH, String(content));
}

void WikiConfig::clear() {
  if (s_config) {
    *s_config = WikiConfigData{};
  }
  Storage.remove(WIKI_CFG_PATH);
}

bool WikiConfig::configureFromImageFile(const std::string& imgPath) {
  if (imgPath.length() <= 4) return false;
  HalFile f;
  if (!Storage.openFileForRead("WikiConfig", imgPath, f)) {
    return false;
  }
  uint64_t fileSize = f.fileSize64();
  bool ok = false;
  if (fileSize >= 512) {
    f.seek64(fileSize - 512);
    uint8_t sector[512];
    if (f.read(sector, 512) == 512 && sector[0] == 'C' && sector[1] == 'P' && sector[2] == 'R' && sector[3] == '1') {
      uint32_t indexSector = sector[4] | (sector[5] << 8) | (sector[6] << 16) | (sector[7] << 24);
      uint32_t chunksSector = sector[8] | (sector[9] << 8) | (sector[10] << 16) | (sector[11] << 24);
      uint32_t fontSector = sector[12] | (sector[13] << 8) | (sector[14] << 16) | (sector[15] << 24);
      uint32_t imageFlags = sector[16] | (sector[17] << 8) | (sector[18] << 16) | (sector[19] << 24);

      WikiConfigData& cfg = get();
      cfg.indexSector = indexSector;
      cfg.chunksSector = chunksSector;
      cfg.fontSector = fontSector;
      cfg.hasImages = (imageFlags & 0x1u) ? 1 : 0;
      if (imageFlags & 0x2u) {
        cfg.hasDates = (imageFlags & 0x4u) ? WikiConfigData::DATES_PRESENT : WikiConfigData::DATES_ABSENT;
      } else {
        cfg.hasDates = WikiConfigData::DATES_UNKNOWN;
      }
      std::strncpy(cfg.imagePath, imgPath.c_str(), sizeof(cfg.imagePath) - 1);
      cfg.imagePath[sizeof(cfg.imagePath) - 1] = '\0';
      cfg.clearResume();
      save();
      ok = true;
    }
  }
  f.close();
  return ok;
}

bool WikiConfig::resolveHasDates(WikiSearchEngine& engine, SdRawReader& reader) {
  WikiConfigData& cfg = get();
  if (cfg.hasDates != WikiConfigData::DATES_UNKNOWN) {
    return cfg.hasDates == WikiConfigData::DATES_PRESENT;
  }
  if (cfg.indexSector == 0) {
    return false;
  }
  if (!reader.isMounted() && !reader.mount(-1, -1, -1, -1)) {
    return false;
  }
  engine.setRootSector(cfg.indexSector);

  // Backwards-compatible B-Tree probe for legacy .img archives built before CPR1 FlagDatesKnown (0x2):
  // Check 6 ordinary mid-month dates spread across the year so partial dumps (e.g. top_maxi, which
  // only contains Feb 25 & Jun 6) resolve to false while full Wikipedia dumps resolve to true in <5 ms.
  static constexpr const char* kProbes[6][3] = {
      {"January 15", "january 15", "15 january"},
      {"March 19", "march 19", "19 march"},
      {"May 12", "may 12", "12 may"},
      {"July 22", "july 22", "22 july"},
      {"September 18", "september 18", "18 september"},
      {"November 14", "november 14", "14 november"},
  };
  int found = 0;
  uint64_t dummyOffset = 0;
  for (int i = 0; i < 6; ++i) {
    if (engine.search(kProbes[i][0], dummyOffset) || engine.search(kProbes[i][1], dummyOffset) ||
        engine.search(kProbes[i][2], dummyOffset)) {
      ++found;
    }
  }
  cfg.hasDates = (found >= 5) ? WikiConfigData::DATES_PRESENT : WikiConfigData::DATES_ABSENT;
  save();
  return cfg.hasDates == WikiConfigData::DATES_PRESENT;
}

namespace WikiJpeg {
namespace {
struct JpegStreamState {
  GfxRenderer* renderer = nullptr;
  SdRawReader* reader = nullptr;
  uint32_t startSector = 0;
  uint32_t startOffsetInSector = 0;
  int32_t filePos = 0;
  int x = 0;
  int y = 0;
  int clipMinX = 0;
  int clipMaxX = 0x7FFF;
  int clipMinY = 0;
  int clipMaxY = 0x7FFF;
  freeink::ImageToneMapper mapper;
  uint8_t* band = nullptr;
  uint16_t bandW = 0;
  uint32_t bandTop = 0;
  uint32_t bandFilled = 0;
  bool stopped = false;

  void flushBandRows(uint32_t upToSrcY) {
    while (bandTop < upToSrcY && !stopped) {
      mapper.addSourceRow(band + (bandTop % 16) * bandW);
      ++bandTop;
    }
  }
};

void rowSinkCallback(void* ctx, int dstY, const uint8_t* levels, int dstWidth) {
  auto* state = static_cast<JpegStreamState*>(ctx);
  if (!state || !state->renderer) return;
  const int drawY = state->y + dstY;
  if (drawY < state->clipMinY || drawY >= state->clipMaxY) return;
  const int drawX = state->x;
  for (int i = 0; i < dstWidth; ++i) {
    const int px = drawX + i;
    if (px >= state->clipMinX && px < state->clipMaxX && levels[i] == 0) {
      state->renderer->drawPixel(px, drawY, true);
    }
  }
}

void myClose(void*) {}

int32_t myRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t iLen) {
  auto* state = static_cast<JpegStreamState*>(pFile->fHandle);
  if (!state || !state->reader) return 0;
  int32_t bytesToRead = iLen;
  uint8_t sectorBuf[512];

  while (bytesToRead > 0) {
    const uint32_t totalByteOffset = state->startOffsetInSector + state->filePos;
    const uint32_t sector = state->startSector + (totalByteOffset / 512);
    const uint32_t offsetInSector = totalByteOffset % 512;

    if (!state->reader->readSectors(sector, 1, sectorBuf)) {
      break;
    }

    const int32_t availableInSector = 512 - offsetInSector;
    const int32_t copyCount = (bytesToRead < availableInSector) ? bytesToRead : availableInSector;

    std::memcpy(pBuf, sectorBuf + offsetInSector, copyCount);

    pBuf += copyCount;
    state->filePos += copyCount;
    bytesToRead -= copyCount;
  }

  return iLen - bytesToRead;
}

int32_t mySeek(JPEGFILE* pFile, int32_t iPosition) {
  auto* state = static_cast<JpegStreamState*>(pFile->fHandle);
  if (state) state->filePos = iPosition;
  return iPosition;
}

int myDraw(JPEGDRAW* pDraw) {
  auto* state = static_cast<JpegStreamState*>(pDraw->pUser);
  if (!state || state->stopped) return 0;

  if (static_cast<uint32_t>(pDraw->y) >= state->bandFilled) {
    state->flushBandRows(state->bandFilled);
    state->bandFilled = pDraw->y + pDraw->iHeight;
  } else if (static_cast<uint32_t>(pDraw->y + pDraw->iHeight) > state->bandFilled) {
    state->bandFilled = pDraw->y + pDraw->iHeight;
  }

  const auto* src = reinterpret_cast<const uint8_t*>(pDraw->pPixels);
  int copyWidth = pDraw->iWidth;
  if (pDraw->x + copyWidth > state->bandW) {
    copyWidth = state->bandW - pDraw->x;
  }
  if (copyWidth <= 0) return 1;

  for (int by = 0; by < pDraw->iHeight; ++by) {
    if (static_cast<uint32_t>(pDraw->y + by) >= state->bandFilled) break;
    uint8_t* dst = state->band + ((pDraw->y + by) % 16) * state->bandW + pDraw->x;
    std::memcpy(dst, src + by * pDraw->iWidth, copyWidth);
  }
  return 1;
}
}  // namespace

bool getDimensions(SdRawReader& reader, uint32_t startSector, uint32_t offsetInSector, int& outWidth, int& outHeight) {
  outWidth = 0;
  outHeight = 0;
  uint8_t* scratch = WikiScratchBuffer::get();
  if (!scratch) return false;

  struct UseGuard {
    UseGuard() { WikiScratchBuffer::beginUse(); }
    ~UseGuard() { WikiScratchBuffer::endUse(); }
  } guard;

  auto* jpeg = new (scratch) JPEGDEC();
  JpegStreamState state;
  state.reader = &reader;
  state.startSector = startSector;
  state.startOffsetInSector = offsetInSector;
  if (jpeg->open(static_cast<void*>(&state), 0x7FFFFFFF, myClose, myRead, mySeek, myDraw)) {
    outWidth = jpeg->getWidth();
    outHeight = jpeg->getHeight();
    jpeg->close();
    return (outWidth > 0 && outHeight > 0);
  }
  return false;
}

bool render(GfxRenderer& renderer, SdRawReader& reader, uint32_t startSector, uint32_t offsetInSector, int drawX,
            int drawY, int dstW, int dstH, int clipMinX, int clipMaxX, int clipMinY, int clipMaxY) {
  if (clipMinX >= clipMaxX || clipMinY >= clipMaxY) return false;
  uint8_t* scratch = WikiScratchBuffer::get();
  if (!scratch) return false;

  struct UseGuard {
    UseGuard() { WikiScratchBuffer::beginUse(); }
    ~UseGuard() { WikiScratchBuffer::endUse(); }
  } guard;

  auto* jpeg = new (scratch) JPEGDEC();
  JpegStreamState state;
  state.renderer = &renderer;
  state.reader = &reader;
  state.startSector = startSector;
  state.startOffsetInSector = offsetInSector;
  state.clipMinX = clipMinX;
  state.clipMaxX = clipMaxX;
  state.clipMinY = clipMinY;
  state.clipMaxY = clipMaxY;

  if (!jpeg->open(static_cast<void*>(&state), 0x7FFFFFFF, myClose, myRead, mySeek, myDraw)) {
    return false;
  }

  jpeg->setUserPointer(&state);
  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
  const int imgWidth = jpeg->getWidth();
  const int imgHeight = jpeg->getHeight();
  const int targetW = (dstW > 0) ? dstW : imgWidth;
  const int targetH = (dstH > 0) ? dstH : imgHeight;

  state.x = drawX;
  state.y = drawY;
  state.bandW = imgWidth;
  state.band = scratch + sizeof(JPEGDEC);

  if (state.mapper.begin(imgWidth, imgHeight, targetW, targetH, 2, rowSinkCallback, &state)) {
    jpeg->decode(0, 0, 0);
    state.flushBandRows(imgHeight);
    state.mapper.finish();
  }
  jpeg->close();
  return true;
}

bool renderCenteredInBox(GfxRenderer& renderer, SdRawReader& reader, uint32_t startSector, uint32_t offsetInSector,
                         int boxX, int boxW, int minLeftX, int drawY, int clipMinX, int clipMaxX, int clipMinY,
                         int clipMaxY) {
  int imgWidth = 0;
  int imgHeight = 0;
  if (!getDimensions(reader, startSector, offsetInSector, imgWidth, imgHeight)) {
    return false;
  }
  int imgDrawX = boxX + (boxW / 2) - (imgWidth / 2);
  if (imgDrawX < minLeftX) imgDrawX = minLeftX;
  return render(renderer, reader, startSector, offsetInSector, imgDrawX, drawY, imgWidth, imgHeight, clipMinX, clipMaxX,
                clipMinY, clipMaxY);
}
}  // namespace WikiJpeg

// --- WikiFileSelectActivity implementation ---

WikiFileSelectActivity::WikiFileSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               std::string basepath)
    : UiListActivity("WikiFileSelect", renderer, mappedInput), basepath_(std::move(basepath)) {
  if (basepath_.empty()) basepath_ = "/";
  loadFiles();
}

void WikiFileSelectActivity::loadFiles() {
  entries_.clear();
  rowItems_.clear();

  auto root = Storage.open(basepath_.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }
  root.rewindDirectory();

  char nameBuf[256];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(nameBuf, sizeof(nameBuf));
    const bool isDir = file.isDirectory();
    if (nameBuf[0] == '.' || std::strcmp(nameBuf, "System Volume Information") == 0) {
      file.close();
      continue;
    }
    if (isDir) {
      entries_.emplace_back(std::string(nameBuf) + "/");
    } else {
      std::string_view fname{nameBuf};
      if (FsHelpers::checkFileExtension(fname, ".img")) {
        entries_.emplace_back(fname);
      }
    }
    file.close();
  }
  root.close();

  FsHelpers::sortFileList(entries_);
  rowItems_.reserve(entries_.size());
  for (size_t i = 0; i < entries_.size(); ++i) {
    freeink::ui::ListItem li{};
    li.label = entries_[i].c_str();
    li.actionValue = static_cast<int16_t>(i);
    rowItems_.push_back(li);
  }
}

void WikiFileSelectActivity::onEnter() {
  buttonSelectionVisible_ = !mappedInput.hasTouchHardware();
  UiListActivity::onEnter();
}

int WikiFileSelectActivity::listCount() const { return static_cast<int>(entries_.size()); }

const char* WikiFileSelectActivity::headerTitle() const { return "Select Wikipedia .img"; }

void WikiFileSelectActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int headerH = TouchHeaderBackButton::height(metrics, mappedInput);
  screen.setContentMarginFromScreen(freeink::ui::Insets{
      static_cast<int16_t>(metrics.topPadding + headerH + (mappedInput.hasTouchHardware() ? 4 : 12)), 0,
      static_cast<int16_t>(WikiGUI::hintsHeight(mappedInput)), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  freeink::ui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = freeink::ui::InputTouch;
  syncListViewport(screen, props);
  if (!buttonSelectionVisible_) {
    props.selectedIndex = -1;
  }
  screen.list(props);
}

void WikiFileSelectActivity::navigateButtons() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Down) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left) ||
      mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    buttonSelectionVisible_ = true;
  }
  UiListActivity::navigateButtons();
}

void WikiFileSelectActivity::activateIndex(int index) {
  if (index < 0 || static_cast<size_t>(index) >= entries_.size()) return;
  const std::string& entry = entries_[index];
  if (!entry.empty() && entry.back() == '/') {
    if (basepath_.back() != '/') basepath_ += "/";
    basepath_ += entry.substr(0, entry.length() - 1);
    loadFiles();
    nav.selected = 0;
    nav.top = 0;
    requestUpdate();
    return;
  }

  std::string fullPath = basepath_;
  if (fullPath.empty() || fullPath.back() != '/') fullPath += "/";
  fullPath += entry;
  ActivityResult res{FilePathResult{std::move(fullPath)}};
  setResult(std::move(res));
  finish();
}

void WikiFileSelectActivity::onBackButton() {
  if (basepath_ != "/" && !basepath_.empty()) {
    size_t slash = basepath_.find_last_of('/');
    basepath_ = (slash == 0 || slash == std::string::npos) ? "/" : basepath_.substr(0, slash);
    loadFiles();
    nav.selected = 0;
    nav.top = 0;
    requestUpdate();
    return;
  }
  ActivityResult res;
  res.isCancelled = true;
  setResult(std::move(res));
  finish();
}

bool WikiFileSelectActivity::handleCustomInput() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    onBackButton();
    return true;
  }
  return false;
}

void WikiFileSelectActivity::drawChrome() {
  const char* title = headerTitle();
  if (!title) return;
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, title, false);
  } else {
    GUI.drawHeader(renderer, header, title);
  }
}

void WikiFileSelectActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  WikiGUI::drawDoubleButtonHints(renderer, {labels.btn1, ""}, {labels.btn2, ""}, {labels.btn3, ""}, {labels.btn4, ""});
}

// --- WikiRecentActivity implementation ---

WikiRecentActivity::WikiRecentActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("WikiRecent", renderer, mappedInput) {}

void WikiRecentActivity::onEnter() {
  Activity::onEnter();
  const auto& cfg = WikiConfig::get();
  const int totalSelectable = static_cast<int>(cfg.recentSearches.size() + cfg.recentVisited.size()) + 1;
  if (mappedInput.hasTouchHardware()) {
    selectedIndex_ = -1;
  } else if (selectedIndex_ < 0 || selectedIndex_ >= totalSelectable) {
    selectedIndex_ = 0;
  }
  requestUpdate();
}

void WikiRecentActivity::activateSelection() {
  auto& cfg = WikiConfig::get();
  const int sCount = static_cast<int>(cfg.recentSearches.size());
  const int vCount = static_cast<int>(cfg.recentVisited.size());

  const WikiRecentEntry* chosen = nullptr;
  if (selectedIndex_ >= 0 && selectedIndex_ < sCount) {
    chosen = &cfg.recentSearches[selectedIndex_];
  } else if (selectedIndex_ >= sCount && selectedIndex_ < sCount + vCount) {
    chosen = &cfg.recentVisited[selectedIndex_ - sCount];
  } else {
    cfg.clearRecentHistory();
    WikiConfig::save();
    selectedIndex_ = mappedInput.hasTouchHardware() ? -1 : 0;
    requestUpdate();
    return;
  }

  if (chosen && chosen->title[0] != '\0') {
    char payload[136];
    std::snprintf(payload, sizeof(payload), "%llu|%s",
                  static_cast<unsigned long long>(chosen->offset), chosen->title);
    ActivityResult res;
    res.isCancelled = false;
    res.data = FilePathResult{std::string(payload)};
    setResult(std::move(res));
    finish();
  }
}

void WikiRecentActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasBackGesture() || mappedInput.wasHomeGesture()) {
    ActivityResult res;
    res.isCancelled = true;
    setResult(std::move(res));
    finish();
    return;
  }

  const auto& cfg = WikiConfig::get();
  const int sCount = static_cast<int>(cfg.recentSearches.size());
  const int vCount = static_cast<int>(cfg.recentVisited.size());
  const int totalSelectable = sCount + vCount + 1;

  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int headerEndY = metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput);
    if (ty < headerEndY) {
      return;
    }

    constexpr int kSectionHeaderH = 26;
    constexpr int kRowH = 28;
    const int searchListStartY = headerEndY + 8 + kSectionHeaderH;
    for (int i = 0; i < sCount; ++i) {
      int ry = searchListStartY + i * kRowH;
      if (ty >= ry - 2 && ty < ry + kRowH - 2) {
        selectedIndex_ = i;
        activateSelection();
        return;
      }
    }

    const int afterSearchesY = searchListStartY + std::max(1, sCount) * kRowH;
    const int visitedListStartY = afterSearchesY + 12 + kSectionHeaderH;
    for (int i = 0; i < vCount; ++i) {
      int ry = visitedListStartY + i * kRowH;
      if (ty >= ry - 2 && ty < ry + kRowH - 2) {
        selectedIndex_ = sCount + i;
        activateSelection();
        return;
      }
    }

    const int afterVisitedY = visitedListStartY + std::max(1, vCount) * kRowH;
    const int clearBtnY = afterVisitedY + 16;
    if (ty >= clearBtnY && ty <= clearBtnY + 32) {
      selectedIndex_ = sCount + vCount;
      activateSelection();
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedIndex_ = (selectedIndex_ <= 0) ? (totalSelectable - 1) : (selectedIndex_ - 1);
    requestUpdate();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
      mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedIndex_ = (selectedIndex_ < 0) ? 0 : ((selectedIndex_ + 1) % totalSelectable);
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && selectedIndex_ >= 0) {
    activateSelection();
    return;
  }
}

void WikiRecentActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, "Recent", false);
  } else {
    GUI.drawHeader(renderer, header, "Recent");
  }

  const auto& cfg = WikiConfig::get();
  const int sCount = static_cast<int>(cfg.recentSearches.size());
  const int vCount = static_cast<int>(cfg.recentVisited.size());

  const int headerEndY = header.y + header.height;
  constexpr int kSectionHeaderH = 26;
  constexpr int kRowH = 28;

  // 1. "Searches" section
  const int searchHeaderY = headerEndY + 8;
  renderer.drawText(UI_12_FONT_ID, 16, searchHeaderY, "Searches", true, EpdFontFamily::BOLD);
  renderer.drawLine(16, searchHeaderY + kSectionHeaderH - 4, pageWidth - 16, searchHeaderY + kSectionHeaderH - 4, true);

  const int searchListStartY = searchHeaderY + kSectionHeaderH;
  if (sCount == 0) {
    renderer.drawText(UI_10_FONT_ID, 20, searchListStartY + 2, "No recent searches", true, EpdFontFamily::REGULAR);
  } else {
    for (int i = 0; i < sCount; ++i) {
      const int y = searchListStartY + i * kRowH;
      char itemLabel[112];
      std::snprintf(itemLabel, sizeof(itemLabel), "%d. %s", i + 1, cfg.recentSearches[i].title);
      std::string safeLabel =
          renderer.truncatedText(UI_10_FONT_ID, itemLabel, pageWidth - 40, EpdFontFamily::REGULAR);
      if (selectedIndex_ == i) {
        renderer.fillRect(14, y - 2, pageWidth - 28, kRowH - 2, true);
        renderer.drawText(UI_10_FONT_ID, 20, y + 1, safeLabel.c_str(), false, EpdFontFamily::BOLD);
      } else {
        renderer.drawText(UI_10_FONT_ID, 20, y + 1, safeLabel.c_str(), true, EpdFontFamily::REGULAR);
      }
    }
  }

  // 2. "Visited" section
  const int afterSearchesY = searchListStartY + std::max(1, sCount) * kRowH;
  const int visitedHeaderY = afterSearchesY + 12;
  renderer.drawText(UI_12_FONT_ID, 16, visitedHeaderY, "Visited", true, EpdFontFamily::BOLD);
  renderer.drawLine(16, visitedHeaderY + kSectionHeaderH - 4, pageWidth - 16, visitedHeaderY + kSectionHeaderH - 4, true);

  const int visitedListStartY = visitedHeaderY + kSectionHeaderH;
  if (vCount == 0) {
    renderer.drawText(UI_10_FONT_ID, 20, visitedListStartY + 2, "No recently visited articles", true,
                      EpdFontFamily::REGULAR);
  } else {
    for (int i = 0; i < vCount; ++i) {
      const int y = visitedListStartY + i * kRowH;
      char itemLabel[112];
      std::snprintf(itemLabel, sizeof(itemLabel), "%d. %s", i + 1, cfg.recentVisited[i].title);
      std::string safeLabel =
          renderer.truncatedText(UI_10_FONT_ID, itemLabel, pageWidth - 40, EpdFontFamily::REGULAR);
      const int itemIdx = sCount + i;
      if (selectedIndex_ == itemIdx) {
        renderer.fillRect(14, y - 2, pageWidth - 28, kRowH - 2, true);
        renderer.drawText(UI_10_FONT_ID, 20, y + 1, safeLabel.c_str(), false, EpdFontFamily::BOLD);
      } else {
        renderer.drawText(UI_10_FONT_ID, 20, y + 1, safeLabel.c_str(), true, EpdFontFamily::REGULAR);
      }
    }
  }

  // 3. "Clear history" selectable button/header under the 2 lists
  const int afterVisitedY = visitedListStartY + std::max(1, vCount) * kRowH;
  const int clearBtnY = afterVisitedY + 16;
  const int clearBtnH = 32;
  const char* clearText = "Clear history";
  const int clearTextW = renderer.getTextWidth(UI_12_FONT_ID, clearText, EpdFontFamily::BOLD);
  const int clearTextX = (pageWidth - clearTextW) / 2;
  const bool clearSelected = (selectedIndex_ == sCount + vCount);
  if (clearSelected) {
    renderer.fillRect(14, clearBtnY, pageWidth - 28, clearBtnH, true);
    renderer.drawText(UI_12_FONT_ID, clearTextX, clearBtnY + 6, clearText, false, EpdFontFamily::BOLD);
  } else {
    renderer.drawRect(14, clearBtnY, pageWidth - 28, clearBtnH, true);
    renderer.drawText(UI_12_FONT_ID, clearTextX, clearBtnY + 6, clearText, true, EpdFontFamily::BOLD);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  WikiGUI::drawDoubleButtonHints(renderer, {labels.btn1, ""}, {labels.btn2, ""}, {labels.btn3, ""}, {labels.btn4, ""});

  renderer.displayBuffer();
}

