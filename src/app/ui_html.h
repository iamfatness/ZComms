// The panel UI, embedded so the binary is self-contained.
//
// Styling follows ZCOMMS-BRAND.md (Rev 01) exactly. Structure follows the
// intercom-grid model the owner converged on across both products
// (2026-08-29): ONE grid of person-cells -- a cell IS a talk key, wearing
// the person's name and a state line -- with ALL CALL above it. No roster
// column duplicating the same people, no per-channel module bank. Talent
// grouping lives behind [EDIT], audio device/processing controls behind
// [SETTINGS], machine truth in the bottom status bar.
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
.rack{width:100%;height:100%;display:flex;flex-direction:column;background:var(--rack)}
.rail{display:flex;justify-content:space-between;align-items:center;
  padding:14px 22px;border-bottom:1px solid var(--scribe)}
.brand{display:flex;align-items:center;gap:12px}
.mark{position:relative;width:28px;height:28px;border:1px solid var(--ivory);
  border-radius:50%;flex-shrink:0}
.mark i{position:absolute;width:5px;height:5px;border-radius:50%;
  background:var(--amber);transform:translate(-50%,-50%)}
.mark i:nth-child(1){left:29.8%;top:36.3%}
.mark i:nth-child(2){left:70.2%;top:36.3%}
.mark i:nth-child(3){left:50%;top:68.7%}
.brand b{font-size:20px;letter-spacing:.16em;font-weight:800}
.brand span{font:10px var(--mono);letter-spacing:.42em;color:var(--legend)}
.railright{display:flex;align-items:center;gap:18px}
.meet{font:12px var(--mono);color:var(--legend)}
.leds{display:flex;gap:18px}
.led{display:flex;align-items:center;gap:8px;font:11px var(--mono);
  letter-spacing:.22em;color:var(--dim)}
