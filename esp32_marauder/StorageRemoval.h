#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Iterative, constant-memory traversal. Adapter closes all handles before erase.
// A failed/limited operation may have removed children; never report it as atomic.
namespace storage_removal {
constexpr size_t PATH_LIMIT=160;
constexpr uint32_t ENTRY_LIMIT=4096;
enum class Node { Missing, File, Directory, Error };
enum class Child { End, Found, Error };
struct Result { const char* error=nullptr; uint32_t removed=0; };
inline bool validPath(const char* p) {
 size_t n=strlen(p);if(!n||n>PATH_LIMIT||p[0]!='/'||(n>1&&p[n-1]=='/'))return false;
 for(size_t i=0;i<n;i++)if(uint8_t(p[i])<32||uint8_t(p[i])==127||p[i]=='\\')return false;
 const char* part=p+1;
 while(*part){const char* end=strchr(part,'/');size_t len=end?size_t(end-part):strlen(part);
  if(!len||(len==1&&part[0]=='.')||(len==2&&part[0]=='.'&&part[1]=='.'))return false;
  if(!end)break;part=end+1;
 }return true;
}
template<class Filesystem> Result run(Filesystem& fs,const char* root,bool keepRoot) {
 Result result;
 if(!validPath(root)){result.error="path";return result;}
 if(keepRoot?strcmp(root,"/.trash")!=0:strncmp(root,"/.trash/",8)!=0){result.error="protected_path";return result;}
 char path[PATH_LIMIT+1],name[PATH_LIMIT+1];strcpy(path,root);const size_t boundary=strlen(root);
 uint32_t steps=0;
 auto fail=[&](const char* error,const char* partial){result.error=result.removed?partial:error;return result;};
 for(;;){
  if(++steps>ENTRY_LIMIT*3+2||!fs.proceed())return fail("delete_limit","delete_limit_partial");
  Node kind=fs.inspect(path);
  if(kind==Node::Missing){
   if(keepRoot&&strlen(path)==boundary&&!result.removed)return result;
   return fail("missing","delete_partial");
  }
  if(kind==Node::Error)return fail("delete_io","delete_partial");
  if(keepRoot&&strlen(path)==boundary&&kind!=Node::Directory)return fail("protected_path","delete_partial");
  if(kind==Node::Directory){
   Child child=fs.firstChild(path,name,sizeof(name));
   if(child==Child::Error)return fail("delete_io","delete_partial");
   if(child==Child::Found){
    size_t n=strlen(path),len=strlen(name);
    if(!len||strchr(name,'/')||n+1+len>PATH_LIMIT)return fail("path","delete_partial");
    path[n]='/';memcpy(path+n+1,name,len+1);
    if(!validPath(path))return fail("path","delete_partial");
    continue;
   }
  }
  if(keepRoot&&strlen(path)==boundary)return result;
  if(result.removed==ENTRY_LIMIT)return fail("delete_limit","delete_limit_partial");
  if(!fs.erase(path,kind==Node::Directory))return fail("delete_io","delete_partial");
  ++result.removed;
  if(strlen(path)==boundary)return result;
  *strrchr(path,'/')=0;
 }
}
}
