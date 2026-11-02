const fs=require('fs'),vm=require('vm'),assert=require('assert');
const html=fs.readFileSync(__dirname+'/../static/index.html','utf8');
const source=fs.readFileSync(__dirname+'/../static/app.js','utf8');
const ids=[...html.matchAll(/\bid="([^"]+)"/g)].map(m=>m[1]);
assert.equal(new Set(ids).size,ids.length,'Every DOM ID must be unique');
const elements={};
for(const m of html.matchAll(/<select\b[^>]*id="([^"]+)"[^>]*>([\s\S]*?)<\/select>/g)){
 elements[m[1]]={value:m[2].match(/<option value="([^"]*)"/)[1]};
}
assert(elements['ble-state'],'BLE state filter must resolve to a select');
assert(elements['ble-rssi'],'BLE RSSI filter must resolve to a select');
elements['ble-extra']={value:''};
const context={$:id=>elements[id],listSnapshots:{},renderGroupChoices(){},listGroup(){return '全部';},listName:x=>x.name};
vm.createContext(context);
vm.runInContext(source.slice(source.indexOf('function refineList('),source.indexOf('function renderGroupChoices(')),context);
const devices=[{key:'a',name:'Test A',rssi:-45,online:true},{key:'b',name:'Test B',rssi:-85,online:false}];
context.devices=devices;
const visible=()=>Array.from(vm.runInContext("refineList('ble',devices).map(x=>x.key)",context));
assert.deepEqual(visible(),['a','b'],'Default filters must retain scanned devices');
elements['ble-state'].value='online';assert.deepEqual(visible(),['a']);
elements['ble-state'].value='offline';assert.deepEqual(visible(),['b']);
elements['ble-state'].value='all';elements['ble-rssi'].value='-65';assert.deepEqual(visible(),['a']);
elements['ble-rssi'].value='';elements['ble-sort'].value='rssi-asc';assert.deepEqual(visible(),['b','a']);
assert(source.includes("$('ble-strongest-rssi').innerHTML"));
console.log('PASS: unique DOM IDs; BLE default list, online/offline, RSSI filter and sort');

assert(source.includes("$('ble-status-label').textContent"));
assert(!/\$\('ble-(?:state|rssi)'\)\.(?:textContent|innerHTML)\s*=/.test(source),'Rendering metrics must not overwrite filter options');
