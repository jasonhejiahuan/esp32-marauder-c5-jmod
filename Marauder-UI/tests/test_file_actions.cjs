const fs=require('fs'),vm=require('vm'),assert=require('assert');
const source=fs.readFileSync(__dirname+'/../static/app.js','utf8');
const elements=new Map(),calls=[];let failDelete=false;
function element(id){if(!elements.has(id))elements.set(id,{id,value:'',dataset:{},listeners:{},addEventListener(n,f){this.listeners[n]=f;},showModal(){this.open=true;},close(){this.open=false;this.listeners.close?.();},focus(){this.focused=true;}});return elements.get(id);}
const c={serverUp:true,data:{state:{verified:true,mode:'idle',manual:false,capabilities:['files-v1','files-delete-v1']}},$ :element,
 document:{querySelectorAll:()=>[]},window:{addEventListener(){}},localStorage:{getItem(){return null;}},notice:m=>c.message=m,esc:s=>String(s),bytes:String,meter:()=>'',
 api:async(name,body)=>{calls.push(body);if(body.operation==='delete'&&failDelete)throw Error('部分内容已删除');return {entries:[],next:-1};}};
vm.createContext(c);vm.runInContext(source.slice(source.indexOf("let filePath="),source.indexOf("for(const [id,key] of")),c);
const run=code=>vm.runInContext(code,c);
(async()=>{
 run("filePath='/.trash';fileEntries=[{path:'/.trash/folder',directory:true}];requestFileDelete('delete','/.trash/folder')");
 assert(element('file-delete-dialog').open);assert(element('file-delete-cancel').focused);assert(element('file-delete-warning').textContent.includes('子目录'));
 element('file-delete-cancel').listeners.click();await run('confirmFileDelete()');assert.equal(calls.length,0);
 run("requestFileDelete('emptytrash','/.trash')");assert(element('file-delete-target').textContent.includes('全部'));
 element('file-delete-dialog').close();assert.equal(calls.length,0); // Escape/native dialog cancel.
 run("requestFileDelete('delete','/outside')");assert(!element('file-delete-dialog').open);
 c.data.state.capabilities=['files-v1'];run("renderFileStatus();requestFileDelete('delete','/.trash/x')");assert(element('file-empty-trash').hidden);assert(!element('file-delete-dialog').open);
 c.data.state.capabilities.push('files-delete-v1');run("requestFileDelete('delete','/.trash/<unsafe>')");assert.equal(element('file-delete-target').textContent,'/.trash/<unsafe>');
 await run('confirmFileDelete()');assert.deepEqual(JSON.parse(JSON.stringify(calls)),[{operation:'delete',path:'/.trash/<unsafe>',confirm:true},{operation:'list',path:'/.trash',offset:0}]);
 calls.length=0;failDelete=true;run("requestFileDelete('delete','/.trash/x')");await run('confirmFileDelete()');assert.equal(calls.filter(x=>x.operation==='delete').length,1);assert.equal(calls.at(-1).operation,'list');assert.equal(c.message,'部分内容已删除');
 calls.length=0;run("filePath='/.trash/nested';requestFileDelete('emptytrash','/.trash')");await run('confirmFileDelete()');assert.equal(calls[0].operation,'emptytrash');assert.equal(calls.at(-1).path,'/.trash');
 run("requestFileDelete('delete','/.trash/x')");c.data.state.mode='scancombined';calls.length=0;await run('confirmFileDelete()');assert.equal(calls.length,0);
 console.log('PASS: confirmation/Cancel/Escape, old firmware hidden, trash-only scope, typed confirmation, partial-failure refresh without retry, busy-state recheck');
})().catch(e=>{console.error(e);process.exitCode=1;});