.led i{width:10px;height:10px;border-radius:50%;background:var(--idle);display:inline-block}
.led.g.on{color:var(--green)} .led.g.on i{background:var(--green);box-shadow:0 0 10px #4FB28688}
.led.a.on{color:var(--amber)} .led.a.on i{background:var(--amber);box-shadow:0 0 10px #E8A33D88}
.led.r.on{color:var(--red)} .led.r.on i{background:var(--red);box-shadow:0 0 10px #E2503F88}
/* main desk */
.desk{flex:1;min-height:0;overflow-y:auto;padding:22px;display:flex;
  flex-direction:column;gap:14px;align-items:center}
.allkey{width:100%;max-width:920px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(170px,1fr));
  gap:10px;width:100%;max-width:920px}
/* a cell IS a key */
.cell{appearance:none;cursor:pointer;user-select:none;touch-action:none;
  background:var(--panel-key);border:1px solid var(--edge);padding:12px 14px;
  text-align:left;color:#B9BDC4;min-height:58px}
.cell:hover{border-color:#4A515B}
.cell:focus-visible{outline:1px solid var(--amber);outline-offset:2px}
.cell .nm{display:block;font:600 14px var(--disp);color:var(--ivory);
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.cell .st{display:block;font:10px var(--mono);letter-spacing:.14em;
  color:var(--dim);margin-top:5px;text-transform:uppercase}
.cell.ready .st{color:var(--legend)}
.cell.hot{background:var(--red);border-color:var(--red)}
.cell.hot .nm{color:#12141A}.cell.hot .st{color:#12141A;font-weight:500}
.cell.armed{background:#1B1810;border-color:var(--amber)}
.cell.armed .nm{color:var(--amber)}.cell.armed .st{color:var(--amber)}
.cell.off{cursor:default;background:var(--panel);border-color:var(--scribe)}
.cell.off .nm{color:var(--dim)}
/* key: the ALL CALL bar */
.key{appearance:none;cursor:pointer;user-select:none;touch-action:none;
  padding:18px 0;background:var(--panel-key);border:1px solid var(--edge);
  font:500 13px var(--mono);letter-spacing:.26em;text-indent:.26em;
  color:#B9BDC4;text-transform:uppercase}
.key:hover{border-color:#4A515B}
.key:focus-visible{outline:1px solid var(--amber);outline-offset:3px}
.key.hot{background:var(--red);border-color:var(--red);color:#12141A;font-weight:700}
.key.armed{background:#1B1810;border-color:var(--amber);color:var(--amber)}
/* join / sign-in card */
.joincard{display:none;flex-direction:column;align-items:center;gap:14px;
  padding:40px 0 6px}
.joincard.show{display:flex}
.plate{background:var(--rack);border:1px solid var(--scribe);
  padding:12px 34px;text-align:center}
.plate b{font-size:18px;letter-spacing:.14em;font-weight:800;color:var(--ivory)}
.plate span{display:block;font:11px var(--mono);color:var(--legend);margin-top:5px}
.joincard input{width:min(420px,86vw);padding:14px 18px;
  border:1px solid var(--edge);background:var(--rack);color:var(--ivory);
  font:17px var(--disp)}
.joincard input::placeholder{color:var(--dim)}
.joincard input:focus{outline:none;border-color:var(--amber)}
/* controls strip */
.strip{display:flex;align-items:center;gap:12px;padding:12px 22px;
  border-top:1px solid var(--scribe);background:var(--panel);flex-wrap:wrap}
.tog{appearance:none;cursor:pointer;border:1px solid var(--edge);
  padding:10px 16px;font:500 11px var(--mono);letter-spacing:.26em;
  text-indent:.26em;text-transform:uppercase;color:#B9BDC4;
  background:var(--panel-key)}
.tog:hover{border-color:#4A515B}
.tog:focus-visible{outline:1px solid var(--amber);outline-offset:2px}
.tog.on{background:#1B1810;border-color:var(--amber);color:var(--amber)}
.hmeter{display:flex;gap:3px;align-items:center;margin-left:auto}
.hmeter i{width:7px;height:18px;background:var(--idle)}
.hmeter i.on{background:var(--green)}
.hmeter i.mid{background:var(--idle)} .hmeter i.mid.on{background:var(--amber)}
.hmeter i.cap{background:#3A2020} .hmeter i.cap.on{background:var(--red)}
.hint{font:10px var(--mono);color:var(--dim);letter-spacing:.04em}
/* settings drawer */
.drawer{display:none;border-top:1px solid var(--scribe);background:var(--panel);
  padding:18px 22px}
.drawer.show{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));
  gap:18px}
.field{display:flex;flex-direction:column;gap:6px}
.field label{font:10px var(--mono);letter-spacing:.30em;color:var(--dim);
  text-transform:uppercase}
.field select{background:var(--rack);border:1px solid var(--edge);
  color:var(--ivory);font:14px var(--disp);padding:10px 12px}
.field select:focus{outline:none;border-color:var(--amber)}
.field .row{display:flex;gap:10px;align-items:center}
.gain button{appearance:none;width:34px;height:30px;border:1px solid var(--edge);
  background:var(--panel-key);color:var(--ivory);cursor:pointer;
  font:500 14px var(--mono)}
.gain b{font:13px var(--mono);min-width:64px;text-align:center;font-weight:400}
.stat{display:flex;justify-content:space-between;font:12px var(--mono);
  color:var(--legend);padding:2px 0}
.stat b{color:var(--ivory);font-weight:400}
/* edit-talent list */
.editlist{display:none;width:100%;max-width:920px;flex-direction:column;gap:8px}
.editlist.show{display:flex}
.editlist li{list-style:none;display:flex;justify-content:space-between;
  align-items:center;font:12px var(--mono);color:var(--ivory);
  border-left:2px solid var(--edge);padding:6px 0 6px 10px}
.editlist li.tb{border-left-color:var(--green)}
.editlist li.no{color:var(--dim);border-left-color:var(--red)}
.editlist small{font:10px var(--mono);letter-spacing:.22em;color:var(--dim)}
.chips{display:flex;gap:4px}
.chip{appearance:none;cursor:pointer;border:1px solid var(--edge);
  width:22px;height:20px;font:500 10px var(--mono);color:var(--dim);
  background:var(--panel-key)}
.chip.on{color:#12141A;background:var(--green);border-color:var(--green)}
/* status bar */
.ops{border-top:1px solid var(--scribe);padding:10px 22px;background:var(--rack);
  font:13px var(--mono);color:var(--legend);min-height:38px;flex-shrink:0;
  display:flex;gap:26px;overflow:hidden;white-space:nowrap;text-transform:lowercase}
</style></head><body>
<main class="rack" role="application" aria-label="ZComms talkback station">
  <div class="rail">
    <div class="brand">
      <div class="mark" aria-hidden="true"><i></i><i></i><i></i></div>
      <b>ZCOMMS</b><span>TALKBACK STATION</span>
    </div>
    <div class="railright">
      <div class="meet" id="meet">—</div>
      <div class="leds">
        <div class="led g" id="led-link"><i></i>LINK</div>
        <div class="led g" id="led-mic"><i></i>MTG MIC</div>
        <div class="led a" id="led-ch"><i></i>CHANNEL</div>
        <div class="led r" id="led-tx"><i></i>TX</div>
      </div>
    </div>
  </div>
  <div class="desk" id="desk">
    <div class="joincard" id="joincard">
      <div class="plate"><b>JOIN A MEETING</b><span id="joinstate">paste the Zoom link</span></div>
      <input id="joinurl" type="text" placeholder="https://zoom.us/j/…  or meeting ID"
             autocomplete="off" spellcheck="false">
      <button class="tog on" id="joinbtn">CONNECT</button>
    </div>
    <button class="key allkey" id="allkey" style="display:none" aria-pressed="false">ALL CALL</button>
    <div class="grid" id="grid"></div>
    <ul class="editlist" id="editlist"></ul>
    <div class="hint" id="deskhint" style="display:none">hold a cell to talk · digits 1–9 direct · space = all call · latch mode makes presses stick</div>
  </div>
  <div class="drawer" id="drawer">
    <div class="field"><label>microphone</label>
      <select id="micsel"></select></div>
    <div class="field"><label>sidetone output</label>
      <select id="outsel"></select></div>
    <div class="field"><label>processing</label>
      <div class="row">
        <button class="tog" id="side">SIDETONE</button>
        <button class="tog" id="aec">ECHO CANCEL</button>
        <button class="tog" id="tone" title="replace the mic with a 700 Hz tone through the whole live chain">TONE</button>
      </div>
      <div class="row gain">
        <button id="g-dn" aria-label="gain down">–</button>
        <b id="g-val">0 dB</b>
        <button id="g-up" aria-label="gain up">+</button>
      </div>
    </div>
    <div class="field"><label>station</label>
      <div class="stat"><span>frames sent</span><b id="s-send">0</b></div>
      <div class="stat"><span>channel sends</span><b id="s-chsend">0</b></div>
      <div class="stat"><span>underruns</span><b id="s-under">0</b></div>
      <div class="stat"><span>send errors</span><b id="s-fail">0</b></div>
    </div>
  </div>
  <div class="strip" id="strip" style="display:none">
    <button class="tog" id="latchmode">LATCH</button>
    <button class="tog" id="editbtn">EDIT TALENT</button>
    <button class="tog" id="setbtn">SETTINGS</button>
    <div class="hmeter" id="hmeter" title="mic level"></div>
  </div>
  <div class="ops" id="ops"><span>panel ready — waiting for the station…</span></div>
</main>
<script>
const $=id=>document.getElementById(id);
const act=(v,a)=>fetch('/act',{method:'POST',body:v+(a!==undefined?' '+a:'')});
let S={gain:0,sidetone:true};
let latchMode=false,editMode=false;

/* horizontal mic meter: 12 segments over -48..0 dBFS, PPM-fast fall is
   engine-side now; the display is instantaneous */
const hm=$('hmeter');
for(let i=0;i<12;i++){const seg=document.createElement('i');
  if(i===11)seg.className='cap';else if(i>=4)seg.className='mid';
  hm.appendChild(seg);}
function meter(peak){
  const db=peak>1e-4?20*Math.log10(peak):-60;
  const lit=Math.round(Math.max(0,Math.min(1,(db+48)/48))*12);
  [...hm.children].forEach((seg,i)=>seg.classList.toggle('on',i<lit));
}

/* The grid: one cell per occupied channel (a person, or a named group),
   plus disabled cells for people who cannot receive talkback. Cells are
   rebuilt only when the cell signature changes -- rebuilding a grid under
   a press eats the release. */
const grid=$('grid');
let cells=[],cellSig='';
function cellRows(){
  const rows=[];
  (S.channels||[]).forEach((c,i)=>{
    if(c.listeners>0||c.label||c.keyed||c.latched)
      rows.push({slot:i,name:c.label||('CH '+(i+1)),group:!c.label});
  });
  (S.roster||[]).forEach(p=>{if(!p.tb)rows.push({slot:-1,name:p.name,off:true});});
  return rows;
}
function buildGrid(rows){
  grid.innerHTML='';cells=[];
  rows.forEach(r=>{
    const b=document.createElement('button');
    b.className='cell'+(r.off?' off':'');
    const nm=document.createElement('span');nm.className='nm';
    nm.textContent=r.name;nm.title=r.name;
    const st=document.createElement('span');st.className='st';
    b.append(nm,st);
    if(!r.off){
      let held=false;
      const setHeld=v=>{if(held===v)return;held=v;act('talk',r.slot+' '+(v?'on':'off'));};
      b.addEventListener('pointerdown',e=>{
        if(latchMode){const c=(S.channels||[])[r.slot]||{};
          act('latch',r.slot+' '+(c.latched?'off':'on'));return;}
        b.setPointerCapture(e.pointerId);setHeld(true);});
      b.addEventListener('pointerup',()=>setHeld(false));
      b.addEventListener('pointercancel',()=>setHeld(false));
      r.setHeld=setHeld;
    }
    grid.appendChild(b);
    cells.push({row:r,el:b,st});
  });
}
function updateGrid(){
  const rows=cellRows();
  const sig=rows.map(r=>r.slot+':'+r.name+(r.off?'!':'')).join('|');
  if(sig!==cellSig){cellSig=sig;buildGrid(rows);}
  cells.forEach(c=>{
    if(c.row.off){c.st.textContent='no talkback';return;}
    const ch=(S.channels||[])[c.row.slot]||{};
    const hearing=ch.listeners>0;
    c.el.classList.toggle('hot',!!ch.keyed&&hearing);
    c.el.classList.toggle('armed',(!!ch.keyed&&!hearing)||(!ch.keyed&&!!ch.latched===false&&false));
    c.el.classList.toggle('ready',!ch.keyed&&hearing);
    c.el.setAttribute('aria-pressed',!!ch.keyed);
    c.st.textContent=
      ch.keyed&&hearing?(ch.latched?'on air · latched':'on air')
      :ch.keyed?'nobody in channel'
      :!ch.ready?'forming…'
      :c.row.group?(ch.listeners+' listening')
      :hearing?'ready':'invite in flight';
  });
}

/* panel-scoped keyboard: digits key the first 9 grid cells, space = all call */
const typing=e=>e.target&&(e.target.tagName==='INPUT'||e.target.tagName==='SELECT');
addEventListener('keydown',e=>{
  if(typing(e))return;
  if(e.code==='Space'){e.preventDefault();if(!e.repeat)allSet(true);return;}
  const d=e.code.startsWith('Digit')?+e.code.slice(5):0;
  const live=cells.filter(c=>!c.row.off);
  if(d>=1&&d<=live.length&&!e.repeat){e.preventDefault();live[d-1].row.setHeld(true);}
});
addEventListener('keyup',e=>{
  if(e.code==='Space'){allSet(false);return;}
  const d=e.code.startsWith('Digit')?+e.code.slice(5):0;
  const live=cells.filter(c=>!c.row.off);
  if(d>=1&&d<=live.length){e.preventDefault();live[d-1].row.setHeld(false);}
});
addEventListener('blur',()=>{cells.forEach(c=>c.row.setHeld&&c.row.setHeld(false));allSet(false);});

/* ALL CALL */
let allHeld=false;
const allSet=v=>{if(allHeld===v)return;allHeld=v;act('talkall',v?'on':'off');};
const allkey=$('allkey');
allkey.addEventListener('pointerdown',e=>{
  if(latchMode){const chans=S.channels||[];
    const allOn=chans.length&&chans.every(c=>c.latched);
    act('latchall',allOn?'off':'on');return;}
  allkey.setPointerCapture(e.pointerId);allSet(true);});
allkey.addEventListener('pointerup',()=>allSet(false));
allkey.addEventListener('pointercancel',()=>allSet(false));

/* strip + drawer */
$('latchmode').onclick=()=>{latchMode=!latchMode;$('latchmode').classList.toggle('on',latchMode);};
$('editbtn').onclick=()=>{editMode=!editMode;$('editbtn').classList.toggle('on',editMode);render();};
$('setbtn').onclick=()=>{$('drawer').classList.toggle('show');
  $('setbtn').classList.toggle('on',$('drawer').classList.contains('show'));};
$('side').onclick=()=>act('sidetone',(S.sidetone?'off':'on'));
$('aec').onclick=()=>act('aec',(S.aec?'off':'on'));
$('tone').onclick=()=>act('tone',(S.tone?'off':'on'));
$('g-dn').onclick=()=>act('gain',(S.gain-1));
$('g-up').onclick=()=>act('gain',(S.gain+1));
let micSig='',outSig='';
function fillSel(sel,list,current,sig,verb){
  const s=(list||[]).join('|')+'@'+current;
  if(s===sig.v)return sig;
  sel.innerHTML='';
  (list||[]).forEach(n=>{const o=document.createElement('option');
    o.value=n;o.textContent=n;o.selected=(n===current);sel.appendChild(o);});
  sel.onchange=()=>act(verb,sel.value);
  sig.v=s;return sig;
}
const micsig={v:''},outsig={v:''};

/* join / sign-in */
const needPass=()=>S.phase==='joining'&&/PASSCODE/.test(S.status||'');
const joinNow=()=>{
  if(S.phase==='signin'){act('signin','');$('joinstate').textContent='browser opened — approve zcomms there';return;}
  const v=$('joinurl').value.trim();
  if(!v)return;
  if(needPass()){act('passcode',v);$('joinurl').value='';$('joinstate').textContent='checking…';}
  else{act('join',v);$('joinstate').textContent='connecting…';}};
$('joinbtn').onclick=joinNow;
$('joinurl').addEventListener('keydown',e=>{if(e.key==='Enter')joinNow();});

const es=new EventSource('/events');
es.onmessage=ev=>{S=JSON.parse(ev.data);render();};
es.onerror=()=>{$('led-link').classList.remove('on');};

function render(){
  const chans=S.channels||[];
  const idle=S.phase!=='up';
  $('joincard').classList.toggle('show',idle);
  $('allkey').style.display=idle?'none':'';
  $('grid').style.display=(idle||editMode)?'none':'grid';
  $('editlist').classList.toggle('show',!idle&&editMode);
  $('deskhint').style.display=idle?'none':'';
  $('strip').style.display=idle?'none':'flex';
  const ops=$('ops');ops.innerHTML='';
  (S.log||[]).forEach(l=>{const sp=document.createElement('span');sp.textContent=l;ops.appendChild(sp);});
  if(idle){
    const pass=needPass();
    const signin=S.phase==='signin';
    $('joinstate').textContent=(S.phase==='joining')?(S.status||'connecting…')
                             :signin?'connect your Zoom account'
                                    :'paste the Zoom link';
    $('joinurl').style.display=signin?'none':'';
    $('joincard').querySelector('.plate b').textContent=
      signin?'SIGN IN WITH ZOOM':'JOIN A MEETING';
    $('joinurl').placeholder=pass?'meeting passcode'
                                 :'https://zoom.us/j/…  or meeting ID';
    $('joinbtn').textContent=signin?'SIGN IN':(pass?'SUBMIT':'CONNECT');
    $('meet').textContent=S.status||'—';
    ['led-link','led-mic','led-ch','led-tx'].forEach(i=>$(i).classList.remove('on'));
    return;
  }
  $('meet').textContent='MTG '+(S.meeting||'—')+' · '+(S.status||'');
  const anyReady=chans.some(c=>c.ready);
  const anyHearing=chans.some(c=>c.keyed&&c.listeners>0);
  $('led-link').classList.toggle('on',S.status==='IN MEETING');
  $('led-mic').classList.toggle('on',!!S.sdkmic);
  $('led-ch').classList.toggle('on',anyReady);
  $('led-tx').classList.toggle('on',!!S.talking&&anyHearing);
  const allKeyed=chans.length&&chans.every(c=>c.keyed);
  allkey.classList.toggle('hot',!!allKeyed&&anyHearing);
  allkey.classList.toggle('armed',!!allKeyed&&!anyHearing);
  allkey.setAttribute('aria-pressed',!!allKeyed);
  updateGrid();
  /* edit-talent list */
  if(editMode){
    const r=$('editlist');r.innerHTML='';
    const inUse=chans.map((c,i)=>i).filter(i=>{const c=chans[i];
      return c.listeners>0||c.label||c.keyed||c.latched;});
    (S.roster||[]).forEach(m=>{
      const li=document.createElement('li');
      li.className=m.tb?'tb':'no';
      const nm=document.createElement('span');nm.textContent=m.name;
      li.appendChild(nm);
      if(m.tb){
        const mine=chans.map((c,i)=>i).filter(i=>(m.chans||[])[i]);
        const spare=chans.findIndex((c,i)=>!inUse.includes(i)&&!mine.includes(i));
        const slots=[...new Set([...inUse,...mine,...(spare>=0?[spare]:[])])].sort((a,b)=>a-b);
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
  }
  /* settings */
  $('side').classList.toggle('on',!!S.sidetone);
  $('aec').classList.toggle('on',!!S.aec);
  $('tone').classList.toggle('on',!!S.tone);
  $('g-val').textContent=(S.gain>0?'+':'')+S.gain+' dB';
  fillSel($('micsel'),S.mics,S.mic,micsig,'setmic');
  fillSel($('outsel'),S.outs,S.out,outsig,'setout');
  $('s-send').textContent=S.sends;
  $('s-chsend').textContent=S.chsends||0;
  $('s-under').textContent=S.underruns;
  $('s-fail').textContent=S.fails;
  meter(S.peak||0);
}
</script></body></html>
)ZCUI";

}  // namespace zc
