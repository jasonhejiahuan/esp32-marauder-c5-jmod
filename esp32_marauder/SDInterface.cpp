#include "HostConnection.h"
#include "SDInterface.h"
#include "lang_var.h"

// GCOVR_EXCL_START -- requires mounted SPIFFS and SD filesystems.
namespace {
  bool removeTree(fs::FS& fs, const String& path, bool keep_root = false) {
    if (!fs.exists(path))
      return true;

    File node = fs.open(path);
    if (!node)
      return false;

    if (!node.isDirectory()) {
      node.close();
      return fs.remove(path);
    }

    File child = node.openNextFile();
    while (child) {
      String child_path = child.path();
      child.close();
      if (!removeTree(fs, child_path)) {
        node.close();
        return false;
      }
      child = node.openNextFile();
    }

    node.close();
    return keep_root || fs.rmdir(path);
  }

  String joinPath(const String& base, const String& child) {
    return base == "/" ? "/" + child : base + "/" + child;
  }

  bool copyTree(
    fs::FS& source,
    const String& source_path,
    fs::FS* destination,
    const String& destination_path,
    size_t& files_copied,
    size_t& bytes_copied,
    uint8_t& error
  ) {
    File source_node = source.open(source_path);
    if (!source_node) {
      error = 3;
      return false;
    }

    if (!source_node.isDirectory()) {
      if (destination) {
        File destination_file = destination->open(destination_path, FILE_WRITE);
        if (!destination_file) {
          source_node.close();
          error = 3;
          return false;
        }

        uint8_t buffer[512];
        while (source_node.available()) {
          size_t bytes_read = source_node.read(buffer, sizeof(buffer));
          if (bytes_read == 0 || destination_file.write(buffer, bytes_read) != bytes_read) {
            source_node.close();
            destination_file.close();
            error = 3;
            return false;
          }
          bytes_copied += bytes_read;
        }
        destination_file.close();
      }
      else
        bytes_copied += source_node.size();
      source_node.close();
      files_copied++;
      return true;
    }

    if (destination && destination_path != "/" &&
        !destination->exists(destination_path) && !destination->mkdir(destination_path)) {
      source_node.close();
      error = 3;
      return false;
    }

    File child = source_node.openNextFile();
    while (child) {
      String child_source_path = child.path();
      String child_name = child_source_path;
      if (child_name.startsWith(source_path))
        child_name.remove(0, source_path.length());
      while (child_name.startsWith("/"))
        child_name.remove(0, 1);
      child.close();

      if (!copyTree(
        source,
        child_source_path,
        destination,
        joinPath(destination_path, child_name),
        files_copied,
        bytes_copied,
        error
      )) {
        source_node.close();
        return false;
      }
      child = source_node.openNextFile();
    }

    source_node.close();
    return true;
  }

}
// GCOVR_EXCL_STOP

#ifdef HAS_C5_SD
  SDInterface::SDInterface(SPIClass* spi, int cs)
    : _spi(spi), _cs(cs) {}
#endif

