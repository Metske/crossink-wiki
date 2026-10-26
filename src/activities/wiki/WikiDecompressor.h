#pragma once

#include <cstddef>
#include <cstdint>
#include <uzlib.h>
#include "SdRawReader.h"
#include "WikiShared.h"

/**
 * @class WikiDecompressor
 * @brief Streams and decompresses GZIP data from raw SD sectors directly into a buffer.
 * 
 * Strict Constraint: Uses the shared heap-allocated WikiScratchBuffer and zero static (.bss) RAM when inactive.
 */
class WikiDecompressor {
 public:
  explicit WikiDecompressor(SdRawReader& reader);
  ~WikiDecompressor();

  /**
   * @brief Initializes the decompressor at the specified raw sector and byte offset.
   * @param gzipSector LBA sector where the GZIP stream begins.
   * @param byteOffsetInSector Byte offset within the starting sector (0-511).
   * @return true on success, false if GZIP header is invalid.
   */
  bool begin(uint32_t gzipSector, uint32_t byteOffsetInSector = 0);

  /**
   * @brief Initializes the decompressor at the specified cluster offset.
   */
  bool beginCluster(uint32_t chunksSector, uint64_t articleOffset);

  /**
   * @brief Decompresses the next chunk of text.
   * @param outBuffer Destination buffer for decompressed text.
   * @param maxOutBytes Maximum bytes to decompress.
   * @return Number of bytes actually decompressed, or 0 on EOF/error.
   */
  size_t read(char* outBuffer, size_t maxOutBytes);

  [[nodiscard]] bool isDone() const { return isDone_ || (isClusterMode_ && clusterBytesRead_ >= clusterArticleLength_); }

 private:
  WikiScratchScope scratchScope_;
  SdRawReader& reader_;
  uint32_t currentSector_ = 0;
  uint32_t sectorOffset_ = 0;

  bool isClusterMode_ = false;
  uint32_t clusterArticleLength_ = 0;
  uint32_t clusterBytesRead_ = 0;

  alignas(4) uint8_t sdBuffer_[512];
  uzlib_uncomp decomp_;
  bool isDone_ = false;

  static int readCallback(struct uzlib_uncomp* uncomp);
  int readNextByte();
};
