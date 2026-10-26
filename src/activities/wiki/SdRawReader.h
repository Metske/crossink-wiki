#pragma once

#include <cstddef>
#include <cstdint>

#ifndef ESP_PLATFORM
#include <cstdio>
#endif

/**
 * @class SdRawReader
 * @brief Reads raw 512-byte sectors from the SD card block device without VFS overhead.
 * 
 * Strict Constraint: Zero dynamic memory allocation during reads.
 */
class SdRawReader {
 public:
  SdRawReader() = default;
  ~SdRawReader() { unmount(); }

  /**
   * @brief Mounts the SD card in raw sector mode.
   * @return true on success, false otherwise.
   */
  bool mount(int mosi = -1, int miso = -1, int sck = -1, int cs = -1);

  /**
   * @brief Reads a sequence of 512-byte sectors into a preallocated buffer.
   * @param startSector Starting LBA sector number.
   * @param sectorCount Number of 512-byte sectors to read.
   * @param buffer Preallocated memory buffer of at least (sectorCount * 512) bytes.
   * @return true on success, false on failure.
   */
  bool readSectors(size_t startSector, size_t sectorCount, uint8_t* buffer);

  /**
   * @brief Unmounts raw mode.
   */
  void unmount();

  [[nodiscard]] bool isMounted() const { return mounted_; }
  [[nodiscard]] uint32_t getSectorCount() const { return sector_count_; }

 private:
  bool mounted_ = false;
  uint32_t base_sector_ = 0;
  uint32_t sector_count_ = 0;
  bool is_file_ = false;

#ifdef ESP_PLATFORM
  void* file_ptr_ = nullptr; // Opaque pointer to FsFile to avoid #include <SdFat.h> overhead in header
#else
  FILE* raw_drive_ = nullptr;
  FILE* index_file_ = nullptr;
  FILE* chunks_file_ = nullptr;
  size_t index_sectors_ = 0;
#endif
};
