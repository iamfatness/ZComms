// The panel UI, embedded so the binary is self-contained.
//
// Design notes (the why, since the HTML only shows the what): this is drawn
// as a piece of broadcast intercom hardware -- a keypanel -- because that is
// the object it replaces. Engraved-plate labels, LED semantics (red = talk
// tally, amber = latch, green = link), an LED-ladder meter, and one big
// lever-style TALK key that is the actual PTT gate. Color is never
// decoration: if it glows, it is state.
#pragma once

namespace zc {

inline const char* kPanelHtml = R"ZCUI(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ZComms — Talkback Panel</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=Archivo+Narrow:wght@500;700&family=Martian+Mono:wght@400;700&display=swap" rel="stylesheet">
<style>
:root{
  --iron:#1b1d21; --bezel:#26292f; --bezel-hi:#2e323a; --line:#0e0f11;
  --bone:#e8e4da; --ink-dim:#8a8f96; --silk:#565b62;
  --talk:#e5484d; --latch:#f5a524; --ok:#46c46e;
  --disp:"Archivo Narrow","Arial Narrow",Arial,sans-serif;
  --mono:"Martian Mono",ui-monospace,Consolas,monospace;
}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--iron);color:var(--bone);font-family:var(--disp);
  min-height:100vh;display:grid;place-items:center;padding:24px}