bool SDInterface::initSD() {
  #ifdef MARAUDER_INTERNAL_STORAGE
    this->supported = mountInternalStorage();
    if (!this->supported) return false;
    this->cardType = CARD_NONE; // Internal filesystem, no SD card present.
    this->cardSizeMB = 0;
    this->card_sz = "Internal";
    this->sd_files = new LinkedList<String>();
    if (!storageFS().exists("/SCRIPTS")) storageFS().mkdir("/SCRIPTS");
    return true;
  #else
  #ifdef HAS_SD
    String display_string = "";

    #ifdef KIT
      pinMode(SD_DET, INPUT);
      if (digitalRead(SD_DET) != LOW) {
        this->supported = false;
        return false;
      }
    #endif

    pinMode(SD_CS, OUTPUT);

    delay(10);
    #if (defined(MARAUDER_M5STICKC)) || (defined(HAS_CYD_TOUCH)) || (defined(MARAUDER_CARDPUTER)) || (defined(MARAUDER_CARDPUTER_ADV))
      /* Set up SPI SD Card using external pin header
      StickCPlus Header - SPI SD Card Reader
                  3v3   -   3v3
                  GND   -   GND
                   G0   -   CLK
              G36/G25   -   MISO
                  G26   -   MOSI
                        -   CS (jumper to SD Card GND Pin)
      */
      #if defined(MARAUDER_M5STICKC)
        enum { SPI_SCK = 0, SPI_MISO = 36, SPI_MOSI = 26 };
      #elif defined(HAS_CYD_TOUCH) || defined(MARAUDER_CARDPUTER) || defined(MARAUDER_CARDPUTER_ADV) || defined(HAS_SEPARATE_SD)
        enum { SPI_SCK = SD_SCK, SPI_MISO = SD_MISO, SPI_MOSI = SD_MOSI };
      #else
        enum { SPI_SCK = 0, SPI_MISO = 36, SPI_MOSI = 26 };
      #endif
      #if !defined(MARAUDER_CARDPUTER) && !defined(MARAUDER_CARDPUTER_ADV)
        this->spiExt = new SPIClass();
      #else
        this->spiExt = new SPIClass(FSPI);
      #endif
      host_connection::console().println(F("Using external SPI configuration..."));
      this->spiExt->begin(SPI_SCK, SPI_MISO, SPI_MOSI, SD_CS);
      if (!SD.begin(SD_CS, *(this->spiExt))) {
    #elif defined(HAS_C5_SD)
      if (!SD.begin(SD_CS, *_spi)) {
    #else
      if (!SD.begin(SD_CS)) {
    #endif
      host_connection::console().println(F("Failed to mount SD Card"));
      this->supported = false;
      return false;
    }
    else {
      this->supported = true;
      this->cardType = SD.cardType();

      this->cardSizeMB = SD.cardSize() / (1024 * 1024);
    
      if (this->supported) {
        const int NUM_DIGITS = log10(this->cardSizeMB) + 1;

        char sz[NUM_DIGITS + 1];

        sz[NUM_DIGITS] =  0;
        for ( size_t i = NUM_DIGITS; i--; this->cardSizeMB /= 10)
        {
            sz[i] = '0' + (this->cardSizeMB % 10);
            display_string.concat((String)sz[i]);
        }
  
        this->card_sz = sz;
      }

      if (!storageFS().exists("/SCRIPTS")) {

        storageFS().mkdir("/SCRIPTS");
      }

      this->sd_files = new LinkedList<String>();
    
      return true;
  }

  #else
    return false;
  #endif
  #endif
}

File SDInterface::getFile(String path) {
  if (this->supported) {
    File file = storageFS().open(path, FILE_READ);

    //if (file)
    return file;
  }
  return File();
}

bool SDInterface::removeFile(String file_path) {
  if (storageFS().remove(file_path))
    return true;
  else
    return false;
}

// GCOVR_EXCL_START -- requires mounted SPIFFS and SD filesystems.
bool SDInterface::migrateSPIFFS(uint8_t operation, size_t& files_copied, size_t& bytes_copied, uint8_t& error) {
  files_copied = bytes_copied = error = 0;

  if (!this->supported) {
    error = 1;
    return false;
  }

  const String backup_path = "/spiffs";
  File backup = storageFS().open(backup_path);
  bool valid_backup = backup && backup.isDirectory();
  backup.close();

  if (operation == 1) {
    if (!valid_backup) {
      error = 2;
      return false;
    }
    return copyTree(storageFS(), backup_path, nullptr, "", files_copied, bytes_copied, error);
  }

  if (operation == 2) {
    if (!valid_backup) {
      error = 2;
      return false;
    }
    const String rollback_path = "/spiffs.restore-rollback";
    if (!removeTree(storageFS(), rollback_path)) {
      error = 3;
      return false;
    }
    size_t rollback_files = 0, rollback_bytes = 0;
    uint8_t rollback_error = 0;
    if (!copyTree(SPIFFS, "/", &storageFS(), rollback_path, rollback_files, rollback_bytes, rollback_error)) {
      removeTree(storageFS(), rollback_path);
      error = 3;
      return false;
    }
    bool cleared = removeTree(SPIFFS, "/", true);
    if (cleared && copyTree(storageFS(), backup_path, &SPIFFS, "/", files_copied, bytes_copied, error)) {
      removeTree(storageFS(), rollback_path);
      return true;
    }
    removeTree(SPIFFS, "/", true);
    size_t recovered_files = 0, recovered_bytes = 0;
    uint8_t recovery_error = 0;
    copyTree(storageFS(), rollback_path, &SPIFFS, "/", recovered_files, recovered_bytes, recovery_error);
    removeTree(storageFS(), rollback_path);
    error = 3;
    return false;
  }

  const String staging_path = "/spiffs.tmp";
  const String previous_path = "/spiffs.previous";

  if (!removeTree(storageFS(), staging_path) || !removeTree(storageFS(), previous_path)) {
    error = 3;
    return false;
  }

  if (!copyTree(SPIFFS, "/", &storageFS(), staging_path, files_copied, bytes_copied, error)) {
    removeTree(storageFS(), staging_path);
    return false;
  }

  if (storageFS().exists(backup_path) && !storageFS().rename(backup_path, previous_path)) {
    removeTree(storageFS(), staging_path);
    error = 3;
    return false;
  }

  if (!storageFS().rename(staging_path, backup_path)) {
    if (storageFS().exists(previous_path))
      storageFS().rename(previous_path, backup_path);
    error = 3;
    return false;
  }

  removeTree(storageFS(), previous_path);
  return true;
}
// GCOVR_EXCL_STOP

void SDInterface::listDirToLinkedList(LinkedList<String>* file_names, String str_dir, String ext) {
  if (this->supported) {
    File dir = storageFS().open(str_dir);
    while (true)
    {
      File entry = dir.openNextFile();
      if (!entry)
      {
        break;
      }

      if (entry.isDirectory())
        continue;

      String file_name = entry.name();
      if (ext != "") {
        if (file_name.endsWith(ext)) {
          file_names->add(file_name);
        }
      }
      else
        file_names->add(file_name);
    }
  }
}

bool SDInterface::listDirectory(String path, LinkedList<SDDirectoryEntry>* entries) {
  if (!this->supported || entries == nullptr)
    return false;

  File dir = storageFS().open(path);
  if (!dir || !dir.isDirectory()) {
    dir.close();
    return false;
  }

  File entry = dir.openNextFile();
  while (entry) {
    String entry_path = entry.path();
    String entry_name = entry_path;
    int slash = entry_name.lastIndexOf('/');
    if (slash >= 0)
      entry_name = entry_name.substring(slash + 1);

    entries->add(SDDirectoryEntry{entry_name, entry_path, entry.isDirectory()});
    entry.close();
    entry = dir.openNextFile();
  }

  dir.close();
  return true;
}

void SDInterface::listDir(String str_dir){
  if (this->supported) {
    File dir = storageFS().open(str_dir);
    if(!dir||!dir.isDirectory()){host_connection::console().println("Directory not found. Use ls / or storageinfo to check the mounted backend.");return;}
    size_t listed=0;
    while (true)
    {
      File entry = dir.openNextFile();
      if (! entry)
      {
        break;
      }
      //for (uint8_t i = 0; i < numTabs; i++)
      //{
      //  host_connection::console().print('\t');
      //}
      ++listed;
      host_connection::console().print(entry.name());
      host_connection::console().print("\t");
      host_connection::console().println(entry.size());
      entry.close();
    }
    host_connection::console().printf("%u entries\n",(unsigned)listed);
  }else host_connection::console().println("Storage not mounted. Run storageinfo; check free space and reboot if mounting failed.");
}

void SDInterface::runUpdate(String file_name) {
  if (file_name == "")
    file_name = "/update.bin";

  #ifdef HAS_SCREEN
    display_obj.tft.setTextWrap(false);
    display_obj.tft.setFreeFont(NULL);
    display_obj.tft.setCursor(0, TFT_HEIGHT / 3);
    display_obj.tft.setTextSize(1);
    display_obj.tft.setTextColor(TFT_WHITE);
  
    display_obj.tft.println("Opening " + file_name + "...");
  #endif

  File updateBin = storageFS().open(file_name);

  if (updateBin) {
    if(updateBin.isDirectory()){
      #ifdef HAS_SCREEN
        display_obj.tft.setTextColor(TFT_RED);
        display_obj.tft.println(F(text_table2[0]));
      #endif
      host_connection::console().print(F("Error, could not find \""));
      host_connection::console().print(file_name);
      host_connection::console().println(F("\""));
      #ifdef HAS_SCREEN
        display_obj.tft.setTextColor(TFT_WHITE);
      #endif
      updateBin.close();
      return;
    }

    size_t updateSize = updateBin.size();

    if (updateSize > 0) {
      if (!this->validateUpdate(updateBin)) {
        updateBin.close();
        return;
      }
      #ifdef HAS_SCREEN
        display_obj.tft.println(F(text_table2[1]));
      #endif
      host_connection::console().println(F("Starting update from storage. Please wait..."));
      if (!this->performUpdate(updateBin, updateSize)) {
        updateBin.close();
        return;
      }
    }
    else {
      #ifdef HAS_SCREEN
        display_obj.tft.setTextColor(TFT_RED);
        display_obj.tft.println(F(text_table2[2]));
      #endif
      host_connection::console().println(F("Error, file is empty"));
      #ifdef HAS_SCREEN
        display_obj.tft.setTextColor(TFT_WHITE);
      #endif
      updateBin.close();
      return;
    }

    updateBin.close();
    
      // whe finished remove the binary from sd card to indicate end of the process
    #ifdef HAS_SCREEN
      display_obj.tft.println(F(text_table2[3]));
    #endif
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next == nullptr || esp_ota_set_boot_partition(next) != ESP_OK) {
      #ifdef HAS_SCREEN
        display_obj.tft.setTextColor(TFT_RED);
        display_obj.tft.println(F("Could not select updated firmware"));
        display_obj.tft.setTextColor(TFT_WHITE);
      #endif
      host_connection::console().println(F("Update verified but boot partition selection failed"));
      return;
    }
     
    ESP.restart();
  }
  else {
    #ifdef HAS_SCREEN
      display_obj.tft.setTextColor(TFT_RED);
      display_obj.tft.println(F(text_table2[4]));
    #endif
    host_connection::console().println(F("Could not open /update.bin on application storage. Run storageinfo and ls /; provide a compatible verified update image or use Dashboard USB OTA."));
    #ifdef HAS_SCREEN
      display_obj.tft.setTextColor(TFT_WHITE);
    #endif
  }
}

bool SDInterface::validateUpdate(File &updateBin) {
  MarauderFirmware::MetadataScanner scanner;
  uint8_t buffer[512];
  updateBin.seek(0);

  while (updateBin.available() && !scanner.found()) {
    size_t bytes_read = updateBin.read(buffer, sizeof(buffer));
    for (size_t i = 0; i < bytes_read && !scanner.found(); i++)
      scanner.push(buffer[i]);
  }

  updateBin.seek(0);

  #ifdef ALLOW_UNVERIFIED_SD_UPDATE
    if (!scanner.found()) {
      host_connection::console().println(F("WARNING: firmware identity missing; developer override accepted update"));
      return true;
    }
  #endif

  if (!scanner.found()) {
    #ifdef HAS_SCREEN
      display_obj.tft.setTextColor(TFT_RED);
      display_obj.tft.println(F("Rejected: not a validated Marauder image"));
      display_obj.tft.setTextColor(TFT_WHITE);
    #endif
    host_connection::console().println(F("Rejected storage update: Marauder firmware identity not found"));
    return false;
  }

  const MarauderFirmware::Metadata &candidate = scanner.metadata();
  const MarauderFirmware::Metadata &current = MarauderFirmware::currentMetadata();
  if (!MarauderFirmware::metadataMatches(candidate, current)) {
    #ifdef ALLOW_UNVERIFIED_SD_UPDATE
      host_connection::console().println(F("WARNING: firmware identity mismatch; developer override accepted update"));
      return true;
    #else
      #ifdef HAS_SCREEN
        display_obj.tft.setTextColor(TFT_RED);
        display_obj.tft.println(F("Rejected: firmware is for another device"));
        display_obj.tft.setTextColor(TFT_WHITE);
      #endif
      host_connection::console().print(F("Rejected storage update: expected "));
      host_connection::console().print(current.hardware);
      host_connection::console().print(F("/"));
      host_connection::console().print(current.chip);
      host_connection::console().print(F(", got "));
      host_connection::console().print(candidate.hardware);
      host_connection::console().print(F("/"));
      host_connection::console().println(candidate.chip);
      return false;
    #endif
  }

  host_connection::console().print(F("Validated storage update for "));
  host_connection::console().print(candidate.hardware);
  host_connection::console().print(F("/"));
  host_connection::console().println(candidate.chip);
  return true;
}

bool SDInterface::performUpdate(Stream &updateSource, size_t updateSize) {
  if (Update.begin(updateSize)) {   
    #ifdef HAS_SCREEN
      display_obj.tft.println(text_table2[5] + String(updateSize));
      display_obj.tft.println(F(text_table2[6]));
    #endif
    size_t written = Update.writeStream(updateSource);
    if (written == updateSize) {
      #ifdef HAS_SCREEN
        display_obj.tft.println(text_table2[7] + String(written) + text_table2[10]);
      #endif
      host_connection::console().print(F("Written : "));
      host_connection::console().print(written);
      host_connection::console().println(F(" successfully"));
    }
    else {
      #ifdef HAS_SCREEN
        display_obj.tft.println(text_table2[8] + String(written) + "/" + String(updateSize) + text_table2[9]);
      #endif
      host_connection::console().print(F("Written only : "));
      host_connection::console().print(written);
      host_connection::console().print(F("/"));
      host_connection::console().print(updateSize);
      host_connection::console().println(F(". Retry?"));
      Update.abort();
      return false;
    }
    if (Update.end()) {
      if (Update.isFinished()) {
        return true;
      }
      else {
        #ifdef HAS_SCREEN
          display_obj.tft.setTextColor(TFT_RED);
          display_obj.tft.println(text_table2[12]);
        #endif
        host_connection::console().println(F("Update not finished? Something went wrong!"));
        #ifdef HAS_SCREEN
          display_obj.tft.setTextColor(TFT_WHITE);
        #endif
        return false;
      }
    }
    else {
      #ifdef HAS_SCREEN
        display_obj.tft.println(text_table2[13] + String(Update.getError()));
      #endif
      host_connection::console().print(F("Error Occurred. Error #: "));
      host_connection::console().println(Update.getError());
      return false;
    }

  }
  else
  {
    #ifdef HAS_SCREEN
      display_obj.tft.println(text_table2[14]);
    #endif
    host_connection::console().println(F("Not enough space to begin OTA"));
    return false;
  }
}
