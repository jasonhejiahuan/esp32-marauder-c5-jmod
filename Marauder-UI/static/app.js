'use strict';
const $=id=>document.getElementById(id);
const esc=v=>String(v??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
let settings={transport:'sse',poll:2000,theme:'system',hideOffline:false,hideBleOffline:false,hideUnnamed:false,hideBleUnnamed:false};
try{settings={...settings,...JSON.parse(localStorage.getItem('marauder-desk')||'{}')};}catch{}
if(!['sse','poll'].includes(settings.transport))settings.transport='sse';
if(!Number.isFinite(settings.poll)||settings.poll<250||settings.poll>60000)settings.poll=2000;
if(!['system','light','dark'].includes(settings.theme))settings.theme='system';
let data=null, logs=[], seq=0, token='', source=null, pollTimer=null, fetching=false, busy=false, streamUp=false, serverUp=false, lastPaint=0, pending=null;
settings.hideOffline=settings.hideOffline===true;
const apView=new PresenceView(), stationView=new PresenceView(),bleView=new PresenceView();
settings.hideBleOffline=settings.hideBleOffline===true;
let view='overview';
const stagedSelection=new Map(),stagedBle=new Set();let selectedOnly=false,bleSelectedOnly=false;
let bleSelected=new Set();try{bleSelected=new Set(JSON.parse(localStorage.getItem('marauder-ble-selected')||'[]').filter(x=>typeof x==='string').slice(0,1000));}catch{}
let connectionModeSaving=false;let manualRefreshSaving=false;let telemetryChanging=false;let renderedMetric=null;let consoleOpen=false;const telemetryClient=crypto.randomUUID().replaceAll('-','');let leaseTimer=null;
function applyTheme(){if(settings.theme==='system')delete document.documentElement.dataset.theme;else document.documentElement.dataset.theme=settings.theme;}
applyTheme();
function saveSettings(){try{localStorage.setItem('marauder-desk',JSON.stringify(settings));}catch{notice('浏览器无法保存设置');}}
function notice(message){$('notice').textContent=message||'';$('notice').hidden=!message;}
async function api(path,body){
 const options={signal:AbortSignal.timeout(body?20000:5000)};
 if(body){options.method='POST';options.headers={'Content-Type':'application/json','X-Desk-Token':token};options.body=JSON.stringify(body);}
 const r=await fetch('/api/'+path,options);const d=await r.json();if(!r.ok)throw Error(d.error||'请求失败');return d;
}
function setBusy(b){busy=b;renderStatus();}
async function act(path,body={}){setBusy(true);notice('');try{await api(path,body);await refresh();}catch(e){notice(e.message);}finally{setBusy(false);}}
function changeView(name){
 if(name==='console'){openConsole();return;}
 view=name;document.querySelectorAll('.view').forEach(x=>x.classList.toggle('active',x.id===name));document.querySelectorAll('.nav').forEach(x=>x.classList.toggle('active',x.dataset.view===name));$('page-title').textContent=({overview:'设备概览',wlan:'WLAN 概览',bluetooth:'蓝牙',networks:'网络观察',files:'文件',settings:'设置'})[name];$('hide-offline').closest('.visibility-bar').hidden=!['wlan','networks'].includes(name);render();sendLease();if(name==='files'&&!fileBusy)loadFiles(filePath);
}
let consolePinned=false,consoleTimer=null,hoverSuppressed=false;
function openConsole(pin=false){clearTimeout(consoleTimer);if(pin)consolePinned=true;if(!consoleOpen){const previous=document.activeElement;$('console').show();consoleOpen=true;if(!pin&&previous instanceof HTMLElement)previous.focus({preventScroll:true});}renderLogs();syncConsole();sendLease();if(pin)$('command').focus();}
function syncConsole(){$('console-launcher').setAttribute('aria-expanded',String(consoleOpen));$('console-pin').setAttribute('aria-pressed',String(consolePinned));}
function closeConsole(focus=true){clearTimeout(consoleTimer);$('console').close();consoleOpen=false;consolePinned=false;hoverSuppressed=true;completion.hide();syncConsole();sendLease();if(focus)$('console-launcher').focus();}
function scheduleConsoleClose(){clearTimeout(consoleTimer);consoleTimer=setTimeout(()=>{const panel=$('console'),selection=window.getSelection();if(!consolePinned&&!panel.classList.contains('maximized')&&!panel.matches(':hover')&&!panel.contains(document.activeElement)&&!(selection?.toString()&&panel.contains(selection.anchorNode)))closeConsole(false);},500);}
$('console-launcher').addEventListener('pointerenter',e=>{if(e.pointerType==='mouse'&&!hoverSuppressed){clearTimeout(consoleTimer);consoleTimer=setTimeout(()=>openConsole(),180);}});
$('console-launcher').addEventListener('pointerleave',()=>{hoverSuppressed=false;scheduleConsoleClose();});
$('console-launcher').addEventListener('click',()=>openConsole(true));
$('console').addEventListener('pointerenter',()=>clearTimeout(consoleTimer));$('console').addEventListener('pointerleave',scheduleConsoleClose);$('console').addEventListener('focusout',scheduleConsoleClose);
$('console-pin').addEventListener('click',()=>{consolePinned=!consolePinned;syncConsole();});
async function sendLease(closed=false){if(!token||!serverUp)return;try{await api('telemetry',{client:telemetryClient,view:closed?'closed':document.hidden?'hidden':(view==='overview'||consoleOpen)?'overview':'background'});}catch{}}
document.querySelectorAll('[data-view]').forEach(b=>b.addEventListener('click',()=>changeView(b.dataset.view)));
document.querySelector('.brand').addEventListener('click',()=>changeView('overview'));
function signal(rssi){const n=rssi>=-50?4:rssi>=-65?3:rssi>=-80?2:1;return `<span class="signal" aria-hidden="true">${[1,2,3,4].map(i=>`<i class="${i<=n?'on':''}"></i>`).join('')}</span>`;}
function age(t){const s=Math.max(0,Math.floor(Date.now()/1000-t));return s<3?'刚刚':s<60?s+' 秒前':Math.floor(s/60)+' 分钟前';}
function clock(t){return new Date(t*1000).toLocaleTimeString('zh-CN',{hour12:false});}
function svg(values,label){
 const w=400,h=90,max=Math.max(1,...values),min=Math.min(0,...values),range=Math.max(1,max-min);
 const path=values.map((v,i)=>`${i?'L':'M'} ${(i*w/Math.max(1,values.length-1)).toFixed(1)} ${(h-((v-min)/range)*(h-8)-4).toFixed(1)}`).join(' ');
 return `<svg viewBox="0 0 ${w} ${h}" preserveAspectRatio="none" role="img" aria-label="${esc(label)}"><line x1="0" y1="30" x2="400" y2="30"/><line x1="0" y1="60" x2="400" y2="60"/><path d="${path}"/></svg>`;
}
function rssiSvg(history){
 if(history.length<2)return '<span>—</span>';
 const start=history[0][0],span=Math.max(1,history.at(-1)[0]-start);
 const path=history.map(([t,r],i)=>`${i?'L':'M'} ${((t-start)/span*400).toFixed(1)} ${(Math.max(0,Math.min(100,-r-20))*.8+3).toFixed(1)}`).join(' ');
 return `<svg viewBox="0 0 400 90" preserveAspectRatio="none" role="img" aria-label="RSSI 历史，纵轴负20至负120 dBm"><line x1="0" y1="30" x2="400" y2="30"/><line x1="0" y1="60" x2="400" y2="60"/><path d="${path}"/></svg>`;
}
function namedDevice(item,kind){const name=kind==='ap'?item.ssid: item.name;return item.named!==false&&!!name?.trim()&&!/^(隐藏网络|<hidden>|hidden|unknown)$/i.test(name.trim())&&name.toLowerCase()!==(item.bssid||item.mac||'').toLowerCase();}
function validStations(){return (data?.stations||[]).filter(s=>/^[0-9a-f]{2}(:[0-9a-f]{2}){5}$/i.test(s.mac)&&!(parseInt(s.mac.slice(0,2),16)&1));}
function knownStations(){return stationView.visible(validStations(),settings.hideOffline,s=>s.mac).filter(s=>!settings.hideUnnamed||namedDevice(s,'sta'));}
function sortedAPs(){return apView.visible(data?.aps||[],settings.hideOffline,a=>a.bssid).filter(a=>!settings.hideUnnamed||namedDevice(a,'ap'));}
function selectionInfo(kind,mac){return data?.selection?.[kind]?.[mac];}
function selectionBadge(kind,mac){const s=selectionInfo(kind,mac);return `${s?`<span class="index-badge">${kind==='ap'?'AP':'STA'} #${s.index}</span>`:''}${s?.selected?'<span class="selected-badge">已选中</span>':''}`;}
function renderVisibility(){
 const wlan=[...(data?.aps||[]).map(x=>[x,'ap']),...validStations().map(x=>[x,'sta'])],ble=(data?.ble||[]).map(x=>[x,'ble']);
 for(const [items,off,unnamed,prefix] of [[wlan,settings.hideOffline,settings.hideUnnamed,''],[ble,settings.hideBleOffline,settings.hideBleUnnamed,'ble-']]){
  $(prefix+'hidden-offline-count').textContent=off?items.filter(([x])=>x.online===false).length:0;
  $(prefix+'hidden-unnamed-count').textContent=unnamed?items.filter(([x,k])=>(!off||x.online!==false)&&!namedDevice(x,k)).length:0;
 }
 $('hide-unnamed').checked=settings.hideUnnamed===true;$('ble-hide-unnamed').checked=settings.hideBleUnnamed===true;
}
function renderSelection(){
 const ready=serverUp&&data?.state.verified&&data.state.capabilities?.includes('selection-v1')&&['idle','console'].includes(data.state.mode)&&!busy;
 $('confirm-selection').disabled=!ready||!stagedSelection.size;$('remove-selection').disabled=!ready||!stagedSelection.size;
 $('confirm-selection').textContent='确认选中'+(stagedSelection.size?' · '+stagedSelection.size:'');
 $('selected-only').setAttribute('aria-pressed',String(selectedOnly));$('ble-selected-only').setAttribute('aria-pressed',String(bleSelectedOnly));
 $('ble-confirm-selection').disabled=!stagedBle.size;$('ble-confirm-selection').textContent='确认选中'+(stagedBle.size?' · '+stagedBle.size:'');
}
function toggleStage(kind,mac){const key=kind+':'+mac;if(stagedSelection.has(key))stagedSelection.delete(key);else stagedSelection.set(key,{kind,mac});renderNetworks();renderSelection();}

function renderStatus(){
 const st=data?.state||{},connected=st.connected&&serverUp,active=st.manual||['scanall','sniffbeacon','scancombined','bleobserve'].includes(st.mode),verified=st.verified&&serverUp;
 $('connection').textContent=connected?'已连接':'未连接';$('connection').classList.toggle('on',!!connected);
 $('connect').textContent=busy?'处理中…':connected?'断开连接':'连接设备 ↗';$('connect').disabled=busy||!serverUp;
 $('side-status').textContent=connected?'ESP32-C5 已连接':'设备未连接';$('side-dot').classList.toggle('on',!!connected);
 $('side-firmware').textContent=st.firmware?st.firmware+' · '+(st.port?.includes('5C932')?'UART':'USB CDC'):'ESP32-C5 · USB CDC';
 $('start').disabled=busy||!verified||st.mode==='bleobserve';$('send').disabled=busy||!verified;$('mode').disabled=active||busy;$('duration').disabled=active||busy;
 const remaining=st.deadline?Math.max(0,Math.ceil(st.deadline-Date.now()/1000)):0;
 $('start').textContent=st.manual?'结束手动控制 ■':active?'停止采集 ■':'开始采集 →';
 $('task-label').textContent=active?(st.mode==='scancombined'?'AP + STA + Beacon':st.mode==='sniffbeacon'?'正在观察 Beacon':'正在发现 AP 与终端')+(st.deadline?' · 剩余 '+remaining+' 秒':''):connected?'就绪':'未连接';
 if(st.mode==='bleobserve')$('task-label').textContent='BLE 发现';
 if(st.manual)$('task-label').textContent='CLI 手动控制';
 if(connected&&['unconfirmed','restarted','connecting'].includes(st.mode))$('task-label').textContent=st.mode==='connecting'?'正在核验设备…':'设备状态待确认';
 $('stream-label').textContent=st.manual?'● CLI':active?'● 采集中':data?.aps.length?'采集已停止':'等待数据';
 $('footer-state').textContent=!serverUp?'本机服务离线 · 请启动本地服务':`${streamUp?'实时推送已连接':settings.transport==='poll'?'轮询 · '+settings.poll+' ms':'实时连接恢复中 · 轮询补偿'}${st.last_rx?' · 设备接收 '+age(st.last_rx):''}`;
 $('clear').disabled=active||busy;
 $('hide-offline').checked=settings.hideOffline;
 if(!telemetryChanging)$('telemetry-enabled').checked=st.telemetry_enabled!==false;
 for(const [id,key,fallback] of [['manual-refresh','work_refresh_interval',8],['console-refresh','work_refresh_interval',8],['idle-refresh','idle_refresh_interval',2],['console-idle-refresh','idle_refresh_interval',2]])if(!manualRefreshSaving&&document.activeElement!==$(id))$(id).value=st[key]??fallback;
 const actual=!connected?'未连接':st.transport==='DUAL_LINK'?'双线 · UART 控制 + USB 数据':st.host_connection?.endpoint===1?'单线 · UART':st.transport==='LEGACY_CLI'?'传统 CLI':'单线 · 原生 USB';
 $('connection-mode-live').textContent=actual+(st.link_status?' · '+st.link_status:'');
 $('connection-mode-override').disabled=connectionModeSaving||busy||!verified||st.manual||st.mode!=='idle'||fileBusy||st.capture?.active||st.connection_switch_allowed===false;
 if(!connectionModeSaving)$('connection-mode-override').value=st.link_override||'auto';
 $('cli-refresh-status').textContent=`空闲 ${st.idle_refresh_interval??2} 秒 / 工作 ${st.work_refresh_interval??8} 秒 · 0 暂停 · 控制台或概览可见时生效`;
 document.querySelector('[data-view="overview"]').hidden=st.telemetry_enabled===false;
 if(st.telemetry_enabled===false&&view==='overview'){changeView('wlan');return;}
 $('ble-start').disabled=busy||!verified||(!['idle','bleobserve'].includes(st.mode))||st.manual;
 $('ble-start').textContent=st.mode==='bleobserve'?'停止发现 ■':'开始发现 →';
 $('ble-clear').disabled=busy||!verified||st.mode!=='idle';
 $('ble-status-label').textContent=st.mode==='bleobserve'?'BLE · 发现中':'BLE';

}
function render(){
 if(!data){renderStatus();return;}renderStatus();renderVisibility();renderSelection();const aps=sortedAPs(),n=aps.length,n2=aps.filter(a=>a.channel<=14).length;
 $('nav-count').textContent=n;
 if(view==='wlan'){
 $('metric-aps').textContent=n;$('metric-bands').innerHTML=`${n2} <em>/</em> ${n-n2}`;
 const strongest=n?aps.reduce((a,b)=>a.rssi>b.rssi?a:b):null;
 $('metric-rssi').innerHTML=`${strongest?strongest.rssi:'—'} <em>dBm</em>`;$('strongest-name').textContent=strongest?.ssid||'—';
 const buckets=new Map(data.activity),now=Math.floor(Date.now()/1000),vals=Array.from({length:60},(_,i)=>buckets.get(now-59+i)||0),rate=vals.slice(-6,-1).reduce((a,b)=>a+b,0)/5;
 $('metric-rate').innerHTML=`${rate.toFixed(1)} <em>/s</em>`;$('activity-total').textContent=aps.reduce((a,b)=>a+b.samples,0).toLocaleString();
 $('activity-chart').innerHTML=svg(vals,'最近 60 秒，每秒收到的 AP 记录数量');
 $('station-count').textContent='终端 '+knownStations().length;
 if(n){
  const channels={};aps.forEach(a=>channels[a.channel]=(channels[a.channel]||0)+1);const max=Math.max(...Object.values(channels));
  $('channel-chart').innerHTML='<div class="bars">'+Object.entries(channels).sort((a,b)=>+a[0]-b[0]).map(([ch,count])=>`<div class="bar-col ${ch>14?'band5':''}" title="信道 ${ch}：${count} 个已观察 AP"><b>${count}</b><span class="bar" data-height="${count/max*130}"></span><small>${ch}</small></div>`).join('')+'</div>';
  $('channel-chart').querySelectorAll('.bar').forEach(b=>b.style.height=b.dataset.height+'px');
  $('top-networks').innerHTML=aps.slice(0,5).map(a=>`<button class="network-row ${a.online===false?'stale':''}" data-bssid="${a.bssid}">${signal(a.rssi)}<span class="network-name"><strong>${esc(a.ssid)}</strong><small>CH ${a.channel} · ${a.channel<=14?'2.4':'5'} GHz</small></span><span class="rssi">${a.rssi} <small>dBm</small></span></button>`).join('');
 }else{
  $('channel-chart').innerHTML='<div class="empty"><span class="empty-icon">⌁</span><strong>—</strong></div>';
  $('top-networks').innerHTML='<div class="empty compact">—</div>';
 }
 const visibleKeys=new Set(aps.map(a=>a.bssid)),events=data.events.filter(e=>!settings.hideOffline||visibleKeys.has(e.bssid));
 $('events').innerHTML=events.length?events.slice(0,5).map(e=>`<div class="event"><i></i><div><strong>${esc(e.text)}</strong><small>${esc(e.detail)}</small></div><time>${clock(e.t)}</time></div>`).join(''):'<div class="empty compact">—</div>';
 }
 if(view==='overview')renderDevice();
 if(view==='bluetooth')renderBluetooth();
 if(view==='networks')renderNetworks();
 if(view==='files')renderFileStatus();
 if(consoleOpen)renderLogs();
 if(view==='settings'){
  renderHostStorage();
  const st=data.state,rows={'固件':st.firmware||'—','构建目标':st.build_target||st.hardware||'—','ESP-IDF':st.idf||'—','串口':st.port||'未连接','PSRAM':st.psram?(st.psram.total/1048576)+' MiB · 启动值':'—','GPS':'禁用'};
  $('device-info').innerHTML=Object.entries(rows).map(([k,v])=>`<div><dt>${esc(k)}</dt><dd>${esc(v)}</dd></div>`).join('');
 }
}
function apFields(a){
 const freq=a.channel===14?2484:a.channel<=13?2407+5*a.channel:5000+5*a.channel;
 const fields=[['BSSID / MAC',a.bssid],['安全协议',a.security||'—'],['信道',a.channel],['中心频率',freq+' MHz'],['频段',(a.channel<=14?'2.4':'5')+' GHz'],['RSSI',a.rssi+' dBm']];
 if(a.brand&&a.brand!=='Unknown')fields.push(['品牌',a.brand]);
 if(a.frames!=null)fields.push(['帧数',a.frames]);
 if(a.station_count!=null)fields.push(['关联终端',a.station_count]);
 if(a.eapol_complete!=null)fields.push(['完整 EAPOL',a.eapol_complete?'是':'否']);
 if(a.capabilities)fields.push(['能力标记',a.capabilities]);
 fields.push(['MAC 地址类型',(parseInt(a.bssid.slice(0,2),16)&2)?'本地管理':'全局管理'],['观察样本',a.samples],['首次发现',clock(a.first)],['最后接收',clock(a.last)]);
 return fields;
}

// Each bulk action uses the same filtered snapshot as the rendered list.
const listSnapshots={ap:[],sta:[],ble:[]}, chosenGroups={ap:new Set(),sta:new Set(),ble:new Set()};
function listKey(kind,x){return kind==='ap'?x.bssid:kind==='ble'?x.key:x.mac;}
function listName(x){return x.ssid||x.name||x.mac||x.bssid||'';}
function listGroup(kind,x){const mode=$(kind+'-group').value;return mode==='none'?'全部':mode==='band'?(x.channel>14?'5 GHz':'2.4 GHz'):mode==='channel'?'CH '+(x.channel||'未知'):mode==='security'?(x.security||'未知'):mode==='brand'?(x.brand||'未知'):mode==='ap'?(x.ap||'未关联'):mode==='company'?(x.company>=0?'Company 0x'+x.company.toString(16).padStart(4,'0'):'未知'):mode==='connectable'?(x.connectable?'可连接':'仅广播'):(x.online===false?'掉线':'在线');}
function refineList(kind,items){
 const state=$(kind+'-state').value,min=$(kind+'-rssi').value,extra=$(kind+'-extra').value.toLowerCase(),order=$(kind+'-sort').value;
 items=items.filter(x=>(state==='all'||(state==='online'?x.online!==false:x.online===false))&&(!min||(Number.isFinite(x.rssi)&&x.rssi>=Number(min)))&&(!extra||String(kind==='ap'?`${x.security||''} ${x.brand||''}`:kind==='sta'?`${x.ap||''} ${x.name||''} ${x.mac||''}`:`${x.company>=0?x.company.toString(16):''} ${x.services||''}`).toLowerCase().includes(extra)));
 const direction=order.endsWith('-desc')?-1:1,field=order.replace(/-(asc|desc)$/,'');
 items=[...items].sort((a,b)=>{const group=$(kind+'-group').value==='none'?0:listGroup(kind,a).localeCompare(listGroup(kind,b),undefined,{numeric:true});if(group)return group;if(field==='stable')return 0;const va=field==='name'?listName(a):field==='mac'?(a.mac||a.bssid):a[field],vb=field==='name'?listName(b):field==='mac'?(b.mac||b.bssid):b[field];if(va==null)return vb==null?0:1;if(vb==null)return -1;return direction*(typeof va==='number'?va-vb:String(va).localeCompare(String(vb),undefined,{numeric:true}));});
 listSnapshots[kind]=items;renderGroupChoices(kind,items);return items;
}
function renderGroupChoices(kind,items){
 const counts=new Map();for(const x of items){const key=listGroup(kind,x);counts.set(key,(counts.get(key)||0)+1);}
 for(const key of chosenGroups[kind])if(!counts.has(key))chosenGroups[kind].delete(key);
 const box=$(kind+'-groups'),html=[...counts].map(([key,n])=>`<label class="check"><input type="checkbox" data-list-group="${kind}" value="${esc(key)}" ${chosenGroups[kind].has(key)?'checked':''}>${esc(key)} <small>${n}</small></label>`).join('');
 if(box.dataset.signature!==html){box.innerHTML=html;box.dataset.signature=html;}
 $(kind+'-visible-count').textContent=items.length+' 项';$(kind+'-select-visible').disabled=!items.length;$(kind+'-select-groups').disabled=!chosenGroups[kind].size;
}
function arrangeGroups(kind,container,items){
 const signature=$(kind+'-group').value+JSON.stringify(items.map(x=>[listKey(kind,x),listGroup(kind,x)]));if(container.dataset.layoutSignature===signature)return;container.dataset.layoutSignature=signature;
 container.querySelectorAll('.list-group-heading').forEach(x=>x.remove());let previous=null;
 for(const x of items){const key=listKey(kind,x),row=[...container.children].find(el=>(el.dataset.key||el.dataset.mac)===key);if(!row)continue;const group=listGroup(kind,x);if($(kind+'-group').value!=='none'&&group!==previous){const heading=document.createElement('h3');heading.className='list-group-heading';heading.textContent=group;container.append(heading);previous=group;}container.append(row);}
}
function stageVisible(kind,groupsOnly){
 const items=listSnapshots[kind].filter(x=>!groupsOnly||chosenGroups[kind].has(listGroup(kind,x)));
 for(const x of items){if(kind==='ble')stagedBle.add(x.key);else stagedSelection.set(kind+':'+listKey(kind,x),{kind,mac:listKey(kind,x)});}
 render();
}

function renderNetworks(){
 const q=$('search').value.toLowerCase(),band=$('band').value;
 const aps=refineList('ap',sortedAPs().filter(a=>(!selectedOnly||selectionInfo('ap',a.bssid)?.selected)&&(band==='all'||(band==='2'?a.channel<=14:a.channel>14))&&`${a.ssid} ${a.bssid} ${a.channel}`.toLowerCase().includes(q)));
 const anchor=[...$('network-list').querySelectorAll('details')].find(x=>x.getBoundingClientRect().bottom>0);
 const anchorTop=anchor?.getBoundingClientRect().top;
 // Keep native details nodes stable while data streams, including keyboard focus.
 const existing=new Map([...$('network-list').querySelectorAll('details')].map(x=>[x.dataset.key,x]));
 $('station-details').hidden=!knownStations().some(s=>!selectedOnly||selectionInfo('sta',s.mac)?.selected);
 $('network-list').querySelectorAll('.empty').forEach(x=>x.remove());
 const keys=new Set(aps.map(a=>a.bssid));existing.forEach((el,k)=>{if(!keys.has(k)){
  const matchesHistory=data.aps.some(a=>a.bssid===k&&a.online===false);
  if(false&&settings.hideOffline&&!settings.hideUnnamed&&!selectedOnly&&!q&&band==='all'&&matchesHistory&&(el.open||el.contains(document.activeElement))){el.dataset.retained='true';el.classList.add('stale');}
  else el.remove();
 }});
 for(const [i,a] of aps.entries()){
  let el=existing.get(a.bssid);
  if(!el){el=document.createElement('details');el.className='network-card';el.dataset.key=a.bssid;el.innerHTML='<summary></summary><div class="detail-content"></div>';el.addEventListener('toggle',()=>{if(!el.open&&el.dataset.retained==='true'){el.remove();}});const next=aps.slice(i+1).map(a=>existing.get(a.bssid)).find(x=>x?.isConnected);$('network-list').insertBefore(el,next||null);}
  delete el.dataset.retained;el.classList.toggle('stale',a.online===false);el.classList.toggle('staged',stagedSelection.has('ap:'+a.bssid));el.classList.toggle('selected-device',!!selectionInfo('ap',a.bssid)?.selected);
  el.querySelector('summary').innerHTML=`${signal(a.rssi)}<span class="network-name"><strong>${esc(a.ssid)}</strong><span class="network-labels">${selectionBadge('ap',a.bssid)}${a.capabilities?`<span class="capability-badge" title="802.11 能力字段（原始十六进制字节）">能力 ${esc(a.capabilities)}</span>`:''}</span><small>${a.bssid} · ${age(a.last)}</small></span><span class="badge">${esc(a.security||'—')} · CH ${a.channel}</span><span class="rssi">${a.rssi} <small>dBm</small></span>`;
  el.querySelector('.detail-content').innerHTML=`<div class="selection-row"><button class="stage-device" data-kind="ap" data-mac="${a.bssid}" aria-pressed="${stagedSelection.has('ap:'+a.bssid)}">${stagedSelection.has('ap:'+a.bssid)?'取消待选':'选择'}</button></div><dl class="ap-metadata">${apFields(a).map(([key,value])=>`<div><dt>${esc(key)}</dt><dd>${esc(value)}</dd></div>`).join('')}</dl><div><p>RSSI · dBm</p><div class="spark-chart">${rssiSvg(a.history)}</div></div>`;
 }
 arrangeGroups('ap',$('network-list'),aps);
 if(!$('network-list').querySelector('details'))$('network-list').innerHTML='<div class="panel empty"><strong>—</strong></div>';
 $('station-details').hidden=!knownStations().some(s=>!selectedOnly||selectionInfo('sta',s.mac)?.selected);
 if(anchor?.isConnected){const shift=anchor.getBoundingClientRect().top-anchorTop;if(Math.abs(shift)>1)window.scrollBy(0,shift);}
 const visibleStations=refineList('sta',knownStations().filter(s=>!selectedOnly||selectionInfo('sta',s.mac)?.selected));
 $('station-details').hidden=false;
 $('station-list').innerHTML=visibleStations.map(s=>`<div class="station-row ${s.online===false?'stale':''} ${stagedSelection.has('sta:'+s.mac)?'staged':''}" data-mac="${s.mac}"><code>${esc(s.mac)}</code> ${selectionBadge('sta',s.mac)} → <code>${esc(s.ap)}</code><button class="stage-device" data-kind="sta" data-mac="${s.mac}" aria-pressed="${stagedSelection.has('sta:'+s.mac)}">${stagedSelection.has('sta:'+s.mac)?'取消待选':'选择'}</button></div>`).join('');
 delete $('station-list').dataset.layoutSignature;arrangeGroups('sta',$('station-list'),visibleStations);
}
function renderLogs(){const atBottom=$('autoscroll').checked;const text=logs.map(l=>`${clock(l.t)} ${l.kind==='tx'?'›':l.kind==='error'?'!':' '} ${l.text}`).join('\n');if($('terminal').textContent!==text)$('terminal').textContent=text;if(atBottom)$('terminal').scrollTop=$('terminal').scrollHeight;}
function accept(d){
 serverUp=true;
 // SSE and polling may race: never replace a newer snapshot with an older one.
 if(data&&d.instance!==data.instance){logs=[];seq=0;data=null;apView.reset();stationView.reset();bleView.reset();fileEntries=[];api('session').then(s=>{token=s.token;}).catch(()=>{});}
 if(data&&d.rev<data.rev)return;
 if(d.seq<seq){logs=[];seq=0;}
 if(data?.state.host_connection?.device_id!==d.state.host_connection?.device_id){stagedSelection.clear();stagedBle.clear();}
 logs=logs.filter(l=>l.id>(d.log_cleared||0));for(const l of d.logs){if(l.id>seq&&l.id>(d.log_cleared||0))logs.push(l);}seq=Math.max(seq,d.seq);logs=logs.slice(-1000);data=d;
 if(d.state.error)notice(d.state.error);
 pending=d;const elapsed=performance.now()-lastPaint;if(elapsed>=50){lastPaint=performance.now();render();pending=null;}
}
async function refresh(){if(fetching)return;fetching=true;try{accept(await api('status?since='+seq));}catch(e){serverUp=false;renderStatus();}finally{fetching=false;}}
function updates(){
 if(source)source.close();source=null;streamUp=false;if(pollTimer)clearTimeout(pollTimer);
 if(settings.transport==='sse'){
  source=new EventSource('/api/events?since='+seq);
  source.onopen=()=>{streamUp=true;serverUp=true;renderStatus();};
  source.onmessage=e=>{try{accept(JSON.parse(e.data));}catch{notice('收到无法解析的数据。');}};
  source.onerror=()=>{streamUp=false;renderStatus();};
 }
 const poll=async()=>{if(settings.transport==='poll'||!streamUp)await refresh();pollTimer=setTimeout(poll,settings.poll);};poll();
}
async function refreshPorts(){try{const ps=await api('ports'),selected=$('port').value;$('port').innerHTML='<option value="">自动发现 ESP32</option>'+ps.map(p=>`<option value="${esc(p.path)}">${esc(p.label+' · '+p.path)}</option>`).join('');if(ps.some(p=>p.path===selected))$('port').value=selected;}catch(e){notice(e.message);}}
function download(name,content,type){const url=URL.createObjectURL(new Blob([content],{type}));const a=document.createElement('a');a.href=url;a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);}
$('connect').addEventListener('click',()=>act(data?.state.connected?'disconnect':'connect',{port:$('port').value}));
$('start').addEventListener('click',()=>act('command',{command:(data?.state.manual||['scanall','sniffbeacon','scancombined'].includes(data?.state.mode))?'stopscan':$('mode').value,duration:Number($('duration').value)}));
const completion=new CommandCompletion($('command'),$('command-suggestions'),()=>data?.state.command_help||[]);
$('command-form').addEventListener('submit',async e=>{e.preventDefault();const cmd=$('command').value.trim();if(!cmd)return;completion.hide();await act('console',{command:cmd});$('command').value='';});
$('search').addEventListener('input',renderNetworks);$('band').addEventListener('change',renderNetworks);
$('top-networks').addEventListener('click',e=>{const row=e.target.closest('[data-bssid]');if(row){$('search').value=row.dataset.bssid;changeView('networks');const el=$('network-list').querySelector('details');if(el)el.open=true;}});
$('refresh-ports').addEventListener('click',refreshPorts);
$('transport').value=settings.transport;$('poll-ms').value=settings.poll;$('theme').value=settings.theme;
$('transport').addEventListener('change',()=>{settings.transport=$('transport').value;saveSettings();updates();});
$('poll-ms').addEventListener('change',()=>{const n=Number($('poll-ms').value);if(!Number.isInteger(n)||n<250||n>60000){$('poll-ms').value=settings.poll;notice('轮询间隔须为 250–60000 毫秒。');return;}settings.poll=n;saveSettings();notice('');updates();});
$('poll-ms').addEventListener('input',()=>{const n=Number($('poll-ms').value);if(Number.isInteger(n)&&n>=250&&n<=60000){settings.poll=n;saveSettings();updates();renderStatus();}});
$('hide-offline').addEventListener('change',()=>{settings.hideOffline=$('hide-offline').checked;saveSettings();render();});
$('theme').addEventListener('change',()=>{settings.theme=$('theme').value;saveSettings();applyTheme();});
$('clear').addEventListener('click',()=>act('clear'));
$('export').addEventListener('click',()=>download('marauder-observations-'+Date.now()+'.json',JSON.stringify({exportedAt:new Date().toISOString(),state:data?.state,hideOffline:settings.hideOffline,aps:sortedAPs(),stations:knownStations()},null,2),'application/json'));
$('export-log').addEventListener('click',()=>download('marauder-console-'+Date.now()+'.txt',logs.map(l=>`${new Date(l.t*1000).toISOString()} ${l.kind} ${l.text}`).join('\n'),'text/plain'));
setInterval(()=>{if(pending){lastPaint=performance.now();render();pending=null;}else{renderStatus();if(data&&['overview','wlan','bluetooth'].includes(view))render();}},1000);
window.addEventListener('pagehide',()=>{source?.close();clearTimeout(pollTimer);clearInterval(leaseTimer);fetch('/api/telemetry',{method:'POST',headers:{'Content-Type':'application/json','X-Desk-Token':token},body:JSON.stringify({client:telemetryClient,view:'closed'}),session_refresh:true}).catch(()=>{});});
window.addEventListener('pageshow',e=>{if(e.persisted){updates();startLeases();}});
(async()=>{try{token=(await api('session')).token;await refreshPorts();await refresh();updates();changeView(view);startLeases();}catch(e){notice('请启动本机服务后刷新页面：'+e.message);}})();

function startLeases(){clearInterval(leaseTimer);sendLease();leaseTimer=setInterval(sendLease,10000);}
document.addEventListener('visibilitychange',()=>sendLease());
$('telemetry-enabled').addEventListener('change',async()=>{const enabled=$('telemetry-enabled').checked;telemetryChanging=true;$('telemetry-enabled').disabled=true;try{await act('telemetry',{enabled});}finally{telemetryChanging=false;$('telemetry-enabled').disabled=false;renderStatus();}});
$('console-close').addEventListener('click',()=>closeConsole());
$('console-max').addEventListener('click',()=>{const full=$('console').classList.toggle('maximized');$('console-max').textContent=full?'⤡':'⤢';$('console-max').setAttribute('aria-label',full?'还原控制台':'放大控制台');});
document.addEventListener('keydown',e=>{if(e.key==='Escape'&&consoleOpen&&!e.defaultPrevented&&$('command-suggestions').hidden){e.preventDefault();closeConsole();}});
$('ble-start').addEventListener('click',()=>act('command',{command:'bleobserve '+(data?.state.mode==='bleobserve'?'stop':'start')}));
$('ble-clear').addEventListener('click',()=>act('command',{command:'bleobserve clear'}));
$('ble-search').addEventListener('input',renderBluetooth);
$('ble-hide-offline').checked=settings.hideBleOffline;
$('ble-hide-offline').addEventListener('change',()=>{settings.hideBleOffline=$('ble-hide-offline').checked;saveSettings();render();});
$('ble-export').addEventListener('click',()=>download('marauder-ble-'+Date.now()+'.json',JSON.stringify({at:new Date().toISOString(),devices:visibleBluetooth(),status:data?.ble_status,hideOffline:settings.hideBleOffline},null,2),'application/json'));
function bytes(n){if(!Number.isFinite(n))return '—';return n<1024?n+' B':n>=1048576?(n/1048576).toFixed(2)+' MiB':(n/1024).toFixed(1)+' KiB';}
function percent(used,total){return total?Math.max(0,Math.min(100,used/total*100)):0;}
function meter(label,used,total){const pct=percent(used,total);return `<div class="resource"><div><strong>${esc(label)}</strong><span>${bytes(used)} / ${bytes(total)}</span></div><progress max="100" value="${pct}" aria-label="${esc(label)} ${pct.toFixed(1)}%"></progress></div>`;}
function detailRows(rows){return rows.map(([k,v])=>`<div><dt>${esc(k)}</dt><dd>${esc(v)}</dd></div>`).join('');}
function renderDevice(){
 const m=data?.metrics;if(!m){$('device-health').textContent=data?.state.verified?'—':'未连接';return;}
 $('device-health').textContent=!data.state.verified?'未连接':Date.now()/1000-m.received_at>Math.max(20,(data.state.telemetry_interval||2)*3)?'数据已过期':'在线';
 if(renderedMetric===m.received_at)return;renderedMetric=m.received_at;
 $('chip-temperature').innerHTML=m.temperature_c==null?'—':`${m.temperature_c.toFixed(1)} <em>°C</em>`;
 $('chip-frequency').innerHTML=`${m.cpu_mhz} <em>MHz</em>`;
 $('heap-percent').innerHTML=`${percent(m.heap_total-m.heap_free,m.heap_total).toFixed(1)} <em>%</em>`;
 $('psram-percent').innerHTML=`${percent(m.psram_total-m.psram_free,m.psram_total).toFixed(1)} <em>%</em>`;
 const series=(data.metric_history||[]).filter(x=>Number.isFinite(x[1]));
 if(series.length){const lo=Math.min(...series.map(x=>x[1]))-1,hi=Math.max(...series.map(x=>x[1]))+1,start=series[0][0],span=Math.max(1,series.at(-1)[0]-start);const path=series.map(([t,v],i)=>`${i?'L':'M'} ${(t-start)/span*400} ${86-(v-lo)/(hi-lo)*80}`).join(' ');$('temperature-chart').innerHTML=`<svg viewBox="0 0 400 90" preserveAspectRatio="none" aria-label="芯片温度曲线"><path d="${path}"/></svg>`;$('temperature-range').textContent=`${lo.toFixed(1)}–${hi.toFixed(1)} °C`;}
 $('metrics-time').textContent=clock(m.received_at);const secs=Math.floor(m.uptime_ms/1000);$('uptime').textContent=`运行 ${Math.floor(secs/3600)}:${String(Math.floor(secs/60)%60).padStart(2,'0')}:${String(secs%60).padStart(2,'0')}`;
 $('physical-flash').textContent=bytes(m.flash_physical);
 $('storage-meters').innerHTML=meter('内部文件',m.storage_used,m.storage_total)+meter('设置 · SPIFFS',m.settings_used,m.settings_total)+meter('应用分区',m.app_size,m.app_slot);
 $('memory-meters').innerHTML=meter('内部堆',m.heap_total-m.heap_free,m.heap_total)+meter('PSRAM',m.psram_total-m.psram_free,m.psram_total);
 $('memory-detail').innerHTML=detailRows([['最低空闲堆',bytes(m.heap_min)],['最大连续空闲块',bytes(m.heap_largest)]]);
 $('hardware-detail').innerHTML=detailRows([['固件',data.state.firmware],['芯片修订',`${Math.floor(m.revision/100)}.${m.revision%100}`],['CPU',`${m.cores} 核 · RISC-V`],['复位原因',({1:'上电',3:'软件复位',4:'异常',5:'中断看门狗',6:'任务看门狗',7:'看门狗',9:'欠压',11:'USB 复位'})[m.reset_reason]||m.reset_reason],['USB',data.state.port],['地址窗口',bytes(m.flash_window)]]);
 $('device-health').textContent=!data.state.verified?'未连接':Date.now()/1000-m.received_at>Math.max(20,(data.state.telemetry_interval||2)*3)?'数据已过期':'在线';
 $('radio-capabilities').innerHTML=(m.radios||[]).map(x=>`<span>${esc(({'wifi6-dual-band':'Wi-Fi 6 · 双频',ble:'BLE','802.15.4':'802.15.4'})[x]||x)}</span>`).join('');
}
function visibleBluetooth(){return bleView.visible(data?.ble||[],settings.hideBleOffline,d=>d.key).filter(d=>!settings.hideBleUnnamed||namedDevice(d,'ble'));}
function renderBluetooth(){
 const devices=visibleBluetooth(),status=data?.ble_status||{},q=$('ble-search').value.toLowerCase();$('ble-count').textContent=devices.length;$('ble-reports').textContent=(settings.hideBleOffline||settings.hideBleUnnamed)?devices.reduce((n,d)=>n+d.count,0):status.reports||0;$('ble-dropped').textContent=(status.dropped||0)+(status.tableFull||0);$('ble-strongest-rssi').innerHTML=devices.length?`${Math.max(...devices.map(x=>x.rssi))} <em>dBm</em>`:'—';
 const list=$('ble-list'),anchor=[...list.querySelectorAll('details')].find(e=>e.getBoundingClientRect().bottom>0),anchorTop=anchor?.getBoundingClientRect().top,existing=new Map([...list.querySelectorAll('details')].map(x=>[x.dataset.key,x]));const visible=refineList('ble',devices.filter(x=>(!bleSelectedOnly||bleSelected.has(x.key))&&`${x.name} ${x.mac} ${x.services}`.toLowerCase().includes(q)));const keys=new Set(visible.map(x=>x.key));existing.forEach((el,k)=>{if(!keys.has(k)){const old=data.ble.find(d=>d.key===k);if(false&&settings.hideBleOffline&&!settings.hideBleUnnamed&&!bleSelectedOnly&&!q&&old?.online===false&&(el.open||el.contains(document.activeElement))){el.dataset.retained='true';el.classList.add('stale');}else el.remove();}});
 for(const d of visible){let el=existing.get(d.key);if(!el){el=document.createElement('details');el.className='network-card';el.dataset.key=d.key;el.innerHTML='<summary></summary><div class="detail-content"></div>';el.addEventListener('toggle',()=>{if(!el.open&&el.dataset.retained==='true')el.remove();});const next=visible.slice(visible.indexOf(d)+1).map(x=>existing.get(x.key)).find(x=>x?.isConnected);list.insertBefore(el,next||null);}delete el.dataset.retained;el.classList.toggle('stale',d.online===false);
 el.classList.toggle('staged',stagedBle.has(d.key));el.classList.toggle('selected-device',bleSelected.has(d.key));
 const stamp=bleSelected.has(d.key)+'/'+stagedBle.has(d.key)+'/'+d.name+'/'+d.online+'/'+d.last+'/'+d.count+'/'+Math.floor((Date.now()/1000-d.last)/5);if(el.dataset.stamp===stamp)continue;el.dataset.stamp=stamp;
 el.querySelector('summary').innerHTML=`${signal(d.rssi)}<span class="network-name"><strong>${esc(d.name||d.mac)} ${bleSelected.has(d.key)?'<span class="selected-badge">已选中</span>':''}</strong><small>${esc(d.mac)} · ${age(d.last)}</small></span><span class="badge">${d.connectable?'可连接':'广播'}</span><span class="rssi">${d.rssi} <small>dBm</small></span>`;
 const rows=[['地址',d.mac],['地址类型',['Public','Random','Public identity','Random identity'][d.address_type]],['广播报告',d.count],['服务 UUID',d.services||'—'],['Company ID',d.company>=0?'0x'+d.company.toString(16).padStart(4,'0'):'—'],['TX Power',d.tx_power===127?'—':d.tx_power+' dBm'],['Appearance',d.appearance<0?'—':d.appearance],['广播长度',d.payload_length+' B'+(d.truncated?' · 截断':'')],['原始广播',d.payload||'—']];
 el.querySelector('.detail-content').innerHTML=`<div class="selection-row"><button data-ble-stage="${d.key}" aria-pressed="${stagedBle.has(d.key)}">${stagedBle.has(d.key)?'取消待选':'选择'}</button>${bleSelected.has(d.key)?`<button data-ble-remove="${d.key}">取消选择</button>`:''}</div><dl class="ap-metadata">${detailRows(rows)}</dl><div class="spark-chart">${rssiSvg(d.history||[])}</div>`;
 }
 arrangeGroups('ble',list,visible);
 if(anchor?.isConnected){const shift=anchor.getBoundingClientRect().top-anchorTop;if(Math.abs(shift)>1)window.scrollBy(0,shift);}
}

let filePath='/',fileEntries=[],fileBusy=false,fileCancel=false,fileNameAction=null,fileDeleteAction=null;
function fileReady(){return serverUp&&data?.state.verified&&data.state.mode==='idle'&&!data.state.manual&&data.state.capabilities?.includes('files-v1');}
function fileDeleteReady(){return fileReady()&&data.state.capabilities?.includes('files-delete-v1');}
function renderFileStatus(){
 for(const id of ['file-refresh','file-mkdir'])$(id).disabled=fileBusy||!fileReady();$('file-trash').disabled=fileBusy;
 const trash=filePath==='/.trash'||filePath.startsWith('/.trash/');$('file-mkdir').hidden=trash;
 $('file-empty-trash').hidden=!trash||!data?.state.capabilities?.includes('files-delete-v1');$('file-empty-trash').disabled=fileBusy||!fileDeleteReady();
 document.querySelectorAll('[data-file-action]').forEach(b=>{b.disabled=fileBusy||!fileReady();if(b.dataset.fileAction==='delete'){b.hidden=!data?.state.capabilities?.includes('files-delete-v1');b.disabled=fileBusy||!fileDeleteReady();}});
 $('file-delete-confirm').disabled=fileBusy||!fileDeleteReady();
}
function fileParent(path){return path.slice(0,path.lastIndexOf('/'))||'/';}
function renderFiles(){
 const crumbs=[['内部文件','/']],parts=filePath.split('/').filter(Boolean);parts.forEach((part,i)=>crumbs.push([part==='.trash'?'回收站':part,'/'+parts.slice(0,i+1).join('/')]));
 $('file-breadcrumbs').innerHTML=crumbs.map(([name,path])=>`<button data-file-dir="${esc(path)}">${esc(name)}</button>`).join('<span>/</span>');
 const q=$('file-search').value.toLowerCase(),trash=filePath==='/.trash'||filePath.startsWith('/.trash/');
 $('file-list').innerHTML=fileEntries.filter(f=>f.name.toLowerCase().includes(q)).map(f=>`<article class="file-row"><button class="file-title" data-file-action="${f.directory?'open':'download'}" data-path="${esc(f.path)}"><span class="file-icon">${f.directory?'▱':'▤'}</span><span><strong>${esc(f.name)}</strong><small>${f.directory?'文件夹':bytes(f.size)}</small></span></button><div class="file-actions">${!f.directory?`<button data-file-action="download" data-path="${esc(f.path)}" aria-label="下载 ${esc(f.name)}">↓</button>`:''}${trash?`<button data-file-action="restore" data-path="${esc(f.path)}">恢复</button><button class="danger" data-file-action="delete" data-path="${esc(f.path)}" aria-label="永久删除 ${esc(f.name)}">永久删除</button>`:`<button data-file-action="rename" data-path="${esc(f.path)}">重命名</button><button data-file-action="trash" data-path="${esc(f.path)}" aria-label="移至回收站 ${esc(f.name)}">⌫</button>`}</div></article>`).join('')||'<div class="empty">—</div>';
 renderFileStatus();
}
async function fileTask(work){if(fileBusy)return;fileBusy=true;fileCancel=false;notice('');renderFileStatus();try{return await work();}catch(e){notice(e.message);}finally{fileBusy=false;$('file-transfer').hidden=true;renderFileStatus();}}
async function fetchDirectory(path){let cursor=0,entries=[];do{const result=await api('files',{operation:'list',path,offset:cursor});entries.push(...result.entries);cursor=result.next;if(entries.length>2000)throw Error('目录项目过多');if(result.total)$('file-storage').innerHTML=meter('内部存储',result.used,result.total);}while(cursor>=0);filePath=path;fileEntries=entries;renderFiles();}
function loadFiles(path){return fileTask(()=>fetchDirectory(path));}
async function trackTransfer(id){
 let missing=0;
 $('file-transfer').hidden=false;
 for(;;){
  let t;try{t=await api('transfers',{operation:fileCancel?'cancel':'status',transfer_id:id});}catch(e){
   if(e.message.includes('传输不存在或已过期')&&missing++<20){await new Promise(r=>setTimeout(r,250));continue;}
   if(e.message.includes('传输不存在或已过期'))localStorage.removeItem('marauder-transfer');throw e;
  }
  $('file-progress').value=t.progress*100;$('file-transfer-label').textContent=bytes(t.offset)+' / '+bytes(t.expected_size||0);
  if(t.status==='COMPLETED'){localStorage.removeItem('marauder-transfer');const a=document.createElement('a');a.href='/api/transfers/download/'+id;a.download=t.path.split('/').at(-1);a.click();return;}
  if(t.status==='FAILED'||t.status==='CANCELLED'){localStorage.removeItem('marauder-transfer');throw Error(t.error||'下载已取消');}
  await new Promise(r=>setTimeout(r,250));
 }
}
async function downloadFile(path){return fileTask(async()=>{const id=crypto.randomUUID().replaceAll('-','');localStorage.setItem('marauder-transfer',id);await api('transfers',{operation:'start',path,transfer_id:id});await trackTransfer(id);});}
// Reload reconnects to the same backend lifecycle, never starts another device read.
window.addEventListener('load',()=>{const id=localStorage.getItem('marauder-transfer');if(id)setTimeout(()=>fileTask(()=>trackTransfer(id)),800);});
function requestFileName(action,path){fileNameAction={action,path};$('file-name-title').textContent=action==='mkdir'?'新建文件夹':'重命名';$('file-name').value=action==='rename'?path.split('/').at(-1):'';$('file-name-dialog').showModal();$('file-name').focus();$('file-name').select();}
$('file-name-form').addEventListener('submit',async e=>{e.preventDefault();const name=$('file-name').value;if(!name||name==='.'||name==='..'||name.includes('/')||name.includes('\\')||/[\x00-\x1f\x7f]/.test(name)){notice('名称无效');return;}const {action,path}=fileNameAction;const destination=(action==='mkdir'?filePath:fileParent(path)).replace(/\/$/,'')+'/'+name;$('file-name-dialog').close();await fileTask(async()=>{await api('files',action==='mkdir'?{operation:'mkdir',path:destination}:{operation:'rename',path,destination});await fetchDirectory(filePath);});});
$('file-name-cancel').addEventListener('click',()=>$('file-name-dialog').close());
$('file-list').addEventListener('click',async e=>{const b=e.target.closest('[data-file-action]');if(!b||fileBusy)return;const action=b.dataset.fileAction,path=b.dataset.path;if(action==='open')return loadFiles(path);if(action==='download')return downloadFile(path);if(action==='rename')return requestFileName(action,path);if(action==='delete')return requestFileDelete(action,path);await fileTask(async()=>{await api('files',{operation:action,path});await fetchDirectory(filePath);});});
$('file-breadcrumbs').addEventListener('click',e=>{const b=e.target.closest('[data-file-dir]');if(b&&!fileBusy)loadFiles(b.dataset.fileDir);});
$('file-refresh').addEventListener('click',()=>loadFiles(filePath));$('file-trash').addEventListener('click',()=>loadFiles('/.trash'));$('file-mkdir').addEventListener('click',()=>requestFileName('mkdir',filePath));$('file-search').addEventListener('input',renderFiles);$('file-cancel').addEventListener('click',()=>{fileCancel=true;});

function requestFileDelete(action,path){
 if(fileBusy||!fileDeleteReady()||!['delete','emptytrash'].includes(action))return;
 if(action==='delete'?!path.startsWith('/.trash/'):path!=='/.trash')return;
 fileDeleteAction={action,path};const empty=action==='emptytrash',directory=fileEntries.find(f=>f.path===path)?.directory;
 $('file-delete-title').textContent=empty?'清空回收站？':'永久删除？';
 $('file-delete-target').textContent=empty?'回收站中的全部文件和文件夹':path;
 $('file-delete-warning').textContent=empty||directory?'包括所有子目录及内容，无法恢复。':'此操作无法恢复。';
 $('file-delete-confirm').textContent=empty?'清空回收站':'永久删除';
 $('file-delete-dialog').showModal();$('file-delete-cancel').focus();
}
async function confirmFileDelete(){
 if(!fileDeleteAction||fileBusy||!fileDeleteReady())return;
 const {action,path}=fileDeleteAction;$('file-delete-dialog').close();fileDeleteAction=null;
 await fileTask(async()=>{
  let failure;try{await api('files',{operation:action,path,confirm:true});}catch(e){failure=e;}
  // Also refresh after partial/indeterminate deletion; never retry the mutation.
  try{await fetchDirectory(action==='emptytrash'?'/.trash':filePath);}catch(e){if(!failure)failure=e;}
  if(failure)throw failure;
 });
}
$('file-empty-trash').addEventListener('click',()=>requestFileDelete('emptytrash','/.trash'));
$('file-delete-form').addEventListener('submit',e=>{e.preventDefault();confirmFileDelete();});
$('file-delete-cancel').addEventListener('click',()=>$('file-delete-dialog').close());
$('file-delete-dialog').addEventListener('close',()=>{fileDeleteAction=null;});

for(const [id,key] of [['hide-unnamed','hideUnnamed'],['ble-hide-unnamed','hideBleUnnamed']])$(id).addEventListener('change',()=>{settings[key]=$(id).checked;saveSettings();render();});
$('selected-only').addEventListener('click',()=>{selectedOnly=!selectedOnly;render();});
$('ble-selected-only').addEventListener('click',()=>{bleSelectedOnly=!bleSelectedOnly;render();});
for(const id of ['network-list','station-list']){
 $(id).addEventListener('contextmenu',e=>{const row=e.target.closest(id==='network-list'?'details[data-key]':'.station-row');if(!row)return;e.preventDefault();toggleStage(id==='network-list'?'ap':'sta',row.dataset.key||row.dataset.mac);});
 $(id).addEventListener('click',e=>{const b=e.target.closest('.stage-device');if(!b)return;e.preventDefault();toggleStage(b.dataset.kind,b.dataset.mac);});
}
async function commitSelection(selected){if(busy||!stagedSelection.size)return;setBusy(true);notice('');try{const items=[...stagedSelection.values()].sort((a,b)=>a.kind.localeCompare(b.kind));for(let i=0;i<items.length;i+=64){const batch=items.slice(i,i+64);await api('selection',{items:batch,selected});for(const item of batch)stagedSelection.delete(item.kind+':'+item.mac);renderSelection();}}catch(e){notice(e.message);}finally{setBusy(false);await refresh();render();}}
$('confirm-selection').addEventListener('click',()=>commitSelection(true));$('remove-selection').addEventListener('click',()=>commitSelection(false));
function stageBluetooth(key){if(stagedBle.has(key))stagedBle.delete(key);else stagedBle.add(key);renderBluetooth();renderSelection();}
$('ble-list').addEventListener('contextmenu',e=>{const row=e.target.closest('details[data-key]');if(row){e.preventDefault();stageBluetooth(row.dataset.key);}});
$('ble-list').addEventListener('click',e=>{const b=e.target.closest('[data-ble-stage],[data-ble-remove]');if(!b)return;e.preventDefault();if(b.dataset.bleStage)stageBluetooth(b.dataset.bleStage);else{bleSelected.delete(b.dataset.bleRemove);saveBleSelection();renderBluetooth();}});
function saveBleSelection(){try{localStorage.setItem('marauder-ble-selected',JSON.stringify([...bleSelected]));}catch{notice('浏览器无法保存选择');}}
$('ble-confirm-selection').addEventListener('click',()=>{for(const key of stagedBle)if(bleSelected.size<1000)bleSelected.add(key);stagedBle.clear();saveBleSelection();render();});
$('clear-log').addEventListener('click',async()=>{try{const result=await api('clear_logs',{});logs=logs.filter(x=>x.id>result.through);renderLogs();await refresh();}catch(e){notice(e.message);}});

let hostTempDirty=false,hostTempSaving=false;
function renderHostStorage(){
 const state=data?.state.host_storage;if(!state)return;
 const input=$('host-temp-dir');input.placeholder=state.base_directory;input.disabled=hostTempSaving||!serverUp;
 if(!hostTempDirty&&!hostTempSaving&&document.activeElement!==input)input.value=state.configured;
 $('host-temp-save').disabled=hostTempSaving||!serverUp;
 $('host-temp-default').disabled=hostTempSaving||!serverUp;
}
$('host-temp-dir').addEventListener('input',()=>{hostTempDirty=true;});
async function saveHostStorage(directory){
 hostTempSaving=true;renderHostStorage();notice('');
 try{const state=await api('host_storage',{operation:'set',directory});hostTempDirty=false;data.state.host_storage=state;$('host-temp-dir').value=state.configured;await refresh();}
 catch(error){notice(error.message);}
 finally{hostTempSaving=false;renderHostStorage();}
}
$('host-temp-form').addEventListener('submit',event=>{event.preventDefault();saveHostStorage($('host-temp-dir').value);});
$('host-temp-default').addEventListener('click',()=>saveHostStorage(''));

for(const kind of ['ap','sta','ble']){
 for(const suffix of ['state','rssi','extra','sort','group'])$(kind+'-'+suffix).addEventListener(suffix==='extra'?'input':'change',()=>{if(suffix==='group')chosenGroups[kind].clear();render();});
 $(kind+'-groups').addEventListener('change',e=>{if(!e.target.matches('[data-list-group]'))return;const groups=chosenGroups[kind];if(e.target.checked)groups.add(e.target.value);else groups.delete(e.target.value);$(kind+'-select-groups').disabled=!groups.size;});
 $(kind+'-select-visible').addEventListener('click',()=>stageVisible(kind,false));$(kind+'-select-groups').addEventListener('click',()=>stageVisible(kind,true));
 $(kind+'-clear-stage').addEventListener('click',()=>{if(kind==='ble')stagedBle.clear();else for(const [key,item] of stagedSelection)if(item.kind===kind)stagedSelection.delete(key);render();});
}

for(const [id,key] of [['manual-refresh','work_refresh_interval'],['console-refresh','work_refresh_interval'],['idle-refresh','idle_refresh_interval'],['console-idle-refresh','idle_refresh_interval']])$(id).addEventListener('change',async()=>{
 const interval=Number($(id).value);
 if(!Number.isInteger(interval)||interval<0||interval>60){notice('状态刷新间隔须为 0–60 秒');$(id).value=data?.state[key]??(key==='idle_refresh_interval'?2:8);return;}
 manualRefreshSaving=true;try{await api('telemetry',{[key]:interval});notice('');await refresh();}catch(e){notice(e.message);}finally{manualRefreshSaving=false;renderStatus();}
});
$('connection-mode-override').addEventListener('change',async()=>{connectionModeSaving=true;renderStatus();try{await api('transport',{operation:'configure',mode:$('connection-mode-override').value});notice('');await refresh();}catch(e){notice(e.message);}finally{connectionModeSaving=false;renderStatus();}});
