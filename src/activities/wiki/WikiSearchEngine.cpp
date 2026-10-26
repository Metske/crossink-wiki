#include "WikiSearchEngine.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <Logging.h>
#ifdef ESP32
#include <esp_random.h>
#endif

static const char* TAG = "WikiSearchEngine";

WikiSearchEngine::WikiSearchEngine(SdRawReader& reader, uint32_t rootSector)
    : reader_(reader), rootSector_(rootSector) {}

bool WikiSearchEngine::isValidNode(const uint8_t* buffer) {
  if (!buffer) return false;

  const uint8_t isLeaf = buffer[0];
  const uint8_t numKeys = buffer[1];

  // isLeaf must be 0 or 1
  if (isLeaf > 1) return false;

  // numKeys must be between 1 and MAX_KEYS (7)
  if (numKeys == 0 || numKeys > MAX_KEYS) return false;

  // In xt-zim-imager serialization, bytes 2 and 3 are padding (0)
  if (buffer[2] != 0 || buffer[3] != 0) return false;

  // Inspect the first key at buffer + 4: must have printable characters
  const char* firstKey = reinterpret_cast<const char*>(buffer + 4);
  bool hasPrintable = false;
  for (size_t i = 0; i < KEY_SIZE && firstKey[i] != '\0'; ++i) {
    const unsigned char c = static_cast<unsigned char>(firstKey[i]);
    if (c >= 32 && c <= 126) {
      hasPrintable = true;
    } else if (c < 32) {
      return false;
    }
  }

  return hasPrintable;
}

bool WikiSearchEngine::search(const char* title, uint64_t& outOffset) {
  if (!reader_.isMounted() || !title || title[0] == '\0') {
    return false;
  }

  uint32_t currentSector = rootSector_;
  char searchKey[KEY_SIZE];
  std::memset(searchKey, 0, KEY_SIZE);

  // Copy searchKey exactly to match xt-zim-imager exact convention
  for (size_t i = 0; i < KEY_SIZE && title[i] != '\0'; ++i) {
    searchKey[i] = title[i];
  }

  int depth = 0;
  alignas(4) uint8_t buffer[NODE_SIZE];

  while (depth < (int)MAX_DEPTH) {
    if (!reader_.readSectors(currentSector, 1, buffer)) {
      LOG_ERR(TAG, "Failed to read node sector %u", currentSector);
      return false;
    }

    if (!isValidNode(buffer)) {
      LOG_ERR(TAG, "Invalid node at sector %u", currentSector);
      return false;
    }

    const bool isLeaf = (buffer[0] != 0);
    const uint8_t numKeys = buffer[1];

    const char* keysBase = reinterpret_cast<const char*>(buffer + 4);
    const uint64_t* offsetsBase =
        reinterpret_cast<const uint64_t*>(buffer + 4 + MAX_KEYS * KEY_SIZE);
    const uint32_t* childrenBase =
        reinterpret_cast<const uint32_t*>(buffer + 4 + MAX_KEYS * KEY_SIZE + MAX_KEYS * 8);

    int left = 0;
    int right = numKeys - 1;
    while (left <= right) {
      int mid = left + (right - left) / 2;
      const char* midKey = keysBase + mid * KEY_SIZE;

      int cmp = std::strncmp(searchKey, midKey, KEY_SIZE);
      if (cmp == 0) {
        outOffset = offsetsBase[mid];
        return true;
      }
      if (cmp < 0) {
        right = mid - 1;
      } else {
        left = mid + 1;
      }
    }

    if (isLeaf) {
      return false;
    }

    currentSector = rootSector_ + childrenBase[left];
    depth++;
  }
  return false;
}

