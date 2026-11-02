#pragma once
#include "configs.h"
#include "FS.h"
#include "SD.h"
constexpr const char* STORAGE_INTERNAL_MOUNT="/internal";

// Application filesystem capability. The SD backend is retained for other boards.
fs::FS& storageFS();
const char* storageBackend();
size_t storageTotalBytes();
size_t storageUsedBytes();
bool storageHasSpace(size_t bytes);
bool mountInternalStorage();
