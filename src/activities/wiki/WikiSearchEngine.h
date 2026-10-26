#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "SdRawReader.h"

struct WikiSearchResult {
  char title[65];  // 64 bytes from B-Tree + null terminator
  uint64_t offset;
};

/**
 * @class WikiSearchEngine
 * @brief Binary search and prefix search over the B-Tree index using raw sector reads.
 * 
 * Strict Constraint: Zero dynamic memory allocation (`malloc`/`new`).
 */
class WikiSearchEngine {
 public:
  static constexpr size_t NODE_SIZE = 512;
  static constexpr size_t MAX_KEYS = 6;
  static constexpr size_t KEY_SIZE = 64;

  WikiSearchEngine(SdRawReader& reader, uint32_t rootSector);
  ~WikiSearchEngine() = default;

  void setRootSector(uint32_t rootSector) { rootSector_ = rootSector; }
  [[nodiscard]] uint32_t rootSector() const { return rootSector_; }

  /**
   * @brief Performs an exact match search for a title.
   * @param title Title to find.
   * @param outOffset Populated with the byte offset on success.
   * @return true if exact title is found, false otherwise.
   */
  bool search(const char* title, uint64_t& outOffset);

  /**
   * @brief Performs a prefix search for matching articles.
   * @param prefix Search prefix string.
   * @param results Array to store results.
   * @param maxResults Maximum results to return.
   * @return Count of matching results found.
   */
  int searchPrefix(const char* prefix, WikiSearchResult* results, int maxResults);

  /**
   * @brief Selects a random article from the B-Tree index.
   * @param outResult Populated with the selected article title and offset.
   * @return true if a valid article was found, false otherwise.
   */
  bool getRandomArticle(WikiSearchResult& outResult);

  /**
   * @brief Tests whether a 512-byte buffer is structurally a valid B-Tree node.
   */
  static bool isValidNode(const uint8_t* buffer);

 private:
  SdRawReader& reader_;
  uint32_t rootSector_;


  static constexpr size_t MAX_DEPTH = 20;
};
