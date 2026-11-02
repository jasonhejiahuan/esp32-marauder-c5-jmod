#include "HostConnection.h"
#include "StorageBackend.h"
#ifdef MARAUDER_INTERNAL_STORAGE
#include "LittleFS.h"
#include "esp_partition.h"
#include "esp_littlefs.h"
static fs::LittleFSFS internalStorage;
static bool mounted = false;
fs::FS& storageFS() { return internalStorage; }
const char* storageBackend() { return "internal-littlefs"; }
size_t storageTotalBytes() { return mounted ? internalStorage.totalBytes() : 0; }
size_t storageUsedBytes() { return mounted ? internalStorage.usedBytes() : 0; }
bool storageHasSpace(size_t bytes) {
  // Preserve room for metadata and a clean close; capture never reformats or overwrites old files.
  const size_t total = storageTotalBytes(), used = storageUsedBytes();
  return total > used && total - used > bytes + 65536;
}
bool mountInternalStorage() {
  const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
      ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "storage");
  if (!part || part->address != MARAUDER_STORAGE_OFFSET || part->size != MARAUDER_STORAGE_SIZE) {
    host_connection::console().println("[Storage] Missing or unexpected internal partition; not formatting");
    return false;
  }
  bool blank = true;
  uint8_t block[4096];
  for (size_t offset = 0; offset < part->size && blank; offset += sizeof(block)) {
    if (esp_partition_read(part, offset, block, sizeof(block)) != ESP_OK) {
      host_connection::console().println("[Storage] Read failed; not formatting");
      return false;
    }
    for (uint8_t b : block) if (b != 0xff) { blank = false; break; }
    yield();
  }
  if (blank) {
    host_connection::console().printf("[Storage] Initializing verified blank internal volume (%u KiB)\n", unsigned(part->size / 1024));
    if (esp_littlefs_format("storage") != ESP_OK) return false;
  }
  // Existing data is never auto-formatted on a mount error.
  mounted = internalStorage.begin(false, STORAGE_INTERNAL_MOUNT, 10, "storage");
  if (!mounted) {
    host_connection::console().println("[Storage] Internal mount failed; existing data preserved");
    return false;
  }
  host_connection::console().printf("[Storage] backend=internal-littlefs total=%u used=%u bytes; SD card absent\n",
      (unsigned)storageTotalBytes(), (unsigned)storageUsedBytes());
  return true;
}
#else
fs::FS& storageFS() { return SD; }
const char* storageBackend() { return "sd"; }
size_t storageTotalBytes() { return SD.totalBytes(); }
size_t storageUsedBytes() { return SD.usedBytes(); }
bool storageHasSpace(size_t) { return true; }
bool mountInternalStorage() { return false; }
#endif
