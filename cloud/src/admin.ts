const PAGE = String.raw`<!doctype html>
<html lang="ja"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>HomePanel Cloud Settings</title>
<style>
:root{color-scheme:dark;font-family:system-ui,sans-serif;background:#0d1117;color:#e6edf3}*{box-sizing:border-box}body{margin:0;padding:24px}main{max-width:980px;margin:auto}section{background:#161b22;border:1px solid #30363d;border-radius:12px;padding:18px;margin:16px 0}.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}label{display:grid;gap:6px}input,textarea,button{font:inherit;border:1px solid #3d444d;border-radius:8px;background:#0d1117;color:#e6edf3;padding:10px}textarea{width:100%;min-height:430px;font-family:ui-monospace,monospace}button{cursor:pointer;background:#21262d}.primary{background:#238636}.warn{background:#9e6a03}.row{display:flex;gap:10px;flex-wrap:wrap}.status{min-height:24px;margin-top:10px;color:#79c0ff}.muted{font-size:13px;color:#8b949e}@media(max-width:700px){.grid{grid-template-columns:1fr}}
</style></head><body><main>
<h1>HomePanel Cloud Settings</h1><p>Cloudflare上の端末設定と遠隔操作を管理します。</p>
<section><div class="grid"><label>Device ID<input id="deviceId" value="primary"></label><label>管理トークン<input id="token" type="password"></label></div>
<div class="row" style="margin-top:12px"><button id="load" class="primary">クラウド設定を読み込む</button><button id="save" class="primary">保存して端末を再起動</button></div><div id="status" class="status"></div><div class="muted">Cloudflare同期は300秒、CO2・温湿度アップロードは30分で固定されます。Spotifyは端末から直接取得します。</div></section>
<section><label>端末設定 JSON<textarea id="config" spellcheck="false"></textarea></label></section>
<section><h2>遠隔操作</h2><div class="row" id="commands"><button data-command="restart_app" class="warn">HomePanel再起動</button><button data-command="reconnect_stationhead">SH再接続</button><button data-command="reload_dashboard">クラウド同期</button><button data-command="clear_display_cache">表示キャッシュ削除</button><button data-command="check_update">更新確認</button></div></section>
</main><script>
const defaults={cloudPollSeconds:300,telemetryMinutes:30,screen:{width:1920,height:1280},co2:{serialPort:"",temperatureOffset:-4.5},stationhead:{url:"https://www.stationhead.com/sakuramankai",fallbackUrl:"https://www.stationhead.com/buddy46",healthCheckIntervalSeconds:60,restartAfterHealthMisses:3,blockImages:true,blockFonts:true,lowMemoryMode:true,memoryLimitMb:450,secondary:{enabled:true,url:"https://www.stationhead.com/sakuramankai"}},updates:{manifestUrl:location.origin+"/v1/update/manifest"}};
const byId=id=>document.getElementById(id),status=message=>{byId("status").textContent=message};let loadedDeviceId="",loadedVersion=null;
const merge=(base,extra)=>{const output=structuredClone(base);if(!extra||typeof extra!=="object"||Array.isArray(extra))return output;for(const [key,value] of Object.entries(extra)){if(value&&typeof value==="object"&&!Array.isArray(value)&&output[key]&&typeof output[key]==="object")output[key]=merge(output[key],value);else output[key]=value}output.cloudPollSeconds=300;output.telemetryMinutes=30;return output};
const migrate=config=>{const output=structuredClone(config||{}),station=output.stationhead;if(station&&typeof station==="object"&&!Array.isArray(station)){if(!Object.hasOwn(station,"blockImages")&&Object.hasOwn(station,"blockImagesAfterPlayback"))station.blockImages=station.blockImagesAfterPlayback;if(!Object.hasOwn(station,"blockFonts")&&Object.hasOwn(station,"blockFontsAfterPlayback"))station.blockFonts=station.blockFontsAfterPlayback;delete station.blockImagesAfterPlayback;delete station.blockFontsAfterPlayback;delete station.hideChatAfterPlayback}return output};
const credentials=()=>{const token=byId("token").value.trim(),deviceId=byId("deviceId").value.trim();if(!token)throw new Error("管理トークンを入力してください");if(!/^[A-Za-z0-9._-]{1,100}$/.test(deviceId))throw new Error("Device IDが不正です");sessionStorage.setItem("homepanelToken",token);localStorage.setItem("homepanelDeviceId",deviceId);return{token,deviceId}};
const call=async(path,options={})=>{const{token}=credentials();const response=await fetch(path,{...options,headers:{Authorization:"Bearer "+token,"Content-Type":"application/json",...(options.headers||{})}});const body=await response.json().catch(()=>({}));if(!response.ok)throw new Error(body.error||("HTTP "+response.status));return body};
async function load(){try{status("読み込み中...");const{deviceId}=credentials(),body=await call("/v1/device/config?deviceId="+encodeURIComponent(deviceId));loadedDeviceId=deviceId;loadedVersion=Number(body.version||0);byId("config").value=JSON.stringify(merge(defaults,migrate(body.config)),null,2);status("クラウド設定を読み込みました。version "+loadedVersion)}catch(error){status(error.message||String(error))}}
async function save(){try{status("保存中...");const{deviceId}=credentials();if(loadedVersion===null||loadedDeviceId!==deviceId)throw new Error("保存前にこのDevice IDの設定を読み込んでください");const config=JSON.parse(byId("config").value);config.cloudPollSeconds=300;config.telemetryMinutes=30;const body=await call("/v1/device/config?deviceId="+encodeURIComponent(deviceId),{method:"PUT",headers:{"If-Match":"\"device-config-"+deviceId+"-"+loadedVersion+"\""},body:JSON.stringify(config)});loadedVersion=Number(body.version||loadedVersion);status(body.changed===false?"変更はありません":"保存しました。再起動コマンドを登録しました。version "+loadedVersion)}catch(error){status(error.message||String(error))}}
byId("load").addEventListener("click",load);byId("save").addEventListener("click",save);byId("commands").addEventListener("click",async event=>{const command=event.target?.dataset?.command;if(!command)return;try{status(command+" を登録中...");const{deviceId}=credentials(),body=await call("/v1/device/commands",{method:"POST",body:JSON.stringify({deviceId,command})});status(command+" を登録しました。command id "+body.id)}catch(error){status(error.message||String(error))}});
byId("token").value=sessionStorage.getItem("homepanelToken")||"";byId("deviceId").value=localStorage.getItem("homepanelDeviceId")||"primary";byId("config").value=JSON.stringify(defaults,null,2);
</script></body></html>`;

export function adminPage(): Response {
  return new Response(PAGE, {
    headers: {
      "Content-Type": "text/html; charset=utf-8",
      "Cache-Control": "no-store",
      "Content-Security-Policy": "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'; form-action 'none'; base-uri 'none'; frame-ancestors 'none'",
      "X-Content-Type-Options": "nosniff",
      "Referrer-Policy": "no-referrer",
    },
  });
}
