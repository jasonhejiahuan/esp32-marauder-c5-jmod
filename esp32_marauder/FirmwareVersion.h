#pragma once
// Independent release axes: upstream changes never reset the extension version.
#define MARAUDER_UPSTREAM_VERSION "1.18.0"
#define MARAUDER_EXTENSION_VERSION "2.6.2"
#define MARAUDER_FIRMWARE_SEMVER MARAUDER_UPSTREAM_VERSION "-jmod" MARAUDER_EXTENSION_VERSION
#define MARAUDER_FIRMWARE_DISPLAY "v" MARAUDER_FIRMWARE_SEMVER
#include "C5BoardProfile.h"
