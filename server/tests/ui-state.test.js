// Runs the real app's event handlers with a minimal DOM, without microphone or network.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const crypto = require('node:crypto').webcrypto;

function setup() {
  const nodes = new Map();
  function node() {
    return {value:'',checked:false,disabled:false,hidden:false,files:[],listeners:{},children:[],
      classList:{toggle(){},add(){},remove(){}},addEventListener(name,fn){this.listeners[name]=fn;},
      append(...items){this.children.push(...items);},replaceChildren(...items){this.children=items;},
      querySelectorAll(){return [];},querySelector(){return null;},add(item){this.children.push(item);},setAttribute(){}};
  }
  const document={getElementById(id){if(!nodes.has(id))nodes.set(id,node());return nodes.get(id);},
    querySelectorAll(){return [];},createElement(){return node();},createTextNode(text){return {textContent:text};},addEventListener(){}};
  const sandbox={document,window:{addEventListener(){}},sessionStorage:{getItem(){return '';},setItem(){},removeItem(){}},
    URL,Blob,crypto,console,setInterval,clearInterval,Option:class{constructor(text,value){this.text=text;this.value=value;}}};
  vm.createContext(sandbox);
  vm.runInContext(fs.readFileSync(path.join(__dirname,'../static/app.js'),'utf8'),sandbox);
  const get=id=>document.getElementById(id);
  get('recipient').value='1';get('trigger').value='arrive';get('voice-title').value='测试';get('voice-language').value='普通话';get('voice-source').value='test';
  get('schedule-voice').value='voice_1';get('schedule-title').value='日期';get('schedule-kind').value='once';get('schedule-date').value='2026-11-07';get('schedule-recipient').value='0';get('schedule-trigger').value='arrive';
  sandbox.base64=async()=> 'AA==';
  const settle=()=>new Promise(resolve=>setImmediate(()=>setImmediate(resolve)));
  return {sandbox,get,settle,run:code=>vm.runInContext(code,sandbox),submit:async id=>{get(id).listeners.submit({preventDefault(){},submitter:node()});await settle();}};
}

test('an in-flight message keeps a newer audio draft and prevents double send',async()=>{
  const app=setup();let complete;
  app.sandbox.api=async(path,options)=>path==='/api/messages'&&options?.method==='POST'?new Promise(resolve=>{complete=resolve;}):{messages:[]};
  app.run('setDraft(new Blob(["first"]),"第一条.wav")');
  await app.submit('message-form');
  app.run('setDraft(new Blob(["second"]),"第二条.wav")');
  const disabledWhilePending=app.get('send-message').disabled;
  complete({title:'第一条'});await app.settle();
  assert.equal(app.run('draft?.name'),'第二条.wav','old upload completion erased the new recording');
  assert.equal(disabledWhilePending,true,'new recording must not re-enable an in-flight submit button');
  assert.equal(app.get('send-message').disabled,false);
});

for(const kind of ['voice','schedule'])test(kind+' retry retains the same idempotency key after lost response',async()=>{
  const app=setup();const ids=[];
  app.get('voice-file').files=[new Blob(['voice'])];
  app.sandbox.api=async(path,options)=>{
    if(options?.method==='POST') {ids.push(options.body.client_id);if(ids.length===1)throw new Error('response lost');return {};}
    return kind==='voice'?{voices:[]}:{schedules:[]};
  };
  await app.submit(kind+'-form');await app.submit(kind+'-form');
  assert.equal(ids.length,2);
  assert.equal(ids[0],ids[1],'retry generated a new key and could create a duplicate');
});