int WikiSearchEngine::searchPrefix(const char* prefix, WikiSearchResult* results, int maxResults) {
  if (!reader_.isMounted() || !prefix || prefix[0] == '\0' || !results || maxResults <= 0) {
    return 0;
  }

  // Copy prefix verbatim to match exact casing
  char lowerPrefix[KEY_SIZE + 1];
  size_t prefixLen = 0;
  while (prefix[prefixLen] != '\0' && prefixLen < KEY_SIZE) {
    lowerPrefix[prefixLen] = prefix[prefixLen];
    prefixLen++;
  }
  lowerPrefix[prefixLen] = '\0';
  if (prefixLen == 0) return 0;

  struct StackFrame {
    uint32_t sector;
    int step;
    bool searched;
  };

  StackFrame stack[MAX_DEPTH];
  int depth = 0;
  stack[0] = {rootSector_, 0, false};

  int foundCount = 0;
  bool started = false;

  alignas(4) uint8_t buffer[NODE_SIZE];
  uint32_t cachedSector = 0xFFFFFFFF;

  while (depth >= 0 && depth < (int)MAX_DEPTH && foundCount < maxResults) {
    StackFrame& frame = stack[depth];

    if (cachedSector != frame.sector) {
      if (!reader_.readSectors(frame.sector, 1, buffer)) {
        return foundCount;
      }
      cachedSector = frame.sector;
    }

    if (!isValidNode(buffer)) {
      return foundCount;
    }

    const bool isLeaf = (buffer[0] != 0);
    const uint8_t numKeys = buffer[1];
    const char* keysBase = reinterpret_cast<const char*>(buffer + 4);
    const uint64_t* offsetsBase =
        reinterpret_cast<const uint64_t*>(buffer + 4 + MAX_KEYS * KEY_SIZE);
    const uint32_t* childrenBase =
        reinterpret_cast<const uint32_t*>(buffer + 4 + MAX_KEYS * KEY_SIZE + MAX_KEYS * 8);

    if (!frame.searched) {
      frame.searched = true;
      if (!started) {
        int left = 0;
        int right = numKeys - 1;
        while (left <= right) {
          int mid = left + (right - left) / 2;
          const char* midKey = keysBase + mid * KEY_SIZE;

          int cmp = std::strncmp(midKey, lowerPrefix, prefixLen);
          if (cmp < 0) {
            left = mid + 1;
          } else {
            right = mid - 1;
          }
        }
        frame.step = left * 2;
      } else {
        frame.step = 0;
      }
    }

    if (frame.step <= numKeys * 2) {
      if (frame.step % 2 == 0) {
        int childIdx = frame.step / 2;
        frame.step++;
        if (!isLeaf && depth + 1 < (int)MAX_DEPTH) {
          depth++;
          stack[depth] = {rootSector_ + childrenBase[childIdx], 0, false};
        }
      } else {
        int keyIdx = frame.step / 2;
        frame.step++;

        const char* key = keysBase + keyIdx * KEY_SIZE;
        if (std::strncmp(key, lowerPrefix, prefixLen) == 0) {
          started = true;
          size_t kLen = 0;
          while (kLen < KEY_SIZE && key[kLen] != '\0') {
            unsigned char c = key[kLen];
            if (c >= 32 && c != 127) {
              results[foundCount].title[kLen] = c;
            } else {
              results[foundCount].title[kLen] = ' ';
            }
            kLen++;
          }
          results[foundCount].title[kLen] = '\0';
          // Trim trailing spaces if any
          while (kLen > 0 && results[foundCount].title[kLen - 1] == ' ') {
            kLen--;
            results[foundCount].title[kLen] = '\0';
          }
          results[foundCount].offset = offsetsBase[keyIdx];
          foundCount++;
          if (foundCount >= maxResults) return foundCount;
        } else if (started || std::strncmp(key, lowerPrefix, prefixLen) > 0) {
          return foundCount;
        }
      }
    } else {
      depth--;
    }
  }

  return foundCount;
}

static uint32_t nextRandomUint32() {
#ifdef ESP32
  return esp_random();
#else
  return static_cast<uint32_t>(std::rand());
#endif
}

static bool isHexHash32(const char* key) {
  for (size_t i = 0; i < 32; ++i) {
    char c = key[i];
    bool isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!isHex) return false;
  }
  return key[32] == '\0';
}

static bool isPreferredRandomKey(const char* key, bool requireUppercaseStart) {
  if (!key || key[0] == '\0' || key[1] == '\0') return false;
  if (requireUppercaseStart && !(key[0] >= 'A' && key[0] <= 'Z')) return false;
  if (isHexHash32(key)) return false;
  for (size_t i = 0; i < WikiSearchEngine::KEY_SIZE && key[i] != '\0'; ++i) {
    if (key[i] == ':' || key[i] == '_') return false;
  }
  return true;
}

