#define HAL_STORAGE_IMPL
#include "SdRawReader.h"

#include <cstring>

#include "WikiShared.h"
#ifdef ESP_PLATFORM
#define private public
#include "SDCardManager.h"
#undef private
#include <Logging.h>

static const char* TAG = "SdRawReader";

bool SdRawReader::mount(int, int, int, int) {
  if (mounted_) {
    return true;
  }

  if (!SDCardManager::getInstance().ready()) {
    LOG_ERR(TAG, "SDCardManager not ready for raw reads");
    return false;
  }

#if FREEINK_SD_SDMMC
  auto* blockDev = SDCardManager::getInstance().rawBlockDevice();
#else
  auto* blockDev = SDCardManager::getInstance().sd.card();
#endif
  if (!blockDev) {
    LOG_ERR(TAG, "rawBlockDevice unavailable from SDCardManager");
    return false;
  }

  const char* imgPath = WikiConfig::get().imagePath;
  if (imgPath[0] == '\0') {
    return false;
  }

  file_ptr_ = new FsFile();
  FsFile* file = static_cast<FsFile*>(file_ptr_);
  if (SDCardManager::getInstance().openFileForRead(TAG, imgPath, *file)) {
    if (file->isContiguous()) {
      base_sector_ = file->firstSector();
      sector_count_ = file->fileSize() / 512;
      is_file_ = false;
      file->close();
      delete file;
      file_ptr_ = nullptr;
      LOG_INF(TAG, "Mounted %s: File IS contiguous! Fast raw reads enabled at LBA %u", imgPath, (uint32_t)base_sector_);
    } else {
      base_sector_ = 0;
      sector_count_ = file->fileSize() / 512;
      is_file_ = true;
      LOG_ERR(TAG, "Mounted %s: File IS NOT contiguous! SD reads will be extremely slow. Please re-copy to a freshly formatted card.", imgPath);
    }
  } else {
    LOG_ERR(TAG, "Failed to open %s", imgPath);
    delete file;
    file_ptr_ = nullptr;
    return false;
  }

  mounted_ = true;
  return true;
}

bool SdRawReader::readSectors(size_t startSector, size_t sectorCount, uint8_t* buffer) {
  if (!mounted_ || !buffer || sectorCount == 0) {
    return false;
  }

  if (startSector + sectorCount > sector_count_) {
      return false;
  }

  if (is_file_ && file_ptr_) {
    FsFile* file = static_cast<FsFile*>(file_ptr_);
    if (!file->seekSet(static_cast<uint64_t>(startSector) * 512)) return false;
    return file->read(buffer, sectorCount * 512) == (int)(sectorCount * 512);
  }

#if FREEINK_SD_SDMMC
  auto* blockDev = SDCardManager::getInstance().rawBlockDevice();
#else
  auto* blockDev = SDCardManager::getInstance().sd.card();
#endif
  if (!blockDev) {
    return false;
  }

  if (blockDev->readSectors(startSector + base_sector_, buffer, sectorCount)) {
    return true;
  }
  blockDev->syncDevice();
  return blockDev->readSectors(startSector + base_sector_, buffer, sectorCount);
}

void SdRawReader::unmount() {
  if (mounted_) {
#ifdef ESP_PLATFORM
    if (file_ptr_) {
      FsFile* file = static_cast<FsFile*>(file_ptr_);
      file->close();
      delete file;
      file_ptr_ = nullptr;
    }
#endif
    mounted_ = false;
    LOG_INF(TAG, "Unmounted raw block device");
  }
}

#else
#include <cstdio>
#include <cstdlib>
#include <iostream>

