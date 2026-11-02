const fs=require('fs'),vm=require('vm'),assert=require('assert');
const source=fs.readFileSync(__dirname+'/../static/app.js','utf8'),elements=new Map(),calls=[];
const element=id=>{if(!elements.has(id))elements.set(id,{value:'',listeners:{},addEventListener(n,f){this.listeners[n]=f;}});return elements.get(id);};
let fail=false;
const c={$:element,serverUp:true,document:{activeElement:null},data:{state:{host_storage:{configured:'/old',base_directory:'/old',directory:'/old/private'}}},
 notice:m=>c.message=m,refresh:async()=>{},api:async(action,body)=>{calls.push({action,...body});if(fail)throw Error('busy');return {configured:body.directory,base_directory:body.directory||'/system',directory:'/private'};}};
vm.createContext(c);vm.runInContext(source.slice(source.indexOf('let hostTempDirty=')),c);
const run=code=>vm.runInContext(code,c);
(async()=>{
 run('renderHostStorage()');assert.equal(element('host-temp-dir').value,'/old');
 element('host-temp-dir').value='/typing';element('host-temp-dir').listeners.input();c.data.state.host_storage.configured='/other-client';run('renderHostStorage()');assert.equal(element('host-temp-dir').value,'/typing');
 await run("saveHostStorage('/typing')");assert.deepEqual(calls,[{action:'host_storage',operation:'set',directory:'/typing'}]);assert.equal(element('host-temp-dir').value,'/typing');
 await element('host-temp-default').listeners.click();assert.equal(calls.at(-1).directory,'');assert.equal(element('host-temp-dir').value,'');assert.equal(element('host-temp-dir').placeholder,'/system');
 fail=true;element('host-temp-dir').value='/unwritable';element('host-temp-dir').listeners.input();await run("saveHostStorage('/unwritable')");assert.equal(c.message,'busy');assert.equal(element('host-temp-dir').value,'/unwritable');assert(!element('host-temp-save').disabled);
 c.serverUp=false;run('renderHostStorage()');assert(element('host-temp-dir').disabled);assert(element('host-temp-save').disabled);
 console.log('PASS: directory edit survives SSE, save/reset, failure preservation, local-only API, offline controls');
})().catch(e=>{console.error(e);process.exitCode=1;});
