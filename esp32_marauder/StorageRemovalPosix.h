#pragma once
#include "StorageRemoval.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace storage_removal {
// Accesses the same mounted LittleFS VFS. readdir distinguishes end from error,
// unlike Arduino openNextFile(), which can also return empty on allocation failure.
struct PosixFilesystem {
 const char* mount;char absolute[PATH_LIMIT+193];
 explicit PosixFilesystem(const char* root):mount(root){}
 bool resolve(const char* path){int n=snprintf(absolute,sizeof(absolute),"%s%s",mount,path);return n>0&&size_t(n)<sizeof(absolute);}
 bool proceed(){return true;}
 Node inspect(const char* path){
  if(!resolve(path))return Node::Error;struct stat info;
  // LittleFS has no symbolic links. Desktop tests reject links explicitly.
  #ifdef ESP_PLATFORM
  int status=stat(absolute,&info);
  #else
  int status=lstat(absolute,&info);
  #endif
  if(status)return errno==ENOENT?Node::Missing:Node::Error;
  return S_ISDIR(info.st_mode)?Node::Directory:S_ISREG(info.st_mode)?Node::File:Node::Error;
 }
 Child firstChild(const char* path,char* name,size_t capacity){
  if(!resolve(path))return Child::Error;DIR* dir=opendir(absolute);if(!dir)return Child::Error;
  Child result=Child::End;
  for(;;){
   errno=0;dirent* entry=readdir(dir);
   if(!entry){if(errno)result=Child::Error;break;}
   if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
   size_t size=strlen(entry->d_name);
   if(size>=capacity){result=Child::Error;break;}
   memcpy(name,entry->d_name,size+1);result=Child::Found;break;
  }
  if(closedir(dir))result=Child::Error;
  return result;
 }
 bool erase(const char* path,bool directory){return resolve(path)&&(directory?rmdir(absolute):unlink(absolute))==0;}
};
}