bool SdRawReader::mount(int, int, int, int) {
  if (mounted_) return true;

  const char* envPath = std::getenv("WIKI_RAW_DRIVE");
  if (!envPath || !*envPath) envPath = std::getenv("CROSSINK_RAW_DRIVE");

  if (envPath && *envPath) {
    raw_drive_ = fopen(envPath, "rb");
    if (raw_drive_) {
      mounted_ = true;
      fseeko(raw_drive_, 0, SEEK_END); sector_count_ = ftello(raw_drive_) / 512; fseeko(raw_drive_, 0, SEEK_SET);
      std::cout << "[SdRawReader] Mounted raw drive from env: " << envPath << "\n";
      return true;
    }
  }

  const char* cfgPath = WikiConfig::get().imagePath;
  if (cfgPath[0] != '\0') {
      std::string path = cfgPath;
      if (path.length() > 0 && path[0] == '/') {
          path = "fs_" + path;
      }
      raw_drive_ = fopen(path.c_str(), "rb");
      if (raw_drive_) {
          fseeko(raw_drive_, 0, SEEK_END);
          sector_count_ = ftello(raw_drive_) / 512;
          fseeko(raw_drive_, 0, SEEK_SET);
          mounted_ = true;
          std::cout << "[SdRawReader] Mounted raw image from settings: " << path << " (sectors: " << sector_count_ << ")\n";
          return true;
      }
  }

  const char* candidatePaths[] = {
      "sdcard.img",
      "../sdcard.img",
      "fs_/sdcard.img",
  };
  for (const char* path : candidatePaths) {
    FILE* f = fopen(path, "rb");
    if (f) {
      raw_drive_ = f;
      mounted_ = true;
      fseeko(raw_drive_, 0, SEEK_END); sector_count_ = ftello(raw_drive_) / 512; fseeko(raw_drive_, 0, SEEK_SET);
      std::cout << "[SdRawReader] Mounted raw image: " << path << "\n";
      return true;
    }
  }

  index_file_ = fopen("../wiki_indexer/index.bin", "rb");
  chunks_file_ = fopen("../wiki_indexer/chunks_0.gz", "rb");
  if (!index_file_) index_file_ = fopen("wiki_indexer/index.bin", "rb");
  if (!chunks_file_) chunks_file_ = fopen("wiki_indexer/chunks_0.gz", "rb");

  if (index_file_) {
    fseeko(index_file_, 0, SEEK_END);
    off_t index_bytes = ftello(index_file_);
    index_sectors_ = (index_bytes + 511) / 512;
  }

  mounted_ = (index_file_ != nullptr && chunks_file_ != nullptr);
  if (mounted_) {
    std::cout << "[SdRawReader] Mounted mock split files (index.bin + chunks_0.gz)\n";
  }
  return mounted_;
}

bool SdRawReader::readSectors(size_t startSector, size_t sectorCount, uint8_t* buffer) {
  if (!mounted_ || !buffer || sectorCount == 0) return false;

  if (raw_drive_) {
    off_t byteOffset = static_cast<off_t>(startSector) * 512;
    if (fseeko(raw_drive_, byteOffset, SEEK_SET) != 0) return false;
    size_t bytes = fread(buffer, 1, sectorCount * 512, raw_drive_);
    if (bytes < sectorCount * 512) {
      std::memset(buffer + bytes, 0, sectorCount * 512 - bytes);
    }
    return bytes > 0;
  }

  if (index_file_ && chunks_file_) {
    for (size_t s = 0; s < sectorCount; ++s) {
      size_t sec = startSector + s;
      uint8_t* secBuf = buffer + s * 512;
      if (sec < index_sectors_) {
        fseeko(index_file_, static_cast<off_t>(sec) * 512, SEEK_SET);
        size_t rd = fread(secBuf, 1, 512, index_file_);
        if (rd < 512) std::memset(secBuf + rd, 0, 512 - rd);
      } else {
        size_t chunkSec = sec - index_sectors_;
        fseeko(chunks_file_, static_cast<off_t>(chunkSec) * 512, SEEK_SET);
        size_t rd = fread(secBuf, 1, 512, chunks_file_);
        if (rd < 512) std::memset(secBuf + rd, 0, 512 - rd);
      }
    }
    return true;
  }

  return false;
}

void SdRawReader::unmount() {
  if (!mounted_) return;
  if (raw_drive_) {
    fclose(raw_drive_);
    raw_drive_ = nullptr;
  }
  if (index_file_) {
    fclose(index_file_);
    index_file_ = nullptr;
  }
  if (chunks_file_) {
    fclose(chunks_file_);
    chunks_file_ = nullptr;
  }
  mounted_ = false;
}
#endif
