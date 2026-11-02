"""Local-only settings and private Host_Connection temporary directories."""
import json,os,tempfile,threading
from pathlib import Path

class DeskSettings:
    def __init__(self,path):
        self.path=Path(path);self.lock=threading.RLock()
        try:self.values=json.loads(self.path.read_text())
        except (OSError,ValueError):self.values={}
        if not isinstance(self.values,dict):self.values={}
    def get(self,key,default=None):
        with self.lock:return self.values.get(key,default)
    def update(self,**changes):
        with self.lock:
            values={**self.values,**changes};self.path.parent.mkdir(parents=True,exist_ok=True)
            fd,tmp=tempfile.mkstemp(prefix='.desk-settings-',dir=self.path.parent)
            try:
                with os.fdopen(fd,'w') as file:
                    json.dump(values,file,ensure_ascii=False);file.flush();os.fsync(file.fileno())
                os.replace(tmp,self.path);self.values=values
            finally:Path(tmp).unlink(missing_ok=True)

def prepare_directory(value):
    if not isinstance(value,str) or len(value)>4096 or any(ord(c)<32 for c in value):raise ValueError('临时目录路径无效')
    if value:
        base=Path(value).expanduser()
        if not base.is_absolute():raise ValueError('请填写绝对路径，或使用 ~/ 开头')
        base=base.resolve()
    else:base=Path(tempfile.gettempdir()).resolve()
    try:
        base.mkdir(parents=True,exist_ok=True,mode=0o700)
        # A private child prevents changing permissions on a user-chosen parent
        # and isolates our cleanup from unrelated files. Creation verifies writes.
        directory=Path(tempfile.mkdtemp(prefix='marauder-host-connection-',dir=base))
    except OSError as e:raise ValueError('无法写入临时目录：'+str(e)) from e
    return dict(configured=str(base) if value else '',base_directory=str(base),directory=str(directory))

def remove_empty(directory):
    # Only called for a directory returned by prepare_directory, never a parent.
    try:Path(directory).rmdir()
    except OSError:pass
