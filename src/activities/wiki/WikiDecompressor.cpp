#include "WikiDecompressor.h"

#include <Logging.h>
#include <cstddef>
#include <cstring>

static const char* TAG = "WikiDecompressor";

static WikiDecompressor* s_activeDecompressor = nullptr;

WikiDecompressor::WikiDecompressor(SdRawReader& reader) : reader_(reader) {
  WikiScratchBuffer::beginUse();
  std::memset(&decomp_, 0, sizeof(decomp_));
  uzlib_init();
}

WikiDecompressor::~WikiDecompressor() {
  if (s_activeDecompressor == this) {
    s_activeDecompressor = nullptr;
  }
  WikiScratchBuffer::endUse();
}

int WikiDecompressor::readCallback(struct uzlib_uncomp*) {
  if (s_activeDecompressor) {
    return s_activeDecompressor->readNextByte();
  }
  return -1;
}

int WikiDecompressor::readNextByte() {
  if (sectorOffset_ >= 512) {
    currentSector_++;
    if (!reader_.readSectors(currentSector_, 1, sdBuffer_)) {
      return -1;
    }
    sectorOffset_ = 0;
  }
  return sdBuffer_[sectorOffset_++];
}

bool WikiDecompressor::beginCluster(uint32_t chunksSector, uint64_t articleOffset) {
  uint32_t offset32 = (articleOffset >> 32);
  uint32_t uncompressedOffset = articleOffset & 0xFFFFFFFF;
  
  uint64_t absoluteBytes = static_cast<uint64_t>(offset32) * 32;
  uint32_t gzipSector = chunksSector + (absoluteBytes / 512);
  uint32_t byteOffsetInSector = absoluteBytes % 512;

  LOG_INF(TAG, "beginCluster: chunksSector=%u, articleOffset=%llu (offset32=%u, uncompOff=%u, gzipSector=%u, byteOff=%u)",
          chunksSector, articleOffset, offset32, uncompressedOffset, gzipSector, byteOffsetInSector);

  if (!begin(gzipSector, byteOffsetInSector)) {
    LOG_ERR(TAG, "beginCluster: begin failed at sector %u offset %u", gzipSector, byteOffsetInSector);
    return false;
  }

  isClusterMode_ = false;
  clusterArticleLength_ = 0;
  clusterBytesRead_ = 0;

  if (uncompressedOffset != 0xFFFFFFFF) {
     // Discard uncompressedOffset bytes without cluster mode restriction
     char discardBuf[512];
     uint32_t remaining = uncompressedOffset;
     while (remaining > 0) {
         size_t toRead = (remaining > sizeof(discardBuf)) ? sizeof(discardBuf) : remaining;
         size_t readCount = read(discardBuf, toRead);
         if (readCount == 0) {
             LOG_ERR(TAG, "beginCluster: discard failed, remaining %u", remaining);
             return false;
         }
         remaining -= readCount;
     }

     // Read 4-byte length header
     uint8_t lenHeader[4];
     uint32_t lenRemaining = 4;
     while (lenRemaining > 0) {
         size_t readCount = read(reinterpret_cast<char*>(lenHeader + 4 - lenRemaining), lenRemaining);
         if (readCount == 0) {
             LOG_ERR(TAG, "beginCluster: read lenHeader failed");
             return false;
         }
         lenRemaining -= readCount;
     }
     
     clusterArticleLength_ = lenHeader[0] | (lenHeader[1] << 8) | (lenHeader[2] << 16) | (lenHeader[3] << 24);
     LOG_INF(TAG, "beginCluster: clusterArticleLength_=%u (header bytes %02x %02x %02x %02x)",
             clusterArticleLength_, lenHeader[0], lenHeader[1], lenHeader[2], lenHeader[3]);
     
     // Now enable cluster mode to restrict subsequent reads
     isClusterMode_ = true;
  }

  return true;
}

bool WikiDecompressor::begin(uint32_t gzipSector, uint32_t byteOffsetInSector) {
  uint8_t* dictBuf = WikiScratchBuffer::get();
  if (!dictBuf) {
    LOG_ERR(TAG, "Failed to acquire scratch dictionary buffer");
    return false;
  }

  s_activeDecompressor = this;
  currentSector_ = gzipSector;
  sectorOffset_ = byteOffsetInSector;
  isDone_ = false;

  if (!reader_.readSectors(currentSector_, 1, sdBuffer_)) {
    LOG_ERR(TAG, "Failed to read initial sector %u", currentSector_);
    return false;
  }

  uzlib_uncompress_init(&decomp_, dictBuf, 32768);

  decomp_.source = nullptr;
  decomp_.source_limit = nullptr;
  decomp_.source_read_cb = WikiDecompressor::readCallback;

  // Parse GZIP header
  uint8_t gzipHeader[10];
  for (int i = 0; i < 10; ++i) {
    int b = readNextByte();
    if (b < 0) return false;
    gzipHeader[i] = static_cast<uint8_t>(b);
  }

  if (gzipHeader[0] != 0x1F || gzipHeader[1] != 0x8B || gzipHeader[2] != 0x08) {
    LOG_ERR(TAG, "Invalid GZIP magic: %02x %02x %02x at sector %u offset %u",
            gzipHeader[0], gzipHeader[1], gzipHeader[2], gzipSector, byteOffsetInSector);
    return false;
  }

  uint8_t flags = gzipHeader[3];
  if (flags & 0x04) {  // FEXTRA
    int len1 = readNextByte();
    int len2 = readNextByte();
    int xlen = len1 | (len2 << 8);
    for (int i = 0; i < xlen; ++i) readNextByte();
  }
  if (flags & 0x08) {  // FNAME
    while (readNextByte() > 0) {}
  }
  if (flags & 0x10) {  // FCOMMENT
    while (readNextByte() > 0) {}
  }
  if (flags & 0x02) {  // FHCRC
    readNextByte();
    readNextByte();
  }

  return true;
}

size_t WikiDecompressor::read(char* outBuffer, size_t maxOutBytes) {
  if (isDone() || !outBuffer || maxOutBytes == 0) return 0;
  s_activeDecompressor = this;

  if (isClusterMode_) {
      uint32_t remaining = clusterArticleLength_ - clusterBytesRead_;
      if (maxOutBytes > remaining) {
          maxOutBytes = remaining;
      }
  }

  decomp_.dest = reinterpret_cast<unsigned char*>(outBuffer);
  decomp_.dest_start = reinterpret_cast<unsigned char*>(outBuffer);
  decomp_.dest_limit = reinterpret_cast<unsigned char*>(outBuffer) + maxOutBytes;

  int res = uzlib_uncompress(&decomp_);

  size_t produced = decomp_.dest - reinterpret_cast<unsigned char*>(outBuffer);

  if (isClusterMode_) {
      clusterBytesRead_ += produced;
  }

  if (res == TINF_DONE) {
    isDone_ = true;
  } else if (res != TINF_OK && produced == 0) {
    LOG_ERR(TAG, "Decompression stream error code: %d", res);
    return 0;
  }

  return produced;
}
