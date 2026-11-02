#pragma once
#include <Arduino.h>
#include <FS.h>
// The only application-storage validation/operation boundary, shared by both adapters.
namespace storage_service {
enum class Op : uint8_t { List=1, Stat, Read, Rename, Mkdir, Trash, Restore, Delete, EmptyTrash };
struct Request { Op op; String path, destination; uint32_t offset=0, length=1024; };
struct Entry { String name; bool directory=false; uint32_t size=0; };
struct Result {
 const char* error=nullptr; String path; uint32_t size=0, offset=0, total=0, used=0;
 int32_t next=-1; uint16_t count=0, length=0; Entry entries[20]; uint8_t data[1024];
 bool ok() const {return !error;}
};
bool validPath(const String& path);
const char* available(bool owner=false);
void execute(const Request& request, Result& result);
// A stream owns its file until close. All other mutations/radio operations are excluded.
bool leased();
const char* openStream(const String& path, File& file, uint32_t& generation);
void closeStream(File& file);
uint32_t generation();
}