.rack{width:min(1060px,100%);background:
  linear-gradient(180deg,var(--bezel-hi),var(--bezel) 12%,var(--bezel) 88%,#202329);
  border:1px solid var(--line);border-radius:10px;
  box-shadow:0 24px 60px rgba(0,0,0,.5), inset 0 1px 0 rgba(255,255,255,.06)}
.rail{display:flex;justify-content:space-between;align-items:center;
  padding:14px 22px;border-bottom:1px solid var(--line)}
.brand{display:flex;align-items:baseline;gap:12px}
.brand b{font-size:22px;letter-spacing:.28em;font-weight:700}
.brand span{font-size:11px;letter-spacing:.34em;color:var(--ink-dim)}
.leds{display:flex;gap:18px}
.led{display:flex;align-items:center;gap:7px;font:11px var(--mono);color:var(--silk)}
.led i{width:9px;height:9px;border-radius:50%;background:#111;display:inline-block;
  border:1px solid #000;box-shadow:inset 0 1px 1px rgba(0,0,0,.8)}
.led.on i{box-shadow:0 0 8px 1px currentColor}
.led.g.on{color:var(--ok)} .led.g.on i{background:var(--ok)}
.led.a.on{color:var(--latch)} .led.a.on i{background:var(--latch)}
.led.r.on{color:var(--talk)} .led.r.on i{background:var(--talk)}
.deck{display:grid;grid-template-columns:250px 1fr 230px;gap:1px;background:var(--line)}
.deck>section{background:linear-gradient(180deg,var(--bezel) 0%,#24272d 100%);padding:20px}
h2{font-size:10px;letter-spacing:.3em;color:var(--silk);font-weight:500;
  margin-bottom:14px;text-shadow:0 1px 0 rgba(255,255,255,.05)}
/* station */
.meet{font:13px var(--mono);color:var(--ink-dim);margin-bottom:4px}
.state{font:700 15px var(--disp);letter-spacing:.12em;margin-bottom:16px}
.roster{list-style:none;display:flex;flex-direction:column;gap:8px}
.roster li{display:flex;justify-content:space-between;align-items:center;
  font:12px var(--mono);color:var(--bone);border-left:2px solid var(--silk);
  padding:4px 0 4px 10px}
.roster li.tb{border-left-color:var(--ok)}
.roster li.no{color:var(--silk);border-left-color:var(--talk)}
.roster small{font-size:9px;letter-spacing:.1em;color:var(--silk)}
.roster li.tb small{color:var(--ok)}
/* channel module */
.chan{display:flex;flex-direction:column;align-items:center;gap:16px}
.plate{background:#14161a;border:1px solid #000;border-radius:6px;
  padding:10px 34px;text-align:center;
  box-shadow:inset 0 2px 6px rgba(0,0,0,.7), 0 1px 0 rgba(255,255,255,.05)}
.plate b{font-size:20px;letter-spacing:.34em;font-weight:700;
  color:var(--bone);text-shadow:0 -1px 0 #000}
.plate span{display:block;font:10px var(--mono);color:var(--silk);margin-top:4px}
.key{appearance:none;border:0;cursor:pointer;user-select:none;touch-action:none;
  width:min(340px,80%);padding:34px 0 30px;border-radius:12px;
  font:700 30px/1 var(--disp);letter-spacing:.4em;text-indent:.4em;color:#3a3d44;
  background:linear-gradient(180deg,#3a3e46,#2b2e35 55%,#23262c);
  border:1px solid #000;border-bottom-width:6px;
  box-shadow:0 8px 14px rgba(0,0,0,.45), inset 0 1px 0 rgba(255,255,255,.09);
  transition:transform .05s, box-shadow .05s}
.key:focus-visible{outline:2px solid var(--bone);outline-offset:4px}
.key .cap{color:var(--bone);opacity:.55}
.key.hot{transform:translateY(5px);border-bottom-width:1px;
  background:linear-gradient(180deg,#57262b,#3c1a1e 60%,#2c1315);
  box-shadow:0 2px 6px rgba(0,0,0,.5), 0 0 34px rgba(229,72,77,.35),
             inset 0 1px 0 rgba(255,255,255,.08)}
.key.hot .cap{color:var(--talk);opacity:1;text-shadow:0 0 14px rgba(229,72,77,.9)}
.hint{font:10px var(--mono);color:var(--silk);letter-spacing:.12em}
.row{display:flex;gap:12px}
.tog{appearance:none;border:1px solid #000;border-radius:6px;cursor:pointer;
  padding:10px 20px;font:700 12px var(--disp);letter-spacing:.26em;text-indent:.26em;
  color:var(--silk);background:linear-gradient(180deg,#31353c,#262930);
  box-shadow:0 3px 6px rgba(0,0,0,.4), inset 0 1px 0 rgba(255,255,255,.07)}
.tog:focus-visible{outline:2px solid var(--bone);outline-offset:3px}
.tog.on{color:var(--latch);box-shadow:0 0 18px rgba(245,165,36,.25),
  inset 0 2px 6px rgba(0,0,0,.6)}
/* monitor */
.vu{display:flex;flex-direction:column-reverse;gap:4px;height:150px;
  width:34px;margin:0 auto 14px}
.vu i{flex:1;border-radius:2px;background:#15171b;border:1px solid #000;
  box-shadow:inset 0 1px 2px rgba(0,0,0,.7)}
.vu i.on{background:var(--ok);box-shadow:0 0 6px rgba(70,196,110,.5)}
.vu i.mid.on{background:var(--latch);box-shadow:0 0 6px rgba(245,165,36,.5)}
.vu i.top.on{background:var(--talk);box-shadow:0 0 6px rgba(229,72,77,.6)}
.stat{display:flex;justify-content:space-between;font:11px var(--mono);
  color:var(--ink-dim);padding:3px 0}
.stat b{color:var(--bone);font-weight:400}
.gain{display:flex;align-items:center;justify-content:center;gap:10px;margin:10px 0}
.gain button{appearance:none;width:34px;height:30px;border:1px solid #000;border-radius:5px;
  background:linear-gradient(180deg,#31353c,#262930);color:var(--bone);cursor:pointer;
  font:700 14px var(--mono)}
.gain b{font:13px var(--mono);min-width:64px;text-align:center}
/* ops line */
.ops{border-top:1px solid var(--line);padding:10px 22px;
  font:11px var(--mono);color:var(--silk);min-height:36px;
  display:flex;gap:26px;overflow:hidden;white-space:nowrap}
.ops span{color:var(--ink-dim)}
@media (max-width:820px){.deck{grid-template-columns:1fr}.vu{flex-direction:row;height:26px;width:100%}}
@media (prefers-reduced-motion: reduce){.key{transition:none}}
</style></head><body>
<main class="rack" role="application" aria-label="ZComms talkback panel">
  <div class="rail">
    <div class="brand"><b>ZCOMMS</b><span>TALKBACK STATION</span></div>
    <div class="leds">
      <div class="led g" id="led-link"><i></i>LINK</div>
      <div class="led a" id="led-ch"><i></i>CHANNEL</div>
      <div class="led r" id="led-tx"><i></i>TX</div>
    </div>
  </div>
  <div class="deck">
    <section aria-label="station">
      <h2>STATION</h2>
      <div class="meet" id="meet">—</div>
      <div class="state" id="state">CONNECTING</div>
      <h2>PANEL</h2>
      <ul class="roster" id="roster"><li>no one yet</li></ul>
    </section>
    <section class="chan" aria-label="channel">
      <div class="plate"><b id="chname">PANEL 1</b><span id="chsub">channel forming…</span></div>
      <button class="key" id="talk" aria-pressed="false">
        <span class="cap">TALK</span>
      </button>
      <div class="hint">HOLD KEY OR SPACE · LATCH FOR HANDS-FREE</div>
      <div class="row">
        <button class="tog" id="latch">LATCH</button>
        <button class="tog" id="side">SIDETONE</button>
      </div>
    </section>
    <section aria-label="monitor">
      <h2>MONITOR</h2>
      <div class="vu" id="vu"></div>
      <div class="gain">
        <button id="g-dn" aria-label="gain down">–</button>
        <b id="g-val">0 dB</b>
        <button id="g-up" aria-label="gain up">+</button>
      </div>
      <div class="stat"><span>frames sent</span><b id="s-send">0</b></div>
      <div class="stat"><span>underruns</span><b id="s-under">0</b></div>
      <div class="stat"><span>send errors</span><b id="s-fail">0</b></div>
    </section>
  </div>
  <div class="ops" id="ops"><span>panel ready — waiting for the station…</span></div>
</main>
<script>
const $=id=>document.getElementById(id);
const act=(v,a)=>fetch('/act',{method:'POST',body:v+(a!==undefined?' '+a:'')});
let S={gain:0,latched:false,sidetone:true};

/* VU ladder: 12 segments over -48..0 dBFS; top two red, next three amber */
const vu=$('vu');
for(let i=0;i<12;i++){const seg=document.createElement('i');
  if(i>=10)seg.className='top';else if(i>=7)seg.className='mid';vu.appendChild(seg);}
function meter(peak){
  const db=peak>1e-4?20*Math.log10(peak):-60;
  const lit=Math.round(Math.max(0,Math.min(1,(db+48)/48))*12);
  [...vu.children].forEach((seg,i)=>seg.classList.toggle('on',i<lit));
}

/* TALK key: pointer + spacebar, hold semantics. The server ORs this with the
   console's physical spacebar, so either surface can key the channel. */
const key=$('talk');let held=false;
function setHeld(v){if(held===v)return;held=v;act('talk',v?'on':'off');}
key.addEventListener('pointerdown',e=>{key.setPointerCapture(e.pointerId);setHeld(true);});
key.addEventListener('pointerup',()=>setHeld(false));
key.addEventListener('pointercancel',()=>setHeld(false));
addEventListener('keydown',e=>{if(e.code==='Space'&&!e.repeat){e.preventDefault();setHeld(true);}});
addEventListener('keyup',e=>{if(e.code==='Space'){e.preventDefault();setHeld(false);}});
addEventListener('blur',()=>setHeld(false));

$('latch').onclick=()=>act('latch',S.latched?'off':'on');
$('side').onclick=()=>act('sidetone',S.sidetone?'off':'on');
$('g-dn').onclick=()=>act('gain',(S.gain-1));
$('g-up').onclick=()=>act('gain',(S.gain+1));

const es=new EventSource('/events');
es.onmessage=ev=>{S=JSON.parse(ev.data);render();};
es.onerror=()=>{$('state').textContent='LINK LOST';$('led-link').classList.remove('on');};

function render(){
  $('meet').textContent='MTG '+(S.meeting||'—');
  $('state').textContent=S.status||'—';
  $('led-link').classList.toggle('on',S.status==='IN MEETING');
  $('led-ch').classList.toggle('on',!!S.channel_ready);
  $('led-tx').classList.toggle('on',!!S.talking&&!!S.channel_ready);
  key.classList.toggle('hot',!!S.talking);
  key.setAttribute('aria-pressed',!!S.talking);
  $('latch').classList.toggle('on',!!S.latched);
  $('side').classList.toggle('on',!!S.sidetone);
  $('chsub').textContent=S.channel_ready?(S.listeners+' listening'):'channel forming…';
  $('g-val').textContent=(S.gain>0?'+':'')+S.gain+' dB';
  $('s-send').textContent=S.sends;
  $('s-under').textContent=S.underruns;
  $('s-fail').textContent=S.fails;
  meter(S.peak||0);
  const r=$('roster');r.innerHTML='';
  (S.roster||[]).forEach(m=>{
    const li=document.createElement('li');
    li.className=m.tb?'tb':'no';
    const nm=document.createElement('span');nm.textContent=m.name;
    const tag=document.createElement('small');
    tag.textContent=m.tb?(m.invited?'ON CH':'INVITING'):'NO TALKBACK';
    li.append(nm,tag);r.appendChild(li);
  });
  if(!(S.roster||[]).length){r.innerHTML='<li>no one yet</li>';}
  const ops=$('ops');ops.innerHTML='';
  (S.log||[]).forEach(l=>{const sp=document.createElement('span');sp.textContent=l;ops.appendChild(sp);});
}
</script></body></html>
)ZCUI";

}  // namespace zc
