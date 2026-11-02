#include "../../esp32_marauder/StorageRemovalPosix.h"
#include <assert.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
using namespace storage_removal;
namespace disk=std::filesystem;
struct FakeFilesystem {
 std::map<std::string,Node> nodes;uint32_t erased=0,budget=100000,checks=0;std::string failErase,badChild;
 bool proceed(){return checks++<budget;}
 Node inspect(const char* p){auto i=nodes.find(p);return i==nodes.end()?Node::Missing:i->second;}
 Child firstChild(const char* p,char* out,size_t n){
  if(!badChild.empty()){snprintf(out,n,"%s",badChild.c_str());return Child::Found;}
  std::string prefix=std::string(p)+"/";
  for(auto& node:nodes)if(node.first.rfind(prefix,0)==0){auto name=node.first.substr(prefix.size());if(name.find('/')==std::string::npos){snprintf(out,n,"%s",name.c_str());return Child::Found;}}
  return Child::End;
 }
 bool erase(const char* p,bool){if(failErase==p)return false;++erased;return nodes.erase(p)==1;}
};
void code(Result r,const char* expected,uint32_t removed=0){assert(r.error&&std::string(r.error)==expected);assert(r.removed==removed);}
int main(){
 // Real POSIX files exercise precisely the mounted VFS adapter used on the board.
 char temporary[]="/tmp/marauder-removal-XXXXXX";char* mount=mkdtemp(temporary);assert(mount);
 PosixFilesystem fs(mount);auto root=disk::path(mount);
 std::ofstream(root/"preserve")<<"outside trash";
 assert(!run(fs,"/.trash",true).error); // Missing trash is already empty, no mkdir.
 disk::create_directories(root/".trash"/"tree"/"sub");
 std::ofstream(root/".trash"/"tree"/"sub"/"binary",std::ios::binary).write("\0\xff\1",3);
 std::ofstream(root/".trash"/"保留.pcap")<<"until explicit empty";
 auto r=run(fs,"/.trash/tree",false);assert(!r.error&&r.removed==3);
 assert(disk::exists(root/".trash"/"保留.pcap")&&disk::exists(root/"preserve"));
 code(run(fs,"/.trash/missing",false),"missing");
 disk::create_directory_symlink(root,root/".trash"/"link");
 code(run(fs,"/.trash/link",false),"delete_io");disk::remove(root/".trash"/"link");
 r=run(fs,"/.trash",true);assert(!r.error&&r.removed==1);assert(disk::is_directory(root/".trash"));
 assert(!run(fs,"/.trash",true).error&&disk::exists(root/"preserve"));
 disk::remove_all(root);
 FakeFilesystem fake;fake.nodes={{"/",Node::Directory},{"/.trash",Node::Directory},{"/.trash/a",Node::File},{"/.trash/b",Node::File},{"/keep",Node::File}};
 for(const char* p:{"/","/.trash","/keep","/.trashable/x"})code(run(fake,p,false),"protected_path");
 for(const char* p:{"/","/.trash/a","/keep"})code(run(fake,p,true),"protected_path");
 for(const char* p:{"/.trash/../keep","/.trash//a","/.trash/./a","/.trash/a/","/.trash/a\n","/.trash/a\\b"})code(run(fake,p,false),"path");
 assert(fake.erased==0);
 fake.failErase="/.trash/b";code(run(fake,"/.trash",true),"delete_partial",1);assert(fake.nodes.count("/.trash/b")&&fake.nodes.count("/keep"));
 fake.failErase="";assert(!run(fake,"/.trash",true).error);
 fake.nodes["/.trash/x"]=Node::File;fake.budget=fake.checks;code(run(fake,"/.trash",true),"delete_limit");assert(fake.nodes.count("/.trash/x"));
 fake.budget=100000;fake.badChild="../keep";code(run(fake,"/.trash",true),"path");assert(fake.nodes.count("/keep"));fake.badChild="";
 std::string deep="/.trash";
 for(unsigned i=0;i<70;i++){deep+="/a";fake.nodes[deep]=Node::Directory;}
 assert(!run(fake,"/.trash",true).error); // No recursive stack growth.
 for(unsigned i=0;i<ENTRY_LIMIT+1;i++)fake.nodes["/.trash/"+std::to_string(i)]=Node::File;
 code(run(fake,"/.trash",true),"delete_limit_partial",ENTRY_LIMIT);assert(fake.nodes.size()==4);
 assert(!run(fake,"/.trash",true).error);assert(fake.nodes.size()==3);
 puts("Storage removal: actual files/directories, protected paths, partial errors, missing trash, deep trees and bounded work passed");
}
