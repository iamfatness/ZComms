// The panel UI, embedded so the binary is self-contained.
//
// Styling follows ZCOMMS-BRAND.md (Rev 01) exactly -- it is the single source
// of truth for identity: rack-black/panel surfaces, signal-amber = armed,
// tally-red = transmitting, link-green = linked (exactly one accent per
// state), Archivo for anything a person does, JetBrains Mono for anything a
// machine reports, square corners everywhere except the logo circle and the
// lamps, no gradients. The mark is the three-pin XLR face, drawn in CSS.
#pragma once

namespace zc {

inline const char* kPanelHtml = R"ZCUI(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ZComms — Talkback Station</title>
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E%3Crect width='32' height='32' fill='%230E1013'/%3E%3Ccircle cx='16' cy='16' r='14.4' fill='none' stroke='%23E8E6E1' stroke-width='1.2'/%3E%3Ccircle cx='9.5' cy='11.6' r='2.65' fill='%23E8A33D'/%3E%3Ccircle cx='22.5' cy='11.6' r='2.65' fill='%23E8A33D'/%3E%3Ccircle cx='16' cy='22' r='2.65' fill='%23E8A33D'/%3E%3C/svg%3E">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=Archivo:wght@400;600;700;800&family=JetBrains+Mono:wght@400;500&display=swap" rel="stylesheet">
<style>
:root{
  --rack:#0E1013; --panel:#15181D; --panel-key:#191D23; --scribe:#262B32;
  --edge:#3A4048; --amber:#E8A33D; --red:#E2503F; --green:#4FB286;
  --ivory:#E8E6E1; --legend:#8A9099; --dim:#6E7681; --idle:#2A2F36;
  --disp:"Archivo",Arial,sans-serif;
  --mono:"JetBrains Mono",ui-monospace,Consolas,monospace;
}
*{box-sizing:border-box;margin:0;padding:0}
html,body{height:100%}
body{background:var(--rack);color:var(--ivory);font-family:var(--disp)}
/* The panel IS the window: fills the viewport, deck takes all vertical space. */
.rack{width:100%;height:100%;display:flex;flex-direction:column;background:var(--rack)}
.rail{display:flex;justify-content:space-between;align-items:center;
  padding:14px 22px;border-bottom:1px solid var(--scribe)}
.brand{display:flex;align-items:center;gap:12px}
/* The mark: three-pin XLR face. D=28px; ring stroke .038D; pins .166D at
   (.298,.363) (.702,.363) (.500,.687). Round parts only -- everything else
   on this surface is square. */
.mark{position:relative;width:28px;height:28px;border:1px solid var(--ivory);
  border-radius:50%;flex-shrink:0}
.mark i{position:absolute;width:5px;height:5px;border-radius:50%;
  background:var(--amber);transform:translate(-50%,-50%)}
.mark i:nth-child(1){left:29.8%;top:36.3%}
.mark i:nth-child(2){left:70.2%;top:36.3%}
.mark i:nth-child(3){left:50%;top:68.7%}
.brand b{font-size:20px;letter-spacing:.16em;font-weight:800}
.brand span{font:10px var(--mono);letter-spacing:.42em;color:var(--legend)}
.leds{display:flex;gap:18px}
.led{display:flex;align-items:center;gap:8px;font:11px var(--mono);
  letter-spacing:.22em;color:var(--dim)}
.led i{width:10px;height:10px;border-radius:50%;background:var(--idle);display:inline-block}
.led.g.on{color:var(--green)} .led.g.on i{background:var(--green);box-shadow:0 0 10px #4FB28688}
.led.a.on{color:var(--amber)} .led.a.on i{background:var(--amber);box-shadow:0 0 10px #E8A33D88}
.led.r.on{color:var(--red)} .led.r.on i{background:var(--red);box-shadow:0 0 10px #E2503F88}
.deck{flex:1;min-height:0;display:grid;grid-template-columns:250px 1fr 230px;
  gap:1px;background:var(--scribe)}
.deck>section{background:var(--panel);padding:26px;overflow-y:auto}
h2{font:10px var(--mono);letter-spacing:.30em;color:var(--dim);font-weight:400;
  margin-bottom:14px}
/* station */
.meet{font:13px var(--mono);color:var(--legend);margin-bottom:4px}
.state{font:700 15px var(--disp);letter-spacing:.06em;margin-bottom:16px}
.roster{list-style:none;display:flex;flex-direction:column;gap:8px}
.roster li{display:flex;justify-content:space-between;align-items:center;
  font:12px var(--mono);color:var(--ivory);border-left:2px solid var(--edge);
  padding:4px 0 4px 10px}
.roster li.tb{border-left-color:var(--green)}
.roster li.no{color:var(--dim);border-left-color:var(--red)}
.roster small{font:10px var(--mono);letter-spacing:.22em;color:var(--dim)}
/* channel modules */
.chan{display:flex;flex-direction:column;align-items:center;
  justify-content:center;gap:16px}
.bank{display:flex;flex-wrap:wrap;justify-content:center;gap:14px;width:100%}
.mod{display:flex;flex-direction:column;align-items:center;gap:10px;
  background:var(--panel-key);border:1px solid var(--scribe);
  padding:16px;min-width:170px}
.mod .plate b{font-size:13px;display:block;max-width:170px;overflow:hidden;
  text-overflow:ellipsis;white-space:nowrap}
.mod .key{width:150px;padding:20px 0;font-size:15px}
/* engraved plate */
.plate{background:var(--rack);border:1px solid var(--scribe);
  padding:12px 34px;text-align:center}
.plate b{font-size:18px;letter-spacing:.14em;font-weight:800;color:var(--ivory)}
.plate span{display:block;font:11px var(--mono);color:var(--legend);margin-top:5px}
/* keys */
.key{appearance:none;cursor:pointer;user-select:none;touch-action:none;
  width:min(340px,80%);padding:22px 0;
  background:var(--panel-key);border:1px solid var(--edge);
  font:500 13px var(--mono);letter-spacing:.26em;text-indent:.26em;
  color:#B9BDC4;text-transform:uppercase}
.key:hover{border-color:#4A515B}
.key:active{background:#1F242B}
.key:focus-visible{outline:1px solid var(--amber);outline-offset:3px}
.key.hot{background:var(--red);border-color:var(--red);color:#12141A;font-weight:700}
/* toggles: armed = amber */
.tog{appearance:none;cursor:pointer;border:1px solid var(--edge);
  padding:14px 22px;font:500 13px var(--mono);letter-spacing:.26em;
  text-indent:.26em;text-transform:uppercase;
  color:#B9BDC4;background:var(--panel-key)}
.tog:hover{border-color:#4A515B}
.tog:active{background:#1F242B}
.tog:focus-visible{outline:1px solid var(--amber);outline-offset:3px}
.tog.on{background:#1B1810;border-color:var(--amber);color:var(--amber)}
.hint{font:11px var(--mono);color:var(--dim);letter-spacing:.04em}
.allrow{display:none;gap:12px;align-items:stretch;width:min(560px,92%)}
.allrow.show{display:flex}
.allrow .allkey{flex:1;width:auto;padding:18px 0}
.row{display:flex;gap:12px}
/* join / sign-in card */
.joincard{display:none;flex-direction:column;align-items:center;gap:14px;
  padding:10px 0 6px}
.joincard.show{display:flex}
.joincard input{width:min(420px,86vw);padding:14px 18px;
  border:1px solid var(--edge);background:var(--rack);color:var(--ivory);
  font:17px var(--disp)}
.joincard input::placeholder{color:var(--dim)}
.joincard input:focus{outline:none;border-color:var(--amber)}
/* roster channel chips */
.chips{display:flex;gap:4px}
.chip{appearance:none;cursor:pointer;border:1px solid var(--edge);
  width:22px;height:20px;font:500 10px var(--mono);color:var(--dim);
  background:var(--panel-key)}
.chip.on{color:#12141A;background:var(--green);border-color:var(--green)}
.chip:focus-visible{outline:1px solid var(--amber);outline-offset:2px}
/* meter: stacked segments, bottom-up 4 green / 2 amber, clip cap on top */
.vu{display:flex;flex-direction:column-reverse;gap:3px;margin:0 auto 4px;
  width:34px}
.vu i{width:34px;height:7px;background:var(--idle)}
.vu i.on{background:var(--green)}
.vu i.mid{background:var(--idle)} .vu i.mid.on{background:var(--amber)}
.vu i.cap{background:#3A2020} .vu i.cap.on{background:var(--red)}
.meterrow{display:flex;justify-content:center;gap:8px;margin-bottom:14px}
.scale{display:flex;flex-direction:column-reverse;justify-content:space-between;
  font:10px var(--mono);color:var(--dim);padding:1px 0}
.stat{display:flex;justify-content:space-between;font:12px var(--mono);
  color:var(--legend);padding:3px 0}
.stat b{color:var(--ivory);font-weight:400}
.gain{display:flex;align-items:center;justify-content:center;gap:10px;margin:10px 0}
.gain button{appearance:none;width:34px;height:30px;border:1px solid var(--edge);
  background:var(--panel-key);color:var(--ivory);cursor:pointer;
  font:500 14px var(--mono)}
.gain button:hover{border-color:#4A515B}
.gain b{font:13px var(--mono);min-width:64px;text-align:center;font-weight:400}
/* status bar */
.ops{border-top:1px solid var(--scribe);padding:10px 22px;background:var(--rack);
  font:13px var(--mono);color:var(--legend);min-height:38px;flex-shrink:0;
  display:flex;gap:26px;overflow:hidden;white-space:nowrap;text-transform:lowercase}
.ops span{color:var(--legend)}
@media (max-width:820px){.deck{grid-template-columns:1fr}
  .vu{flex-direction:row;width:100%}.vu i{flex:1}}
</style></head><body>
<main class="rack" role="application" aria-label="ZComms talkback station">
  <div class="rail">
    <div class="brand">
      <div class="mark" aria-hidden="true"><i></i><i></i><i></i></div>
      <b>ZCOMMS</b><span>TALKBACK STATION</span>
    </div>
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
    <section class="chan" aria-label="channels">
      <div class="joincard" id="joincard">
        <div class="plate"><b>JOIN A MEETING</b><span id="joinstate">paste the Zoom link</span></div>
        <input id="joinurl" type="text" placeholder="https://zoom.us/j/…  or meeting ID"
               autocomplete="off" spellcheck="false">
        <button class="tog on" id="joinbtn">CONNECT</button>
      </div>
      <div class="allrow" id="allrow">
        <button class="key allkey" id="allkey" aria-pressed="false">ALL CALL</button>
        <button class="tog" id="alllatch">LATCH ALL</button>
      </div>
      <div class="bank" id="bank"></div>
      <div class="hint">hold a key · digits 1–9 direct · space = all call · latch for hands-free</div>
      <div class="row">
        <button class="tog" id="side">SIDETONE</button>
        <button class="tog" id="aec">ECHO CANCEL</button>
      </div>
    </section>
    <section aria-label="monitor">
      <h2>MONITOR</h2>
      <div class="meterrow">
        <div class="vu" id="vu"></div>
        <div class="scale"><span>−∞</span><span>−18</span><span>−6</span><span>0 dB</span></div>
      </div>
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

/* Meter: 12 segments over -48..0 dBFS. Bottom 4 green, next 2 amber (the
   brand's resting look); the rest light amber under drive and the top
   segment is the clip cap -- red only when the peak is at the rail. */
const vu=$('vu');
for(let i=0;i<12;i++){const seg=document.createElement('i');
  if(i===11)seg.className='cap';else if(i>=4)seg.className='mid';
  vu.appendChild(seg);}
function meter(peak){
  const db=peak>1e-4?20*Math.log10(peak):-60;
  const lit=Math.round(Math.max(0,Math.min(1,(db+48)/48))*12);
  [...vu.children].forEach((seg,i)=>seg.classList.toggle('on',i<lit));
}

/* Channel modules are built once the first state arrives, then updated in
   place. Each TALK key holds per-channel; digits 1..9 hold from the keyboard;
   SPACE (all-call) lives server-side on the physical key. */
const bank=$('bank');
let mods=[];
function buildBank(n){
  bank.innerHTML='';mods=[];
  for(let i=0;i<n;i++){
    const mod=document.createElement('div');mod.className='mod';
    const plate=document.createElement('div');plate.className='plate';
    const nm=document.createElement('b');nm.textContent='CH '+(i+1);
    const sub=document.createElement('span');sub.textContent='—';
    plate.append(nm,sub);
    const key=document.createElement('button');key.className='key';
    key.setAttribute('aria-pressed','false');
    key.textContent='TALK';
    let held=false;
    const setHeld=v=>{if(held===v)return;held=v;act('talk',i+' '+(v?'on':'off'));};
    key.addEventListener('pointerdown',e=>{key.setPointerCapture(e.pointerId);setHeld(true);});
    key.addEventListener('pointerup',()=>setHeld(false));
    key.addEventListener('pointercancel',()=>setHeld(false));
    const latch=document.createElement('button');latch.className='tog';
    latch.textContent='LATCH';
    latch.onclick=()=>act('latch',i+' '+(S.channels[i].latched?'off':'on'));
    mod.append(plate,key,latch);bank.appendChild(mod);
    mods.push({mod,nm,sub,key,latch,setHeld});
  }
}
/* digits 1..9 as per-channel PTT */
addEventListener('keydown',e=>{
  const d=e.code.startsWith('Digit')?+e.code.slice(5):0;
  if(d>=1&&d<=mods.length&&!e.repeat){e.preventDefault();mods[d-1].setHeld(true);}
});
addEventListener('keyup',e=>{
  const d=e.code.startsWith('Digit')?+e.code.slice(5):0;
  if(d>=1&&d<=mods.length){e.preventDefault();mods[d-1].setHeld(false);}
});
addEventListener('blur',()=>{mods.forEach(m=>m.setHeld(false));allSet(false);});

/* ALL CALL: hold to talk to the whole panel, over whatever else is keyed. */
let allHeld=false;
const allSet=v=>{if(allHeld===v)return;allHeld=v;act('talkall',v?'on':'off');};
const allkey=$('allkey');
allkey.addEventListener('pointerdown',e=>{allkey.setPointerCapture(e.pointerId);allSet(true);});
allkey.addEventListener('pointerup',()=>allSet(false));
allkey.addEventListener('pointercancel',()=>allSet(false));
$('alllatch').onclick=()=>{
  const chans=S.channels||[];
  const allOn=chans.length&&chans.every(c=>c.latched);
  act('latchall',allOn?'off':'on');};

const needPass=()=>S.phase==='joining'&&/PASSCODE/.test(S.status||'');
const joinNow=()=>{
  if(S.phase==='signin'){act('signin','');$('joinstate').textContent='browser opened — approve zcomms there';return;}
  const v=$('joinurl').value.trim();
  if(!v)return;
  if(needPass()){act('passcode',v);$('joinurl').value='';$('joinstate').textContent='checking…';}
  else{act('join',v);$('joinstate').textContent='connecting…';}};
$('joinbtn').onclick=joinNow;
$('joinurl').addEventListener('keydown',e=>{if(e.key==='Enter')joinNow();});
$('side').onclick=()=>act('sidetone',(S.sidetone?'off':'on'));
$('aec').onclick=()=>act('aec',(S.aec?'off':'on'));
$('g-dn').onclick=()=>act('gain',(S.gain-1));
$('g-up').onclick=()=>act('gain',(S.gain+1));

const es=new EventSource('/events');
es.onmessage=ev=>{S=JSON.parse(ev.data);render();};
es.onerror=()=>{$('state').textContent='LINK LOST';$('led-link').classList.remove('on');};

function render(){
  const chans=S.channels||[];
  const idle=S.phase!=='up';
  $('joincard').classList.toggle('show',idle);
  $('bank').style.display=idle?'none':'flex';
  if(idle){
    $('allrow').classList.remove('show');
    $('state').textContent=S.status||'—';
    $('joinstate').textContent=(S.phase==='joining')?(S.status||'connecting…')
                             :(S.phase==='signin')?'connect your Zoom account'
                                                  :'paste the Zoom link';
    // The join card doubles as the sign-in card and the passcode prompt.
    const pass=needPass();
    const signin=S.phase==='signin';
    $('joincard').classList.add('show');
    $('joinurl').style.display=signin?'none':'';
    $('joincard').querySelector('.plate b').textContent=
      signin?'SIGN IN WITH ZOOM':'JOIN A MEETING';
    $('joinurl').placeholder=pass?'meeting passcode'
                                 :'https://zoom.us/j/…  or meeting ID';
    $('joinbtn').textContent=signin?'SIGN IN':(pass?'SUBMIT':'CONNECT');
    $('meet').textContent='MTG —';
    ['led-link','led-ch','led-tx'].forEach(i=>$(i).classList.remove('on'));
    const ops=$('ops');ops.innerHTML='';
    (S.log||[]).forEach(l=>{const sp=document.createElement('span');sp.textContent=l;ops.appendChild(sp);});
    return;
  }
  if(mods.length!==chans.length)buildBank(chans.length);
  $('meet').textContent='MTG '+(S.meeting||'—');
  $('state').textContent=S.status||'—';
  const anyReady=chans.some(c=>c.ready);
  $('led-link').classList.toggle('on',S.status==='IN MEETING');
  $('led-ch').classList.toggle('on',anyReady);
  $('led-tx').classList.toggle('on',!!S.talking&&anyReady);
  $('allrow').classList.add('show');
  const allKeyed=chans.length&&chans.every(c=>c.keyed);
  $('allkey').classList.toggle('hot',!!allKeyed);
  $('allkey').setAttribute('aria-pressed',!!allKeyed);
  $('alllatch').classList.toggle('on',chans.length&&chans.every(c=>c.latched));
  chans.forEach((c,i)=>{
    const m=mods[i];if(!m)return;
    // A channel earns a key on the panel when someone is on it (or it is
    // mid-use); empty spares stay off the desk. A one-person channel is a
    // direct line and wears that person's name.
    const active=c.listeners>0||c.label||c.keyed||c.latched;
    m.mod.style.display=active?'':'none';
    m.nm.textContent=c.label||('CH '+(i+1));
    m.nm.title=c.label||'';
    m.sub.textContent=c.ready?(c.label?('direct · ch '+(i+1))
                                      :(c.listeners+' listening'))
                             :'forming…';
    m.key.classList.toggle('hot',!!c.keyed);
    m.key.setAttribute('aria-pressed',!!c.keyed);
    m.latch.classList.toggle('on',!!c.latched);
  });
  $('side').classList.toggle('on',!!S.sidetone);
  $('aec').classList.toggle('on',!!S.aec);
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
    li.appendChild(nm);
    if(m.tb){
      // Chips: the channels in use, plus one spare to start a new group --
      // sixteen buttons per person would drown the roster.
      const inUse=chans.map((c,i)=>i).filter(i=>{const c=chans[i];
        return c.listeners>0||c.label||c.keyed||c.latched||(m.chans||[])[i];});
      const spare=chans.findIndex((c,i)=>!inUse.includes(i));
      const slots=spare>=0?[...inUse,spare]:inUse;
      const chips=document.createElement('span');chips.className='chips';
      slots.forEach(i=>{
        const on=(m.chans||[])[i];
        const b=document.createElement('button');b.className='chip'+(on?' on':'');
        b.textContent=i+1;
        b.title=(on?'Remove from':'Add to')+' '+(chans[i].label||('CH '+(i+1)));
        b.onclick=()=>act('assign',i+':'+m.uid+' '+(on?'off':'on'));
        chips.appendChild(b);
      });
      li.appendChild(chips);
    }else{
      const tag=document.createElement('small');tag.textContent='NO TALKBACK';
      li.appendChild(tag);
    }
    r.appendChild(li);
  });
  if(!(S.roster||[]).length){r.innerHTML='<li>no one yet</li>';}
  const ops=$('ops');ops.innerHTML='';
  (S.log||[]).forEach(l=>{const sp=document.createElement('span');sp.textContent=l;ops.appendChild(sp);});
}
</script></body></html>
)ZCUI";

}  // namespace zc
