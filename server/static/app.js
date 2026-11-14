"use strict";
const $ = id => document.getElementById(id);
let token = sessionStorage.getItem("homehub-phone-token") || "";
let draft = null, recorder = null, holding = false, pendingRecord = false, recordTimer = null;
let voices = [], refreshTimer = null, previewURL = null;
let messagePending = false, voicePending = false, schedulePending = false;
let voiceRequest = null, scheduleRequest = null;
const urls = new Set();
function uid() { return crypto.randomUUID ? crypto.randomUUID().replaceAll("-", "") : "web_" + Date.now() + "_" + Math.random().toString(36).slice(2); }
function notice(message, error = false) { $("notice").textContent = message; $("notice").classList.toggle("error", error); $("notice").hidden = false; }
function formatTime(value) { if (!value) return ""; const d = new Date(value); return isNaN(d) ? value : d.toLocaleString("zh-CN", {hour12:false}); }
async function api(path, options = {}) {
  const headers = {Authorization: "Bearer " + token, ...options.headers};
  if (options.body !== undefined) { headers["Content-Type"] = "application/json"; options.body = JSON.stringify(options.body); }
  const response = await fetch(path, {...options, headers, cache:"no-store"});
  if (!response.ok) { let body; try { body = await response.json(); } catch { body = {}; } throw new Error(body.error || "连接失败，请检查本机服务"); }
  return response.headers.get("content-type").includes("application/json") ? response.json() : response.blob();
}
function empty(container, text) { const box=document.createElement("div");box.className="empty";const symbol=document.createElement("span");symbol.className="empty-symbol";symbol.textContent="⌂";box.append(symbol,document.createTextNode(text));container.append(box); }
function el(tag, cls, text) { const node=document.createElement(tag);if(cls)node.className=cls;if(text!==undefined)node.textContent=text;return node; }
function button(text, callback, danger=false) { const node=el("button",danger?"button-danger":"button-quiet",text);node.type="button";node.addEventListener("click",()=>busy(node,callback));return node; }
async function busy(node, callback) { node.disabled=true;try { await callback(); } catch(error) { notice(error.message,true); } finally { node.disabled=false; } }
async function audioPreview(parent, url) { const blob=await api(url);const player=document.createElement("audio");player.controls=true;const object=URL.createObjectURL(blob);urls.add(object);player.src=object;parent.append(player);try{await player.play();}catch{ /* Playback can still be started with native controls. */ } }
function clearList(id) { const node=$(id);for(const a of node.querySelectorAll("audio")){URL.revokeObjectURL(a.src);urls.delete(a.src);}node.replaceChildren();return node; }
async function refreshMessages() {
  const {messages}=await api("/api/messages");const list=clearList("messages-list");
  if(!messages.length)empty(list,"还没有留言。试着给下一次归来留一句话。");
  for(const message of messages){const item=el("article","item");const top=el("div","item-top");top.append(el("p","item-title",message.title),el("span","status "+message.status,message.status_label));item.append(top,el("p","item-meta",`留给${message.recipient===0?"我自己":"TA"} · 钥匙${message.trigger==="arrive"?"放回":"取走"}时 · ${message.duration.toFixed(1)}秒`),el("p","item-meta",formatTime(message.created_at)));const actions=el("div","actions");actions.append(button("听听这句话",async()=>{if(!item.querySelector("audio"))await audioPreview(item,message.audio_url);}),button("删除",async()=>{if(confirm("删除这条留言？已经下载到设备的副本可能仍可播放。")){await api("/api/messages/"+message.id,{method:"DELETE"});await refreshMessages();}},true));item.append(actions);list.append(item);}
  $("last-refresh").textContent="更新于 "+new Date().toLocaleTimeString("zh-CN",{hour12:false});
}
async function refreshLibrary(){const result=await api("/api/library");voices=result.voices;const list=clearList("library-list");if(!voices.length)empty(list,"声音小屋还是空的。放进一段熟悉的声音吧。");const select=$("schedule-voice");const old=select.value;select.replaceChildren(new Option(voices.length?"请选择一段声音":"请先在声音小屋存一段声音",""));for(const voice of voices){select.add(new Option(voice.title+" · "+voice.language,voice.id));const item=el("article","item");const source={human:"真人录音",test:"测试语音",tts:"合成语音"}[voice.voice_source];item.append(el("p","item-title",voice.title),el("p","item-meta",`${voice.language} · ${source} · ${voice.duration.toFixed(1)}秒`));const actions=el("div","actions");actions.append(button("试听",async()=>{if(!item.querySelector("audio"))await audioPreview(item,voice.audio_url);}),button("删除",async()=>{if(confirm("从声音小屋删除这段声音？已经创建的日期留言会保留副本。")){await api("/api/library/"+voice.id,{method:"DELETE"});await refreshLibrary();}},true));item.append(actions);list.append(item);}select.value=old;}
async function refreshSchedules(){const {schedules}=await api("/api/schedules");const list=clearList("schedules-list");if(!schedules.length)empty(list,"重要的日子，慢慢记在这里。");for(const schedule of schedules){const item=el("article","item");item.append(el("p","item-title",schedule.title),el("p","item-meta",`${schedule.kind==="birthday"?"每年公历 ":"指定日期 "}${schedule.date} · 留给${schedule.recipient===0?"我自己":"TA"} · 钥匙${schedule.trigger==="arrive"?"放回":"取走"}时`));item.append(button("取消日期留言",async()=>{if(confirm("取消这条日期留言？已送进信箱的留言仍会保留。")){await api("/api/schedules/"+schedule.id,{method:"DELETE"});await refreshSchedules();}},true));list.append(item);}}
async function refreshEvents(){const {events}=await api("/api/events");const list=clearList("events-list");if(!events.length)empty(list,"接板并完成配置后，钥匙取放记录会出现在这里。");for(const event of events){const item=el("article","item");item.append(el("p","item-title",`${event.recipient===0?"我":"TA"}的钥匙${event.event==="arrive"?"放回了":"取走了"}`),el("p","item-meta","服务器接收："+formatTime(event.received_at)));if(event.device_time)item.append(el("p","item-meta","设备报告时间（未保证校时）："+formatTime(event.device_time)));list.append(item);}}
async function refreshContext(){const data=await api("/api/context");const cal=data.calendar;$("today").textContent=cal.date+" · "+(cal.solar_term||"归家日常");$("schedule-date").value||=cal.date;$("weather-summary").textContent=data.weather?`${data.weather.city||""} ${data.weather.temperature}℃ · ${data.weather.condition}${data.weather.stale?"（缓存）":""}`:"天气未配置或暂不可用";$("weather-state").textContent=data.weather?`${data.weather.source} · 更新于${formatTime(data.weather.fetched_at)}${data.weather.stale?" · 缓存已过期":""}`:"未配置或暂不可用";$("ai-state").textContent=data.ai.configured?(data.ai.source==="tuya"?"已配置涂鸦服务接入":"已配置服务网关") : "未配置 · 使用本地欢迎语";$("mic-state").textContent=window.isSecureContext?"可请求麦克风权限":"当前地址不支持录音，请使用localhost或HTTPS";$("calendar-state").textContent=cal.solar_status==="date_table"?"按本年度日期表展示；每天按北京时间切换。":"本年度节气表尚未配置，暂不推算节气。";const grid=$("solar-terms");grid.replaceChildren();for(const term of cal.solar_terms){const node=el("div","term"+(term.name===cal.solar_term?" current":""),term.name);node.append(el("span","",term.date.slice(5)));grid.append(node);}}
async function refreshAll(){await Promise.all([refreshMessages(),refreshLibrary(),refreshSchedules(),refreshEvents(),refreshContext()]);}
async function connect(){await api("/api/session",{method:"POST",body:{}});sessionStorage.setItem("homehub-phone-token",token);$("login-panel").hidden=true;$("workspace").hidden=false;await refreshAll();clearInterval(refreshTimer);refreshTimer=setInterval(()=>{if(!document.hidden)Promise.all([refreshMessages(),refreshEvents()]).catch(()=>{});},15000);}
$("login-form").addEventListener("submit",event=>{event.preventDefault();token=$("token").value.trim();busy(event.submitter,connect);});
$("logout").addEventListener("click",()=>{stopRecording();token="";sessionStorage.removeItem("homehub-phone-token");clearInterval(refreshTimer);$("workspace").hidden=true;$("login-panel").hidden=false;$("token").value="";notice("已断开本次会话。");});
for(const tab of document.querySelectorAll(".tab")){tab.addEventListener("click",()=>{for(const other of document.querySelectorAll(".tab")){const active=tab===other;other.classList.toggle("active",active);other.setAttribute("aria-selected",String(active));$("panel-"+other.dataset.tab).hidden=!active;}});}
$("refresh-messages").addEventListener("click",()=>busy($("refresh-messages"),refreshMessages));$("refresh-library").addEventListener("click",()=>busy($("refresh-library"),refreshLibrary));$("refresh-events").addEventListener("click",()=>busy($("refresh-events"),refreshEvents));
function setDraft(blob,name){draft={blob,name,id:uid()};$("draft-info").textContent=name+" · "+Math.ceil(blob.size/1024)+" KB";$("send-message").disabled=messagePending;if(previewURL)URL.revokeObjectURL(previewURL);previewURL=URL.createObjectURL(blob);$("record-preview").src=previewURL;$("record-preview").hidden=false;}
function encodeWav(chunks,rate){const count=chunks.reduce((n,c)=>n+c.length,0);const source=new Float32Array(count);let at=0;for(const chunk of chunks){source.set(chunk,at);at+=chunk.length;}const length=Math.min(480000,Math.floor(count*16000/rate));const buffer=new ArrayBuffer(44+length*2),view=new DataView(buffer);const str=(offset,text)=>{for(let i=0;i<text.length;i++)view.setUint8(offset+i,text.charCodeAt(i));};str(0,"RIFF");view.setUint32(4,36+length*2,true);str(8,"WAVE");str(12,"fmt ");view.setUint32(16,16,true);view.setUint16(20,1,true);view.setUint16(22,1,true);view.setUint32(24,16000,true);view.setUint32(28,32000,true);view.setUint16(32,2,true);view.setUint16(34,16,true);str(36,"data");view.setUint32(40,length*2,true);for(let i=0;i<length;i++){const pos=i*rate/16000,left=Math.min(count-1,Math.floor(pos)),f=pos-left;const sample=Math.max(-1,Math.min(1,source[left]*(1-f)+source[Math.min(count-1,left+1)]*f));view.setInt16(44+i*2,sample<0?sample*32768:sample*32767,true);}return new Blob([buffer],{type:"audio/wav"});}
async function startRecording(){if(recorder||pendingRecord)return;holding=true;pendingRecord=true;$("record-state").textContent="正在请求麦克风权限…";try{if(!window.isSecureContext||!navigator.mediaDevices?.getUserMedia)throw new Error("浏览器录音需要localhost或HTTPS地址。当前可先上传WAV文件。");const stream=await navigator.mediaDevices.getUserMedia({audio:{channelCount:1,echoCancellation:true}});if(!holding){stream.getTracks().forEach(t=>t.stop());$("record-state").textContent="已取消。再次按住按钮即可录音。";return;}const AC=window.AudioContext||window.webkitAudioContext;const context=new AC();await context.resume();if(!holding){stream.getTracks().forEach(t=>t.stop());await context.close();return;}const source=context.createMediaStreamSource(stream),processor=context.createScriptProcessor(4096,1,1),gain=context.createGain();gain.gain.value=0;const chunks=[];processor.onaudioprocess=event=>{if(recorder)chunks.push(new Float32Array(event.inputBuffer.getChannelData(0)));};source.connect(processor);processor.connect(gain);gain.connect(context.destination);recorder={stream,context,source,processor,gain,chunks,start:Date.now()};$("record").classList.add("recording");$("record-label").textContent="正在录音，松手保存";recordTimer=setInterval(()=>{if(!recorder)return;const seconds=Math.floor((Date.now()-recorder.start)/1000);$("record-state").textContent=`已录 ${seconds} 秒 / 30 秒`;if(seconds>=30)stopRecording();},100);}catch(error){holding=false;const message=error.name==="NotAllowedError"?"麦克风权限未允许，请在浏览器地址栏开启麦克风，或上传WAV文件。":error.name==="NotFoundError"?"没有找到麦克风，可以先上传WAV文件。":error.message;notice(message,true);$("record-state").textContent="未开始录音";}finally{pendingRecord=false;}}
function stopRecording(){holding=false;if(!recorder)return;const current=recorder;recorder=null;clearInterval(recordTimer);current.processor.disconnect();current.source.disconnect();current.gain.disconnect();current.stream.getTracks().forEach(t=>t.stop());current.context.close().catch(()=>{});$("record").classList.remove("recording");$("record-label").textContent="按住，说句话";if(!current.chunks.length){$("record-state").textContent="录音太短，请按住稍久一些。";return;}const blob=encodeWav(current.chunks,current.context.sampleRate);setDraft(blob,"我的原声留言.wav");$("record-state").textContent="录音已保存在当前页面，请试听后发送。";$("message-file").value="";}
$("record").addEventListener("pointerdown",event=>{event.preventDefault();$("record").setPointerCapture(event.pointerId);startRecording();});$("record").addEventListener("pointerup",stopRecording);$("record").addEventListener("pointercancel",stopRecording);$("record").addEventListener("keydown",event=>{if((event.code==="Space"||event.code==="Enter")&&!event.repeat){event.preventDefault();startRecording();}});$("record").addEventListener("keyup",event=>{if(event.code==="Space"||event.code==="Enter"){event.preventDefault();stopRecording();}});window.addEventListener("blur",stopRecording);document.addEventListener("visibilitychange",()=>{if(document.hidden)stopRecording();});
$("message-file").addEventListener("change",event=>{const file=event.target.files[0];if(file){if(file.size>6*1024*1024){notice("音频不能超过6MiB，请选择30秒以内的声音。",true);event.target.value="";return;}setDraft(file,file.name);}});
async function base64(blob){const bytes=new Uint8Array(await blob.arrayBuffer());let binary="";for(let i=0;i<bytes.length;i+=32768)binary+=String.fromCharCode(...bytes.subarray(i,i+32768));return btoa(binary);}
$("message-form").addEventListener("submit",event=>{
  event.preventDefault();
  if(messagePending)return;
  const sending=draft;
  messagePending=true;
  busy($("send-message"),async()=>{
    if(!sending)throw new Error("请先按住录音或选择音频。");
    const sent=await api("/api/messages",{method:"POST",body:{client_id:sending.id,recipient:Number($("recipient").value),trigger:$("trigger").value,title:$("message-title").value,filename:sending.name,audio_base64:await base64(sending.blob)}});
    notice("已发送。设备联网轮询并成功下载后，状态会更新为“设备已收到”。");
    if(draft===sending){draft=null;$("draft-info").textContent="已发送："+sent.title;$("message-file").value="";}
    await refreshMessages();
  }).finally(()=>{messagePending=false;$("send-message").disabled=!draft;});
});
function retainedRequest(previous,payload,blob=null){
  const signature=JSON.stringify(payload);
  return previous&&previous.signature===signature&&previous.blob===blob?previous:{id:uid(),signature,blob};
}
$("voice-form").addEventListener("submit",event=>{
  event.preventDefault();if(voicePending)return;voicePending=true;
  busy(event.submitter,async()=>{
    const blob=$("voice-use-draft").checked?draft?.blob:$("voice-file").files[0];
    if(!blob)throw new Error("请选择音频，或先在“留一句话”里录好声音。");
    const payload={title:$("voice-title").value,language:$("voice-language").value,voice_source:$("voice-source").value};
    const submission=voiceRequest=retainedRequest(voiceRequest,payload,blob);
    await api("/api/library",{method:"POST",body:{...payload,client_id:submission.id,audio_base64:await base64(blob)}});
    if(voiceRequest===submission)voiceRequest=null;
    notice("声音已存进小屋。");
    if($("voice-file").files[0]===blob)$("voice-file").value="";
    await refreshLibrary();
  }).finally(()=>{voicePending=false;});
});
$("schedule-kind").addEventListener("change",()=>{$("birthday-note").hidden=$("schedule-kind").value!=="birthday";});
$("schedule-form").addEventListener("submit",event=>{
  event.preventDefault();if(schedulePending)return;schedulePending=true;
  busy(event.submitter,async()=>{
    if(!$("schedule-voice").value)throw new Error("请先在声音小屋存一段声音。");
    const kind=$("schedule-kind").value;
    const payload={recipient:Number($("schedule-recipient").value),trigger:$("schedule-trigger").value,title:$("schedule-title").value,kind,date:kind==="birthday"?$("schedule-date").value.slice(5):$("schedule-date").value,library_id:$("schedule-voice").value};
    const submission=scheduleRequest=retainedRequest(scheduleRequest,payload);
    await api("/api/schedules",{method:"POST",body:{...payload,client_id:submission.id}});
    if(scheduleRequest===submission)scheduleRequest=null;
    notice("已经记下。到期后声音会自动送进信箱。");await refreshSchedules();
  }).finally(()=>{schedulePending=false;});
});
$("try-ai").addEventListener("click",()=>busy($("try-ai"),async()=>{const result=await api("/api/greeting?recipient=1&event=arrive");$("ai-result").textContent=result.source==="unconfigured"?(result.error||"AI服务尚未配置，当前使用本地欢迎语。"):result.text+(result.stale?"（缓存）":"");}));
window.addEventListener("beforeunload",()=>{if(previewURL)URL.revokeObjectURL(previewURL);for(const url of urls)URL.revokeObjectURL(url);});
if(token)connect().catch(error=>{sessionStorage.removeItem("homehub-phone-token");token="";$("login-panel").hidden=false;$("workspace").hidden=true;notice(error.message,true);});
