'use strict';
// Verified summaries only; exact paths avoid guessing the meaning of new options.
// Provenance and review date: docs/completion-notes.md.
const COMMAND_NOTES=Object.freeze({
 "help": "查看命令帮助",
 "info": "查看版本与硬件信息",
 "reboot": "重启设备",
 "channel": "查看或设置信道",
 "channel -s": "设置信道",
 "led": "控制 RGB 灯",
 "led -s": "设置灯光颜色",
 "led -p": "设置灯光效果",
 "list": "查看已保存的列表",
 "list -a": "接入点列表",
 "list -s": "SSID 列表",
 "list -c": "终端列表",
 "list -t": "AirTag 列表",
 "list -b": "BLE 设备列表",
 "list -f": "Flipper 列表",
 "list -i": "IP 地址列表",
 "list -p": "探测请求 SSID",
 "list -x": "Pineapple 列表",
 "list -m": "多 SSID 列表",
 "clearlist": "清空指定列表",
 "clearlist -a": "清空接入点",
 "clearlist -s": "清空 SSID",
 "recon": "记录无线观察任务",
 "recon wifi": "记录 Wi-Fi 观察",
 "recon ble": "记录 BLE 观察",
 "recon status": "查看任务状态",
 "recon stop": "停止记录任务",
 "stopscan": "停止扫描或攻击",
 "stopscan -f": "停止并断开 WLAN",
 "ls": "列出 SD 卡目录",
 "settings": "查看或修改设置",
 "geofence": "管理地理围栏",
 "update": "从 SD 卡更新固件",
 "protocolinfo": "查询协议与固件能力",
 "backupspiffs": "备份 SPIFFS 到 SD 卡",
 "backupstatus": "检查 SD 卡备份",
 "restorespiffs": "从 SD 卡恢复 SPIFFS",
 "gpsdata": "显示 GPS 定位数据",
 "gps": "查询 GPS 与配置输出",
 "nmea": "输出 GPS 原始语句",
 "gpspoi": "记录 GPS 兴趣点",
 "gpstracker": "记录 GPS 轨迹",
 "evilportal": "创建仿冒登录门户",
 "karma": "按探测 SSID 创建门户",
 "packetcount": "统计选中目标帧数",
 "pingscan": "探测局域网在线主机",
 "arpscan": "发现局域网 ARP 邻居",
 "portscan": "扫描主机服务端口",
 "foxhunt": "根据信号强度寻找目标",
 "scanall": "发现 AP 与 STA",
 "scancombined": "发现 AP、STA 与 Beacon",
 "sniffraw": "捕获原始 Wi-Fi 帧",
 "sniffbeacon": "监听 AP 信标帧",
 "sniffprobe": "监听终端探测请求",
 "sniffpwn": "发现 Pwnagotchi",
 "sniffpinescan": "检测 Wi-Fi Pineapple",
 "sniffmultissid": "检测同 MAC 多 SSID",
 "sniffdeauth": "监听断开认证帧",
 "sniffpmkid": "捕获 PMKID / EAPOL",
 "sniffsae": "监听 WPA3 SAE 提交帧",
 "remoteid": "接收无人机 Remote ID",
 "mactrack": "跟踪 MAC 观察记录",
 "attack": "发送 Wi-Fi 攻击帧",
 "select": "选中或取消列表目标",
 "ssid": "生成或移除 SSID",
 "save": "保存列表到 SD 卡",
 "load": "从 SD 卡读取列表",
 "join": "连接 Wi-Fi 网络",
 "randapmac": "随机设置本机 AP MAC",
 "randstamac": "随机设置本机 STA MAC",
 "cloneapmac": "复制目标 MAC 到本机 AP",
 "clonestamac": "复制目标 MAC 到本机 STA",
 "add": "手动添加 AP 或终端",
 "upload": "上传 Wardrive 记录",
 "sniffbt": "扫描 BLE 设备",
 "blespam": "发送 BLE 广播测试包",
 "findmy": "触发 Find My 设备提示音",
 "spoofat": "复制 AirTag 广播",
 "sniffskim": "检测疑似蓝牙盗刷器",
 "brightness": "查看或调整屏幕背光",
 "settings -s": "启用或禁用设置",
 "settings -r": "恢复默认设置",
 "save -a": "保存 AP 列表",
 "save -s": "保存 SSID 列表",
 "load -a": "读取 AP 列表",
 "load -s": "读取 SSID 列表",
 "ssid -a": "添加 SSID",
 "ssid -r": "移除 SSID",
 "ssid -a -g": "生成随机名称",
 "ssid -a -n": "指定网络名称",
 "select -a": "切换 AP 选择",
 "select -c": "切换终端选择",
 "select -s": "切换 SSID 选择",
 "select -f": "按名称筛选",
 "update -s": "从 SD 卡刷入固件",
 "remoteid scan": "发现 Remote ID 广播",
 "remoteid list": "查看无人机列表",
 "remoteid track": "跟踪指定设备",
 "remoteid clear": "清空 Remote ID 列表",
 "brightness -s": "设置屏幕背光等级",
 "brightness -c": "循环屏幕背光等级"
});
// Exact translations of descriptions observed in device help; changed wording remains authoritative.
const HELP_NOTE_TRANSLATIONS=Object.freeze({
 'copy SPIFFS to /spiffs on SD':'备份 SPIFFS 到 SD 卡',
 'inspect /spiffs on SD':'检查 SD 卡备份',
 'restore SPIFFS from /spiffs on SD':'从 SD 卡恢复 SPIFFS'
});
function commandNote(path,catalog){
 const live=catalog.notes?.get(path);
 return live?(HELP_NOTE_TRANSLATIONS[live]||live):(COMMAND_NOTES[path]||'');
}
// Interpret the device's help notation. Never treat suggestions as an allowlist.
function splitHelp(text){
 const parts=[];let start=0,square=0,angle=0,quote='';
 for(let i=0;i<text.length;i++){
  const c=text[i];if(quote){if(c===quote&&text[i-1]!=='\\')quote='';continue;}
  if(c==='"'||c==="'"){quote=c;continue;}
  if(c==='[')square++;else if(c===']')square--;else if(c==='<')angle++;else if(c==='>')angle=Math.max(0,angle-1);
  else if((c==='/'||c==='|')&&!square&&!angle){parts.push(text.slice(start,i));start=i+1;}
 }
 parts.push(text.slice(start));return parts;
}
function helpRoutes(text,depth=0){
 if(depth>12)return [];
 text=text.trim();
 const alternatives=splitHelp(text);
 if(alternatives.length>1)return alternatives.flatMap(s=>helpRoutes(s,depth+1)).slice(0,256);
 let routes=[[]];
 for(let i=0;i<text.length;){
  if(/\s/.test(text[i])||text[i]==='>'){i++;continue;}
  let choices=[];const open=text[i];
  if(open==='['||open==='<'){
   const close=open==='['?']':'>';let end=i+1,n=1;
   for(;end<text.length&&n;end++){if(text[end]===open)n++;if(text[end]===close)n--;}
   const inner=text.slice(i+1,n?text.length:end-1).trim();i=end;
   if(open==='['){
    choices=/^[a-z][\w-]*$/.test(inner)?[[{kind:'arg',text:'['+inner+']'}]]:helpRoutes(inner,depth+1);
    choices=[[],...choices];
   }else if(splitHelp(inner).length>1)choices=helpRoutes(inner,depth+1);
   else choices=[[{kind:'arg',text:'<'+inner+'>'}]];
  }else if(open==='"'||open==="'"){
   let end=i+1;while(end<text.length&&(text[end]!==open||text[end-1]==='\\'))end++;
   choices=[[{kind:'arg',text:text.slice(i,Math.min(text.length,end+1))}]];i=end+1;
  }else{
   const match=text.slice(i).match(/^[^\s\[\]<>]+/);if(!match){i++;continue;}
   choices=[[{kind:'literal',text:match[0]}]];i+=match[0].length;
  }
  routes=routes.flatMap(a=>choices.map(b=>a.concat(b))).slice(0,256);
 }
 return routes;
}
function compileHelp(lines){
 const map=new Map();map.notes=new Map();
 for(let line of lines){
  const description=line.match(/\s+-\s+(.+)$/);
  line=line.replace(/\s+-\s+.*$/,'').trim();const m=line.match(/^([a-z][a-z0-9_-]*)(?:\s+(.*))?$/);if(!m)continue;
  if(description)map.notes.set(m[1],description[1].trim());
  const tail=(m[2]||'').replace(/^\(.*\)$/,'');
  if(!map.has(m[1]))map.set(m[1],[]);map.get(m[1]).push(...helpRoutes(tail.replace(/(-{1,2}[a-z][\w-]*)(?:\/-{1,2}[a-z][\w-]*)+/gi,m=>'<'+m+'>')));
 }
 return map;
}
function commandMatches(value,caret,catalog,showEmpty=false){
 if(caret!==value.length)return [];
 if(!/\s/.test(value)){
  if(!value&&!showEmpty)return [];
  return [...catalog.keys()].filter(name=>name.startsWith(value.toLowerCase())).sort().map(path=>({path,syntax:'',insert:path+' '}));
 }
 if(!/^\S+\s/.test(value))return [];
 const words=value.match(/"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|\S+/g)||[];
 const root=words.shift();const trailing=/\s$/.test(value);const partial=trailing?'':(words.pop()||'');
 const routes=catalog.get(root)||[];const result=new Map();
 for(const route of routes){
  if(words.length>route.length||words.some((w,i)=>route[i].kind==='literal'&&route[i].text!==w))continue;
  const next=route[words.length];if(!next)continue;
  const prefix=[root,...words];const rest=route.slice(words.length+1).map(t=>t.text).join(' ');
  if(next.kind==='literal'){
   if(!next.text.toLowerCase().startsWith(partial.toLowerCase()))continue;
   const path=prefix.concat(next.text).join(' ');const key='word:'+path;
   // Display the shortest useful suffix; following levels appear after Tab/space.
   const syntax=route[words.length+1]?.kind==='arg'?rest:'';
   if(!result.has(key)||syntax.length<result.get(key).syntax.length)result.set(key,{path,syntax,insert:path+' '});
  }else{
   const path=prefix.join(' ');const syntax=[next.text,rest].filter(Boolean).join(' ');const key='hint:'+path;
   if(!result.has(key)||syntax.length<result.get(key).syntax.length)result.set(key,{path,syntax,insert:null});
  }
 }
 return [...result.values()].sort((a,b)=>Number(a.insert===null)-Number(b.insert===null)||a.path.localeCompare(b.path));
}
class CommandCompletion{
 constructor(input,list,source){
  this.input=input;this.list=list;this.source=source;this.items=[];this.index=0;this.signature='';this.catalog=new Map();
  input.addEventListener('input',()=>this.refresh());input.addEventListener('focus',()=>this.refresh());input.addEventListener('click',()=>this.refresh());input.addEventListener('blur',()=>this.hide());
  input.addEventListener('keydown',e=>{
   if(e.key==='Escape'){if(!this.list.hidden){e.preventDefault();e.stopPropagation();}this.hide();return;}
   if(!['Tab','ArrowDown','ArrowUp'].includes(e.key)||e.ctrlKey||e.metaKey||e.altKey||e.shiftKey)return;
   const opening=this.list.hidden;
   if(opening)this.refresh(e.key!=='Tab');const selectable=this.items.map((x,i)=>x.insert!==null?i:-1).filter(i=>i>=0);if(!selectable.length)return;
   e.preventDefault();if(e.key==='Tab'){this.choose();return;}
   if(opening){this.index=e.key==='ArrowUp'?selectable.at(-1):selectable[0];this.paint();return;}
   const pos=selectable.indexOf(this.index);this.index=selectable[(pos+(e.key==='ArrowDown'?1:-1)+selectable.length)%selectable.length];this.paint();
  });
  list.addEventListener('mousedown',e=>{const item=e.target.closest('[role=option]');if(!item)return;e.preventDefault();this.index=Number(item.dataset.index);this.choose();});
  this.resize=new ResizeObserver(()=>this.compact());this.resize.observe(list);
 }
 load(){const lines=this.source();const signature=JSON.stringify(lines);if(signature!==this.signature){this.signature=signature;this.catalog=compileHelp(lines);}}
 refresh(showEmpty=false){this.load();this.items=commandMatches(this.input.value,this.input.selectionStart,this.catalog,showEmpty);this.index=0;this.paint();}
 compact(){
  for(const row of this.list.children){row.classList.remove('compact');const parts=[...row.children].filter(e=>e.getClientRects().length);const width=parts.reduce((n,e)=>n+(e.classList.contains('completion-note')?e.clientWidth:e.scrollWidth),0)+Math.max(0,parts.length-1)*7+20;row.classList.toggle('compact',width>row.clientWidth);}
 }
 paint(){
  this.list.replaceChildren();this.list.hidden=!this.items.length;this.input.setAttribute('aria-expanded',String(!!this.items.length));
  if(!this.items.length){this.input.removeAttribute('aria-activedescendant');return;}
  this.items.forEach((candidate,i)=>{
   const row=document.createElement('div');row.role='option';row.id='command-option-'+i;row.dataset.index=i;
   row.setAttribute('aria-selected',String(i===this.index&&candidate.insert!==null));row.setAttribute('aria-disabled',String(candidate.insert===null));
   row.title=[candidate.path,candidate.syntax].filter(Boolean).join(' ');
   const words=candidate.path.split(' '),last=words.pop();
   if(words.length){const prefix=document.createElement('span');prefix.className='completion-prefix';prefix.textContent=words.join(' ');row.append(prefix);}
   const word=document.createElement('strong');word.textContent=last;row.append(word);
   if(candidate.syntax){const syntax=document.createElement('span');syntax.className='completion-syntax';syntax.textContent=candidate.syntax;row.append(syntax);}
   const note=commandNote(candidate.path,this.catalog);
   if(note){const annotation=document.createElement('span');annotation.className='completion-note';annotation.textContent=note;annotation.title=note;row.append(annotation);}
   this.list.append(row);
  });
  this.compact();const selected=this.list.children[this.index];this.input.setAttribute('aria-activedescendant',selected.id);selected.scrollIntoView({block:'nearest'});
 }
 apply(value){this.input.value=value;this.input.setSelectionRange(value.length,value.length);this.refresh();}
 choose(){const item=this.items[this.index];if(item?.insert)this.apply(item.insert);}
 hide(){this.items=[];this.list.hidden=true;this.input.setAttribute('aria-expanded','false');this.input.removeAttribute('aria-activedescendant');}
}
if(typeof module!=='undefined')module.exports={commandMatches,compileHelp,helpRoutes,commandNote,CommandCompletion};
