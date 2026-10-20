const PAGE = [
'<!doctype html><html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no"><meta name="theme-color" content="#111827"><title>ScreenBridge Relay</title>',
'<style>:root{color-scheme:dark;font-family:system-ui,sans-serif;background:#0b1020;color:#e5e7eb}*{box-sizing:border-box}body{margin:0;padding:18px;max-width:900px;margin-inline:auto}h1{font-size:1.5rem;margin:0 0 6px}p{color:#aeb9ce;line-height:1.45}.panel{background:#111a2c;border:1px solid #2b3852;border-radius:16px;padding:15px;margin:14px 0}.row{display:flex;gap:9px;flex-wrap:wrap;align-items:center}input,button{font:inherit;border-radius:10px;padding:11px 13px;border:1px solid #405171;background:#0b1325;color:#f9fafb}input{width:150px;letter-spacing:.15em;font-weight:700}button{cursor:pointer;font-weight:650}button.primary{background:#2563eb;border-color:#2563eb}button.danger{background:#991b1b;border-color:#dc2626}button:disabled{opacity:.45;cursor:not-allowed}#screenWrap{min-height:220px;display:grid;place-items:center;background:#050811;border-radius:12px;overflow:hidden}#screen{display:block;max-width:100%;height:auto;max-height:65vh;object-fit:contain;touch-action:none;user-select:none;-webkit-user-drag:none}#placeholder{color:#64748b;padding:36px;text-align:center}small,.foot{color:#94a3b8}.pill{border:1px solid #405171;border-radius:999px;padding:3px 9px;font-size:.8rem;color:#cbd5e1}#msg{min-height:1.5em;color:#bfdbfe;word-break:break-word}.controls button{min-width:82px}.foot{font-size:.82rem}</style></head>',
'<body><h1>ScreenBridge Relay</h1><p>Controle remoto autenticado para seu próprio Android. Após o primeiro pareamento, este navegador pode reconectar com a credencial salva. Os frames não são gravados em disco.</p>',
'<section class="panel"><div class="row"><label for="pairCode">Código de pareamento</label><input id="pairCode" inputmode="numeric" autocomplete="one-time-code" maxlength="8" placeholder="8 dígitos"><button class="primary" id="connect">Parear e conectar</button></div><p id="status">Desligado — aguardando pareamento.</p><div id="msg" role="status"></div></section>',
'<section class="panel"><div class="row" style="justify-content:space-between"><strong>Tela do dispositivo</strong><span class="pill" id="frameAge">Sem imagem</span></div><div id="screenWrap"><div id="placeholder">Pareie o Android para receber a tela.</div><img id="screen" alt="Tela do Android" hidden></div><p><small>Toque/clique para tocar. Arraste para deslizar. Os botões permitem voltar, ir ao início ou interromper o controle.</small></p><div class="row controls"><button id="back" disabled>← Voltar</button><button id="home" disabled>⌂ Início</button><button class="danger" id="stop" disabled>■ Parar controle</button><button id="disconnect" disabled>Desconectar</button></div></section>',
'<p class="foot">HTTPS/WSS · código de uso único · credencial deste navegador persistida até revogação · nenhum frame é persistido. Não use em dispositivo compartilhado e não compartilhe tokens.</p>',
'<script>',
'const $=id=>document.getElementById(id);const STORAGE_KEY="screenbridge.controller.v1";let socket=null,deviceId=null,controllerToken=null,frameUrl=null,drag=null,reconnectTimer=null,reconnectDelay=1500,manualDisconnect=false;',
'function status(s){$("status").textContent=s}function message(s){$("msg").textContent=s}function setControls(on){$("back").disabled=!on;$("home").disabled=!on;$("stop").disabled=!on;$("disconnect").disabled=!on;$("connect").disabled=on}',
'function clearSavedIfMatches(token,id){try{const x=JSON.parse(localStorage.getItem(STORAGE_KEY)||"null");if(x&&x.deviceId===id&&x.controllerToken===token)localStorage.removeItem(STORAGE_KEY)}catch(e){}}function saveSaved(){try{if(deviceId&&controllerToken)localStorage.setItem(STORAGE_KEY,JSON.stringify({deviceId,controllerToken,savedAt:Date.now()}))}catch(e){message("O navegador bloqueou o armazenamento local; será necessário parear novamente após fechar a página.")}}function loadSaved(){try{const x=JSON.parse(localStorage.getItem(STORAGE_KEY)||"null");if(x&&typeof x.deviceId==="string"&&typeof x.controllerToken==="string"){deviceId=x.deviceId;controllerToken=x.controllerToken;return true}}catch(e){}return false}function closeController(reason,forget=false){manualDisconnect=true;if(reconnectTimer){clearTimeout(reconnectTimer);reconnectTimer=null}const oldToken=controllerToken,oldId=deviceId;if(socket){const s=socket;socket=null;try{s.close(1000,reason||"disconnect")}catch(e){}}if(forget){clearSavedIfMatches(oldToken,oldId);controllerToken=null;deviceId=null}setControls(false);status("Desconectado.")}',
'async function pair(){const pairCode=$("pairCode").value.trim();if(!/^\\d{8}$/.test(pairCode)){message("Digite o código de 8 dígitos exibido no Android.");return}message("Validando código de uso único…");$("connect").disabled=true;try{const r=await fetch("/api/controller/pair",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({pairCode})});const j=await r.json();if(!r.ok||!j.ok)throw new Error(j.error||"Pareamento recusado");deviceId=j.deviceId;controllerToken=j.controllerToken;manualDisconnect=false;reconnectDelay=1500;saveSaved();message("Pareado. Este navegador será lembrado até revogares o acesso.");connectController()}catch(e){message(e.message||"Falha no pareamento");$("connect").disabled=false}}',
'function scheduleReconnect(){if(reconnectTimer||manualDisconnect||!deviceId||!controllerToken)return;const delay=reconnectDelay;reconnectDelay=Math.min(30000,Math.round(reconnectDelay*1.7));status("Conexão interrompida; tentando novamente em "+Math.ceil(delay/1000)+"s…");reconnectTimer=setTimeout(()=>{reconnectTimer=null;connectController()},delay)}function connectController(){if(manualDisconnect||!deviceId||!controllerToken||socket&&socket.readyState<2)return;$("connect").disabled=true;message("Conectando com a credencial salva…");const wsUrl=new URL("/ws/controller",location.href);wsUrl.protocol=location.protocol==="https:"?"wss:":"ws:";wsUrl.searchParams.set("deviceId",deviceId);const ws=new WebSocket(wsUrl);socket=ws;ws.binaryType="blob";ws.onopen=()=>ws.send(JSON.stringify({type:"auth",role:"controller",deviceId,token:controllerToken}));ws.onmessage=event=>{if(typeof event.data==="string"){let d;try{d=JSON.parse(event.data)}catch(e){return}if(d.type==="auth-result"){if(!d.ok){message("Credencial rejeitada ou revogada; será necessário parear de novo.");closeController("auth failed",true);return}reconnectDelay=1500;setControls(true);status("Conectado e autenticado — controlador salvo.");message("Reconexão automática ativada para este navegador.")}else if(d.type==="action-result"){message("Ação "+(d.op||"")+" — "+(d.accepted?"aceita":"recusada")+(d.detail?": "+d.detail:""));if(d.op==="stop-control"){closeController("emergency stop",true);status("Controle interrompido e credencial revogada.");message("Acesso revogado; para voltar será necessário parear novamente.")}}else if(d.type==="device-status"){message(d.status==="device-online"?"Android conectado.":"Estado do Android: "+d.status)}else if(d.type==="error"){message(d.error||"Erro do relay")}return}if(event.data instanceof Blob){if(frameUrl)URL.revokeObjectURL(frameUrl);frameUrl=URL.createObjectURL(event.data);$("screen").src=frameUrl;$("screen").hidden=false;$("placeholder").hidden=true;$("frameAge").textContent="Ao vivo"}};ws.onerror=()=>message("Erro na conexão WebSocket.");ws.onclose=()=>{if(socket===ws)socket=null;setControls(false);if(!manualDisconnect&&deviceId&&controllerToken){scheduleReconnect()}else{status("Desconectado.")}}}function send(op,extra={}){if(!socket||socket.readyState!==WebSocket.OPEN){message("Não há controlador conectado.");return}socket.send(JSON.stringify({type:"command",id:(crypto.randomUUID?crypto.randomUUID():String(Date.now())+Math.random()),op,...extra}));if(op==="stop-control")message("Parada de emergência: o vínculo salvo será revogado…")}',
'function coords(ev){const r=$("screen").getBoundingClientRect(),img=$("screen");return{x:Math.max(0,Math.min(img.naturalWidth-1,Math.round((ev.clientX-r.left)*img.naturalWidth/r.width))),y:Math.max(0,Math.min(img.naturalHeight-1,Math.round((ev.clientY-r.top)*img.naturalHeight/r.height)))}}',
'$("connect").addEventListener("click",pair);$("pairCode").addEventListener("keydown",e=>{if(e.key==="Enter")pair()});$("back").addEventListener("click",()=>send("back"));$("home").addEventListener("click",()=>send("home"));$("stop").addEventListener("click",()=>send("stop-control"));$("disconnect").addEventListener("click",()=>closeController("user disconnect"));',
'$("screen").addEventListener("pointerdown",e=>{if(!socket||socket.readyState!==WebSocket.OPEN)return;e.preventDefault();$("screen").setPointerCapture(e.pointerId);drag={...coords(e),x0:e.clientX,y0:e.clientY,t:Date.now()}});$("screen").addEventListener("pointerup",e=>{if(!drag)return;const end=coords(e),dx=e.clientX-drag.x0,dy=e.clientY-drag.y0,ms=Math.max(100,Math.min(2000,Date.now()-drag.t));if(Math.hypot(dx,dy)<9)send("tap",{x:drag.x,y:drag.y});else send("swipe",{x1:drag.x,y1:drag.y,x2:end.x,y2:end.y,ms});drag=null});$("screen").addEventListener("pointercancel",()=>drag=null);if(loadSaved()){manualDisconnect=false;status("Controlador salvo encontrado; reconectando…");message("Tentando retomar a sessão salva neste navegador.");connectController()}document.addEventListener("visibilitychange",()=>{if(!document.hidden&&(!socket||socket.readyState>=2)&&!manualDisconnect&&deviceId&&controllerToken){if(reconnectTimer){clearTimeout(reconnectTimer);reconnectTimer=null}connectController()}});',
'</script></body></html>'
].join("\n");
function responseJson(obj,status=200){return new Response(JSON.stringify(obj),{status,headers:{"Content-Type":"application/json; charset=utf-8","Cache-Control":"no-store","X-Content-Type-Options":"nosniff"}})}
function randomToken(){const bytes=crypto.getRandomValues(new Uint8Array(32));let s="";for(const b of bytes)s+=String.fromCharCode(b);return btoa(s).replace(/\+/g,"-").replace(/\//g,"_").replace(/=+$/,"")}
function validDeviceId(s){return typeof s==="string"&&/^[A-Za-z0-9._:-]{8,100}$/.test(s)}
function secureCompare(a,b){return typeof a==="string"&&typeof b==="string"&&a.length===b.length&&a===b}
export default {
 async fetch(request,env){
  const u=new URL(request.url),headers={"Content-Security-Policy":"default-src 'self'; img-src 'self' blob: data:; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self' https: wss:; base-uri 'none'; frame-ancestors 'none'","X-Content-Type-Options":"nosniff","Referrer-Policy":"no-referrer","Cache-Control":"no-store"};
  if(request.method==="GET"&&(u.pathname==="/"||u.pathname==="/app"))return new Response(PAGE,{status:200,headers:{...headers,"Content-Type":"text/html; charset=utf-8"}});
  if(request.method==="GET"&&u.pathname==="/api/health")return responseJson({ok:true,service:"screenbridge-relay",version:"0.2.0",persistentController:true});
  return env.RELAY_HUB.get(env.RELAY_HUB.idFromName("screenbridge-relay-v1")).fetch(request);
 }
};
export class RelayHub {
 constructor(ctx,env){this.ctx=ctx;this.env=env;this.rateBuckets=new Map();this.lastFrameAt=new Map()}
 async fetch(request){
  const url=new URL(request.url);
  if(request.headers.get("Upgrade")?.toLowerCase()==="websocket"){
   if(!["/ws/device","/ws/controller"].includes(url.pathname))return new Response("Not found",{status:404});
   const pair=new WebSocketPair(),client=pair[0],server=pair[1];this.ctx.acceptWebSocket(server);server.serializeAttachment({authenticated:false,connectedAt:Date.now()});return new Response(null,{status:101,webSocket:client});
  }
  if(url.pathname==="/api/device/pair/start"&&request.method==="POST")return this.startPairing(request);
  if(url.pathname==="/api/controller/pair"&&request.method==="POST")return this.finishPairing(request);
  return responseJson({ok:false,error:"Not found"},404);
 }
 async readBody(request){if(Number(request.headers.get("Content-Length")||0)>4096)throw new Error("Request too large");return await request.json()}
 rateLimit(request,limit){const ip=request.headers.get("CF-Connecting-IP")||"unknown",now=Date.now();let b=this.rateBuckets.get(ip);if(!b||now-b.start>60000)b={start:now,count:0};b.count++;this.rateBuckets.set(ip,b);return b.count<=limit}
 async startPairing(request){
  if(!this.rateLimit(request,30))return responseJson({ok:false,error:"Too many requests. Try again in one minute."},429);
  let body;try{body=await this.readBody(request)}catch{return responseJson({ok:false,error:"Invalid JSON body"},400)}
  const deviceId=body?.deviceId,deviceToken=body?.deviceToken;
  if(!validDeviceId(deviceId)||typeof deviceToken!=="string"||deviceToken.length<32||deviceToken.length>512)return responseJson({ok:false,error:"Invalid deviceId or deviceToken"},400);
  const key="device:"+deviceId;let device=await this.ctx.storage.get(key);
  if(device&&!secureCompare(device.deviceToken,deviceToken))return responseJson({ok:false,error:"Device registration conflict"},409);
  if(!device)device={deviceToken,controllerToken:null,controllerTokenExpiresAtMs:null,pairCode:null,pairExpiresAtMs:0,createdAtMs:Date.now()};
  if(device.pairCode&&device.pairExpiresAtMs>Date.now())return responseJson({ok:true,deviceId,pairCode:device.pairCode,expiresAtMs:device.pairExpiresAtMs});
  let code="";
  for(let i=0;i<8;i++){const candidate=String(crypto.getRandomValues(new Uint32Array(1))[0]%100000000).padStart(8,"0");const existing=await this.ctx.storage.get("pair:"+candidate);if(!existing||existing.expiresAtMs<=Date.now()){code=candidate;break}}
  if(!code)return responseJson({ok:false,error:"Could not create a pair code. Retry."},503);
  if(device.pairCode)await this.ctx.storage.delete("pair:"+device.pairCode);
  const expiresAtMs=Date.now()+120000;device.pairCode=code;device.pairExpiresAtMs=expiresAtMs;device.lastPairRequestedAtMs=Date.now();
  await this.ctx.storage.put(key,device);await this.ctx.storage.put("pair:"+code,{deviceId,expiresAtMs});
  return responseJson({ok:true,deviceId,pairCode:code,expiresAtMs});
 }
 async finishPairing(request){
  if(!this.rateLimit(request,8))return responseJson({ok:false,error:"Too many pairing attempts. Wait one minute."},429);
  let body;try{body=await this.readBody(request)}catch{return responseJson({ok:false,error:"Invalid JSON body"},400)}
  const code=body?.pairCode;if(typeof code!=="string"||!/^\d{8}$/.test(code))return responseJson({ok:false,error:"Invalid pair code"},400);
  const pairing=await this.ctx.storage.get("pair:"+code);
  if(!pairing||pairing.expiresAtMs<=Date.now()){await this.ctx.storage.delete("pair:"+code);return responseJson({ok:false,error:"Pair code expired or already used"},404)}
  const key="device:"+pairing.deviceId;const device=await this.ctx.storage.get(key);
  if(!device||device.pairCode!==code||device.pairExpiresAtMs<=Date.now()){await this.ctx.storage.delete("pair:"+code);return responseJson({ok:false,error:"Pair code expired or already used"},404)}
  const controllerToken=randomToken(),controllerTokenExpiresAtMs=null;
  device.controllerToken=controllerToken;device.controllerTokenExpiresAtMs=controllerTokenExpiresAtMs;device.pairCode=null;device.pairExpiresAtMs=0;
  await this.ctx.storage.put(key,device);await this.ctx.storage.delete("pair:"+code);this.closeRole(pairing.deviceId,"controller",4001,"New pairing");
  return responseJson({ok:true,deviceId:pairing.deviceId,controllerToken,expiresAtMs:controllerTokenExpiresAtMs});
 }
 attachment(ws){try{return ws.deserializeAttachment()||{}}catch{return {}}}
 sockets(role,deviceId){return this.ctx.getWebSockets().filter(ws=>{const a=this.attachment(ws);return a.authenticated===true&&a.role===role&&(!deviceId||a.deviceId===deviceId)})}
 closeRole(deviceId,role,code,reason){for(const ws of this.sockets(role,deviceId)){try{ws.close(code||1000,reason||"closed")}catch{}}}
 sendTo(role,deviceId,data){for(const ws of this.sockets(role,deviceId)){try{ws.send(data)}catch{}}}
 async webSocketMessage(ws,message){
  const att=this.attachment(ws);
  if(!att.authenticated){
   if(typeof message!=="string"||message.length>4096){try{ws.close(4002,"Authentication required")}catch{}return}
   let data;try{data=JSON.parse(message)}catch{try{ws.close(4002,"Invalid authentication message")}catch{}return}
   if(data?.type!=="auth"||!["device","controller"].includes(data.role)||!validDeviceId(data.deviceId)||typeof data.token!=="string"){try{ws.close(4002,"Invalid authentication")}catch{}return}
   const record=await this.ctx.storage.get("device:"+data.deviceId);let valid=false;
   if(record&&data.role==="device")valid=secureCompare(record.deviceToken,data.token);
   if(record&&data.role==="controller")valid=secureCompare(record.controllerToken,data.token)&&(record.controllerTokenExpiresAtMs===null||Number(record.controllerTokenExpiresAtMs)>Date.now());
   if(!valid){try{ws.send(JSON.stringify(data.role==="device"?{type:"error",error:"Authentication rejected"}:{type:"auth-result",ok:false}));ws.close(4003,"Authentication rejected")}catch{}return}
   this.closeRole(data.deviceId,data.role,4001,"Session replaced");
   ws.serializeAttachment({authenticated:true,role:data.role,deviceId:data.deviceId,authAtMs:Date.now(),lastFrameAt:0});
   const authReply=data.role==="device"?{type:"authenticated",role:"device",deviceId:data.deviceId}:{type:"auth-result",ok:true,role:"controller",deviceId:data.deviceId};try{ws.send(JSON.stringify(authReply))}catch{}
   this.sendTo(data.role==="device"?"controller":"device",data.deviceId,JSON.stringify({type:"device-status",status:data.role+"-online"}));return;
  }
  if(att.stopped)return;
  if(att.role==="device"){
   if(typeof message!=="string"){const size=message?.byteLength||0;if(size>512*1024)return;const now=Date.now(),last=this.lastFrameAt.get(att.deviceId)||0;if(now-last<75)return;this.lastFrameAt.set(att.deviceId,now);try{ws.serializeAttachment({...att,lastFrameAt:now})}catch{}this.sendTo("controller",att.deviceId,message);return}
   if(message.length>4096)return;let data;try{data=JSON.parse(message)}catch{return}
   if(data.type==="action-result")this.sendTo("controller",att.deviceId,JSON.stringify(data));else if(data.type==="ping"){try{ws.send(JSON.stringify({type:"pong",at:Date.now()}))}catch{}}return;
  }
  if(att.role==="controller"){
   if(typeof message!=="string"||message.length>4096)return;let data;try{data=JSON.parse(message)}catch{return}
   if(data.type!=="command"||!["tap","swipe","back","home","stop-control"].includes(data.op))return;
   const cmd={type:"command",id:typeof data.id==="string"&&data.id.length<=100?data.id:randomToken().slice(0,16),op:data.op};
   if(data.op==="tap"){if(!Number.isFinite(data.x)||!Number.isFinite(data.y)||data.x<0||data.y<0||data.x>20000||data.y>20000)return;cmd.x=Math.floor(data.x);cmd.y=Math.floor(data.y)}
   if(data.op==="swipe"){if(![data.x1,data.y1,data.x2,data.y2,data.ms].every(Number.isFinite)||[data.x1,data.y1,data.x2,data.y2].some(n=>n<0||n>20000)||data.ms<50||data.ms>3000)return;cmd.x1=Math.floor(data.x1);cmd.y1=Math.floor(data.y1);cmd.x2=Math.floor(data.x2);cmd.y2=Math.floor(data.y2);cmd.ms=Math.floor(data.ms)}
   const devices=this.sockets("device",att.deviceId);if(!devices.length){try{ws.send(JSON.stringify({type:"error",error:"Android is offline"}))}catch{}return}
   try{devices[0].send(JSON.stringify(cmd))}catch{try{ws.send(JSON.stringify({type:"error",error:"Could not deliver command"}))}catch{}return}
   if(data.op==="stop-control"){const record=await this.ctx.storage.get("device:"+att.deviceId);if(record){record.controllerToken=null;record.controllerTokenExpiresAtMs=0;await this.ctx.storage.put("device:"+att.deviceId,record)}ws.serializeAttachment({...att,stopped:true})}
  }
 }
 async webSocketClose(ws,code,reason,wasClean){
  const att=this.attachment(ws);
  if(att.authenticated&&att.role==="controller"&&!att.stopped){this.sendTo("device",att.deviceId,JSON.stringify({type:"command",id:"disconnect-"+Date.now(),op:"stop-control"}));this.sendTo("device",att.deviceId,JSON.stringify({type:"device-status",status:"controller-disconnected"}))}
  if(att.authenticated&&att.role==="device")this.sendTo("controller",att.deviceId,JSON.stringify({type:"device-status",status:"device-offline"}));
 }
 async webSocketError(ws,error){const att=this.attachment(ws);if(att.authenticated&&att.role==="controller"&&!att.stopped)this.sendTo("device",att.deviceId,JSON.stringify({type:"command",id:"disconnect-"+Date.now(),op:"stop-control"}))}
}