static void copyCleanTitle(const char* srcKey, char* dstTitle) {
  size_t kLen = 0;
  while (kLen < WikiSearchEngine::KEY_SIZE && srcKey[kLen] != '\0') {
    unsigned char c = static_cast<unsigned char>(srcKey[kLen]);
    dstTitle[kLen] = (c >= 32 && c != 127) ? static_cast<char>(c) : ' ';
    kLen++;
  }
  dstTitle[kLen] = '\0';
  while (kLen > 0 && dstTitle[kLen - 1] == ' ') {
    kLen--;
    dstTitle[kLen] = '\0';
  }
}

bool WikiSearchEngine::getRandomArticle(WikiSearchResult& outResult) {
  if (!reader_.isMounted() || rootSector_ == 0) {
    return false;
  }

  alignas(4) uint8_t buffer[NODE_SIZE];
  WikiSearchResult fallbackCandidate{};
  bool hasFallback = false;

  for (int attempt = 0; attempt < 8; ++attempt) {
    uint32_t currentSector = rootSector_;
    bool constrainToUppercase = (attempt < 5);

    for (size_t depth = 0; depth < MAX_DEPTH; ++depth) {
      if (!reader_.readSectors(currentSector, 1, buffer)) {
        break;
      }
      if (!isValidNode(buffer)) {
        break;
      }

      const bool isLeaf = (buffer[0] != 0);
      const uint8_t numKeys = buffer[1];
      const char* keysBase = reinterpret_cast<const char*>(buffer + 4);
      const uint64_t* offsetsBase =
          reinterpret_cast<const uint64_t*>(buffer + 4 + MAX_KEYS * KEY_SIZE);
      const uint32_t* childrenBase =
          reinterpret_cast<const uint32_t*>(buffer + 4 + MAX_KEYS * KEY_SIZE + MAX_KEYS * 8);

      if (isLeaf) {
        uint8_t matchingIndices[MAX_KEYS];
        uint8_t matchCount = 0;
        for (uint8_t i = 0; i < numKeys; ++i) {
          char tempKey[KEY_SIZE + 1];
          copyCleanTitle(keysBase + i * KEY_SIZE, tempKey);
          if (isPreferredRandomKey(tempKey, constrainToUppercase)) {
            matchingIndices[matchCount++] = i;
          } else if (!hasFallback && isPreferredRandomKey(tempKey, false)) {
            copyCleanTitle(keysBase + i * KEY_SIZE, fallbackCandidate.title);
            fallbackCandidate.offset = offsetsBase[i];
            hasFallback = true;
          }
        }
        if (matchCount > 0) {
          uint8_t pick = matchingIndices[nextRandomUint32() % matchCount];
          copyCleanTitle(keysBase + pick * KEY_SIZE, outResult.title);
          outResult.offset = offsetsBase[pick];
          return true;
        }
        break;
      }

      uint8_t validChildren[MAX_KEYS + 1];
      uint8_t validChildCount = 0;
      if (constrainToUppercase) {
        for (uint8_t cIdx = 0; cIdx <= numKeys; ++cIdx) {
          const char* lowKey = (cIdx > 0) ? (keysBase + (cIdx - 1) * KEY_SIZE) : nullptr;
          const char* highKey = (cIdx < numKeys) ? (keysBase + cIdx * KEY_SIZE) : nullptr;
          bool highOk = !highKey || (static_cast<unsigned char>(highKey[0]) >= 'A');
          bool lowOk = !lowKey || (static_cast<unsigned char>(lowKey[0]) <= 'Z');
          if (highOk && lowOk) {
            validChildren[validChildCount++] = cIdx;
          }
        }
      }
      uint8_t chosenChild = (validChildCount > 0)
                                ? validChildren[nextRandomUint32() % validChildCount]
                                : static_cast<uint8_t>(nextRandomUint32() % (numKeys + 1));
      currentSector = rootSector_ + childrenBase[chosenChild];
    }
  }

  if (hasFallback) {
    outResult = fallbackCandidate;
    return true;
  }
  return false;
}
