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
.deskmsg{font:12px var(--mono);color:var(--legend);letter-spacing:.08em;
  padding:26px 0}
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
/* extern feeds */
.field input{background:var(--rack);border:1px solid var(--edge);
  color:var(--ivory);font:13px var(--mono);padding:10px 12px;min-width:0}
.field input:focus{outline:none;border-color:var(--amber)}
/* the feeds block spans the whole drawer: device names are long, and in a
   260px auto-fit column every control in here truncates to noise. */
.field.wide{grid-column:1/-1}
.feedform{display:grid;grid-template-columns:86px 1fr;gap:10px 14px;
  align-items:center;margin-top:6px}
.feedform > label{justify-self:start}
.feedhint{font:11px var(--mono);color:var(--dim);grid-column:2}
/* row per live feed, stacked: WHO hears it on top, the source under it.
   A name and a device name cannot share one line at this width without
   one of them being cut, and the one that must never be cut is the
   person -- so each gets the full text column instead. */
.feedrow{display:grid;
  grid-template-columns:8px minmax(0,1fr) auto auto auto auto auto;
  grid-template-rows:auto auto;gap:2px 10px;align-items:center;
  font:13px var(--mono);color:var(--ivory);
  padding:7px 0;border-top:1px solid var(--edge)}
.feedrow .fdot,.feedrow button,.feedrow .fgain{grid-row:1/3}
.feedrow .fwho,.feedrow .fsrc{grid-column:2;overflow:hidden;
  text-overflow:ellipsis;white-space:nowrap}
.feedrow .fwho{grid-row:1;font:600 14px var(--disp)}
.feedrow .fwho.spare{font:13px var(--mono);color:var(--legend)}
.feedrow .fsrc{grid-row:2;font:11px var(--mono);color:var(--legend)}
.feedrow .fwho i,.feedrow .fsrc i{font-style:normal;color:var(--dim)}
.feedrow .fgain{font:12px var(--mono);color:var(--legend);min-width:52px;
  text-align:center}
.feedrow .fdot{width:8px;height:8px;border-radius:50%;background:var(--edge)}
.feedrow .fdot.live{background:var(--amber)}
.feedrow .fdot.idle{background:var(--green)}
.feedrow .fdot.dead{background:var(--red)}
.feedrow button{appearance:none;border:1px solid var(--edge);
  background:var(--panel-key);color:var(--ivory);cursor:pointer;
  font:10px var(--mono);letter-spacing:.12em;padding:6px 9px}
.feedrow button.on{border-color:var(--amber);color:var(--amber)}
/* edit-talent list: one row per person, one chip per talkback channel.
   The chips used to be a computed subset (in-use ∪ theirs ∪ ONE spare), so
   the only empty channel an operator could reach was the lowest-numbered
   free one -- "the edit talent doesn't seem to do anything you can't change
   channels" (owner, 2026-09-02). The whole bank is 16 wide and always has
   been; the row has room for all of it, so the subset only ever hid reach. */
.editlist{display:none;width:100%;max-width:920px;flex-direction:column;gap:8px}
.editlist.show{display:flex}
.editlist li{list-style:none;display:flex;justify-content:space-between;
  align-items:center;gap:14px;font:12px var(--mono);color:var(--ivory);
  border-left:2px solid var(--edge);padding:6px 0 6px 10px}
.editlist li.tb{border-left-color:var(--green)}
.editlist li.no{color:var(--dim);border-left-color:var(--red)}
.editlist li.head{border-left-color:transparent;color:var(--dim);
  font-size:10px;letter-spacing:.22em;text-transform:uppercase;padding-bottom:0}
.editlist .who{display:flex;flex-direction:column;gap:3px;min-width:0}
.editlist .who b{font-weight:400;overflow:hidden;text-overflow:ellipsis;
  white-space:nowrap}
/* the instruction has to out-read the greyed-out name above it -- --dim on
   the rack fails contrast at 10px, and this line is the only way out. */
.editlist .why{font:10px var(--mono);font-style:normal;color:var(--legend);
  letter-spacing:.04em}
.editlist small{font:10px var(--mono);letter-spacing:.22em;color:var(--dim);
  flex-shrink:0}
.chips{display:flex;gap:4px;flex-wrap:wrap;justify-content:flex-end;
  flex-shrink:0;max-width:560px}
.chip{appearance:none;cursor:pointer;border:1px solid var(--edge);
  min-width:24px;height:20px;padding:0 2px;font:500 10px var(--mono);
  color:var(--dim);background:var(--panel-key)}
.chip:hover{border-color:#4A515B;color:var(--ivory)}
.chip:focus-visible{outline:1px solid var(--amber);outline-offset:1px}
/* a channel someone ELSE is already on: still clickable (a channel takes
   ten), but it must not read as free space. */
.chip.busy{color:var(--legend);border-color:var(--legend)}
.chip.on{color:#12141A;background:var(--green);border-color:var(--green)}
/* ops strip: one quiet line (the latest event); click for the scroll-back.
   The information is load-bearing mid-show (why isn't my key reaching
   anyone) but the history reading as a log wall was owner-flagged. */
.ops{border-top:1px solid var(--scribe);padding:8px 22px;background:var(--rack);
  font:12px var(--mono);color:var(--legend);min-height:32px;flex-shrink:0;
  overflow:hidden;cursor:pointer;text-transform:lowercase}
.ops span{display:block;overflow:hidden;white-space:nowrap;text-overflow:ellipsis}
.ops.open{max-height:180px;overflow-y:auto}
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
    <div class="deskmsg" id="deskmsg" style="display:none">in the meeting · loading participants…</div>
    <div class="grid" id="grid"></div>
    <ul class="editlist" id="editlist"></ul>
    <div class="hint" id="deskhint" style="display:none">hold a cell to talk · digits 1–9 direct · space = all call · latch mode makes presses stick</div>
  </div>
  <div class="drawer" id="drawer">
    <div class="field"><label>microphone</label>
      <select id="micsel"></select></div>
    <div class="field"><label>sidetone output</label>
      <select id="outsel"></select></div>
    <div class="field" id="roomfield" style="display:none"><label>station room</label>
      <select id="roomsel"></select></div>
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
    <div class="field wide"><label>extern feeds — a device channel latched to whoever is on a comms channel</label>
      <div id="feedlist"></div>
      <div class="feedform">
        <label for="feeddev">source</label>
        <select id="feeddev" title="capture device"></select>
        <label for="feedchansel">channel</label>
        <div class="row">
          <select id="feedchansel" title="device channel" style="flex:1;min-width:0"></select>
          <input id="feedchans" placeholder="e.g. 3 or 3-4" style="display:none;flex:1;min-width:0">
        </div>
        <div class="feedhint" id="feedhint"></div>
        <label for="feedslot">heard by</label>
        <div class="row">
          <select id="feedslot" title="who this feed goes to"
                  style="flex:1;min-width:0"></select>
          <button class="tog" id="feedset" style="flex:none">SET</button>
        </div>
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
    <button class="tog" id="leavebtn" title="leave the meeting, keep the app">LEAVE</button>
    <div class="hmeter" id="hmeter" title="mic level"></div>
  </div>
  <div class="ops" id="ops" title="ops log — click to expand"><span>panel ready — waiting for the station…</span></div>
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
        const c=(S.channels||[])[r.slot]||{};
        // Fail closed: talkback cannot cross rooms -- refuse a key whose
        // whole population is elsewhere.
        const unreach=c.reach&&c.reach.ok.length===0&&c.reach.dark.length>0;
        if(c.room||unreach)return;
        if(latchMode){act('latch',r.slot+' '+(c.latched?'off':'on'));return;}
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
    const reach=ch.reach||null;
    const unreach=reach&&reach.ok.length===0&&reach.dark.length>0;
    if(ch.room||unreach){
      // Cross-room: unreachable from here, and the cell says where they are.
      c.el.classList.add('off');
      c.el.classList.remove('hot','armed','ready');
      c.st.textContent=unreach?(reach.dark.length===1
          ?('in '+(reach.dark[0][1]||'main'))
          :'all in other rooms')
        :('in '+ch.room);
      return;
    }
    c.el.classList.remove('off');
    // A group with part of its membership elsewhere stays keyable but
    // says who is missing.
    const partial=reach&&reach.dark.length>0?(' · '+reach.dark.length+' elsewhere'):'';
    const hearing=ch.listeners>0;
    c.el.classList.toggle('hot',!!ch.keyed&&hearing);
    c.el.classList.toggle('armed',(!!ch.keyed&&!hearing)||(!ch.keyed&&!!ch.latched===false&&false));
    c.el.classList.toggle('ready',!ch.keyed&&hearing);
    c.el.setAttribute('aria-pressed',!!ch.keyed);
    c.st.textContent=(
      ch.keyed&&hearing?(ch.latched?'on air · latched':'on air')
      :ch.keyed?'nobody in channel'
      :!ch.ready?'forming…'
      :c.row.group?(ch.listeners+' listening')
      :hearing?'ready':'invite in flight')+partial;
    const fd=(S.feeds||[]).find(f=>f.slot===c.row.slot);
    if(fd&&fd.latch)c.st.textContent+=
      !fd.ok?' · FEED DEAD':(fd.peak>103?' · FEED ●':' · feed');
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
$('ops').onclick=()=>{$('ops').classList.toggle('open');render();};
$('aec').onclick=()=>act('aec',(S.aec?'off':'on'));
$('tone').onclick=()=>act('tone',(S.tone?'off':'on'));
$('g-dn').onclick=()=>act('gain',(S.gain-1));
$('g-up').onclick=()=>act('gain',(S.gain+1));

/* extern feeds: a device channel latched into a comms channel. State is
   server truth (S.feeds); rows are stateless buttons over verbs. */
const feeddevsig={v:''},feedchsig={v:''},feedslotsig={v:''};
/* A channel is a PERSON here, never a number: the panel already knows who
   is on each slot, so the operator should never have to translate
   "CH 7" back into a name. The number stays as the secondary detail
   because it is what the ops log and the verbs speak. */
function slotWho(i){
  const c=(S.channels||[])[i]||{};
  if(c.label)return{name:c.label,spare:false};
  if(c.listeners>0)return{name:c.listeners+' listening',spare:false};
  return{name:'',spare:true};}
function renderSlotPick(){
  const sel=$('feedslot');
  const sig=[];for(let i=0;i<16;i++)sig.push(slotWho(i).name);
  const s=sig.join('|');if(s===feedslotsig.v)return;feedslotsig.v=s;
  const keep=sel.value;sel.innerHTML='';
  const grp=t=>{const g=document.createElement('optgroup');g.label=t;
    sel.appendChild(g);return g;};
  let people=null,spares=null;
  for(let i=0;i<16;i++){
    const w=slotWho(i),o=document.createElement('option');o.value=i;
    if(w.spare){
      // A feed can be latched into a channel nobody is on yet, so spares
      // stay selectable -- just below the people, and honest about it.
      spares=spares||grp('spare channels');
      o.textContent='ch '+(i+1)+' — nobody on it yet';spares.appendChild(o);
    }else{
      people=people||grp('on a channel now');
      o.textContent=w.name+'  ·  ch '+(i+1);people.appendChild(o);}}
  if([].some.call(sel.options,o=>o.value===keep))sel.value=keep;}
/* Channel picks come from the device's own native count (S.micchans, parallel
   to S.mics). 0 means the backend would not say -- then the free-text field
   takes over rather than us inventing a channel map for hardware we cannot
   see. */
function devChannels(name){
  const i=(S.mics||[]).indexOf(name);
  const n=(S.micchans||[])[i];
  return (typeof n==='number'&&n>0)?n:0;}
function renderChanPick(){
  const dev=$('feeddev').value,n=devChannels(dev),sig=dev+'@'+n;
  if(sig===feedchsig.v)return;feedchsig.v=sig;
  const sel=$('feedchansel'),txt=$('feedchans'),hint=$('feedhint');
  if(!n){sel.style.display='none';txt.style.display='';
    hint.textContent=dev?'this device reports no channel count — type one':'';
    return;}
  sel.style.display='';txt.style.display='none';
  const keep=sel.value;sel.innerHTML='';
  const add=(v,t)=>{const o=document.createElement('option');
    o.value=v;o.textContent=t;sel.appendChild(o);};
  for(let c=1;c<=n;c++)add(String(c),'ch '+c+' — mono');
  for(let c=1;c+1<=n;c+=2)add(c+'-'+(c+1),'ch '+c+'-'+(c+1)+' — stereo pair');
  if([].some.call(sel.options,o=>o.value===keep))sel.value=keep;
  hint.textContent=n+' channel'+(n===1?'':'s')+' on this device';}
$('feeddev').onchange=renderChanPick;
function chanValue(){
  const sel=$('feedchansel');
  return sel.style.display==='none'?$('feedchans'):sel;}
$('feedset').onclick=()=>{
  const el=chanValue(),ch=el.value.trim();
  if(!/^\d+(-\d+)?$/.test(ch)){el.focus();return;}
  act('feed','set '+$('feedslot').value+' '+$('feeddev').value+':'+ch);};
function renderFeeds(){
  const devs=(S.mics||[]).join('|');
  if(devs!==feeddevsig.v){feeddevsig.v=devs;const s=$('feeddev');
    const keep=s.value;s.innerHTML='';
    (S.mics||[]).forEach(n=>{const o=document.createElement('option');
      o.value=n;o.textContent=n;s.appendChild(o);});
    if((S.mics||[]).indexOf(keep)>=0)s.value=keep;}
  renderChanPick();renderSlotPick();
  const fl=$('feedlist');fl.innerHTML='';
  (S.feeds||[]).forEach(f=>{
    const row=document.createElement('div');row.className='feedrow';
    const dot=document.createElement('i');
    /* 103 ~ -50 dBFS: the same threshold the duck's signal gate uses */
    dot.className='fdot '+(!f.ok?'dead':(f.latch&&f.peak>103?'live':'idle'));
    dot.title=!f.ok?'capture device dead'
      :(f.latch?(f.peak>103?'latched · audio flowing':'latched · silent'):'unlatched');
    /* spec is "<device>:<chans>" -- split at the LAST colon so device names
       carrying one of their own survive intact. */
    const cut=f.spec.lastIndexOf(':');
    const dev=cut<0?f.spec:f.spec.slice(0,cut);
    const chs=cut<0?'':f.spec.slice(cut+1);
    /* Lead with who hears this feed, not the slot it lands on. */
    const w=slotWho(f.slot);
    const who=document.createElement('span');
    who.className='fwho'+(w.spare?' spare':'');
    who.textContent=w.spare?('ch '+(f.slot+1)+' — nobody yet'):w.name;
    const wn=document.createElement('i');
    wn.textContent=w.spare?'':(' · ch '+(f.slot+1));
    who.appendChild(wn);
    who.title=who.textContent;
    const sp=document.createElement('span');sp.className='fsrc';
    const ar=document.createElement('i');ar.textContent='← ';
    sp.append(ar,document.createTextNode(dev+' '));
    const ci=document.createElement('i');ci.textContent=chs?'ch '+chs:'';
    sp.appendChild(ci);
    sp.title='← '+f.spec;
    const gv=document.createElement('span');gv.className='fgain';
    gv.textContent=(f.gain>0?'+':'')+f.gain+' dB';
    const gd=document.createElement('button');gd.textContent='−';
    gd.title='feed gain down';
    gd.onclick=()=>act('feed','gain '+f.slot+' '+(f.gain-1));
    const gu=document.createElement('button');gu.textContent='+';
    gu.title='feed gain up';
    gu.onclick=()=>act('feed','gain '+f.slot+' '+(f.gain+1));
    const lb=document.createElement('button');if(f.latch)lb.className='on';
    lb.textContent='LATCH';
    lb.onclick=()=>act('feed','latch '+f.slot+' '+(f.latch?'off':'on'));
    const rm=document.createElement('button');rm.textContent='✕';
    rm.title='remove feed';rm.onclick=()=>act('feed','off '+f.slot);
    row.append(dot,who,sp,gd,gv,gu,lb,rm);fl.appendChild(row);
  });
}
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
const micsig={v:''},outsig={v:''},roomsig={v:''};

/* join / sign-in */
const needPass=()=>S.phase==='joining'&&/PASSCODE/.test(S.status||'');
const joinNow=()=>{
  if(S.phase==='signin'){act('signin','');$('joinstate').textContent='browser opened — approve zcomms there';return;}
  if(S.phase==='joining'&&!needPass()){act('leave','');$('joinstate').textContent='cancelling…';return;}
  const v=$('joinurl').value.trim();
  if(!v)return;
  if(needPass()){act('passcode',v);$('joinurl').value='';$('joinstate').textContent='checking…';}
  else{act('join',v);$('joinstate').textContent='connecting…';}};
$('leavebtn').onclick=()=>act('leave','');
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
  if(idle)$('deskmsg').style.display='none';
  $('grid').style.display=(idle||editMode)?'none':'grid';
  $('editlist').classList.toggle('show',!idle&&editMode);
  $('deskhint').style.display=idle?'none':'';
  $('strip').style.display=idle?'none':'flex';
  const ops=$('ops');ops.innerHTML='';
  const lines=S.log||[];
  const shown=ops.classList.contains('open')?lines.slice().reverse():lines.slice(-1);
  shown.forEach(l=>{const sp=document.createElement('span');sp.textContent=l;ops.appendChild(sp);});
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
    $('joinbtn').textContent=signin?'SIGN IN'
      :(pass?'SUBMIT':(S.phase==='joining'?'CANCEL':'CONNECT'));
    $('meet').textContent=S.status||'—';
    ['led-link','led-mic','led-ch','led-tx'].forEach(i=>$(i).classList.remove('on'));
    return;
  }
  $('meet').textContent='MTG '+(S.meeting||'—')+' · '+(S.status||'')
    +(S.bostarted?(' · ROOM '+(S.boroom||'MAIN')):'');
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
  // Between "in the meeting" and the first person landing, say so rather
  // than presenting an empty desk as a mystery.
  const anyCells=cells.length>0;
  $('deskmsg').style.display=(!editMode&&!anyCells)?'':'none';
  if(!anyCells){
    $('deskmsg').textContent=(S.roster||[]).length
      ?'in the meeting · channels forming…'
      :'in the meeting · waiting for participants…';
  }
  /* edit-talent list */
  if(editMode){
    const r=$('editlist');r.innerHTML='';
    // Who is on each channel, from the same server truth the chips toggle
    // (roster.chans = confirmed membership OR pending intent), so a chip
    // lights on the next state frame rather than waiting for Zoom's async
    // join confirmation.
    const occupants=chans.map((c,i)=>
      (S.roster||[]).filter(p=>(p.chans||[])[i]));
    if((S.roster||[]).length&&chans.length){
      const h=document.createElement('li');h.className='head';
      const hl=document.createElement('span');hl.textContent='talent';
      const hr=document.createElement('span');
      hr.textContent='channel — click a number to put someone on it';
      h.append(hl,hr);r.appendChild(h);
    }
    (S.roster||[]).forEach(m=>{
      const li=document.createElement('li');
      li.className=m.tb?'tb':'no';
      const who=document.createElement('span');who.className='who';
      const nm=document.createElement('b');
      nm.textContent=m.name+(m.room?('  · in '+m.room):'');
      nm.title=m.name;
      who.appendChild(nm);
      if(m.tb){
        li.appendChild(who);
        const chips=document.createElement('span');chips.className='chips';
        // Every channel, always: the operator must be able to reach the
        // channel they actually want, not just the lowest free one.
        chans.forEach((c,i)=>{
          const on=(m.chans||[])[i];
          const others=occupants[i].filter(p=>p.uid!==m.uid).map(p=>p.name);
          const b=document.createElement('button');
          b.className='chip'+(on?' on':(others.length?' busy':''));
          b.textContent=i+1;
          b.title=(on?'take off ':'put on ')+(c.label||('CH '+(i+1)))
            +(others.length?(' — with '+others.join(', '))
                           :(on?'':' — empty'));
          b.onclick=()=>act('assign',i+':'+m.uid+' '+(on?'off':'on'));
          chips.appendChild(b);
        });
        li.appendChild(chips);
      }else{
        // A web-client participant physically cannot receive talkback
        // (IUserInfo::IsSupportTalkback false; the invite itself fails
        // INVALID_PARAMETER). A bare NO TALKBACK tag with no chips beside
        // it is what the whole list looked like broken from -- four of ten
        // rows dead and unexplained. Say why, and say the one fix.
        const why=document.createElement('em');why.className='why';
        why.textContent=
          'on the Zoom web client — ask them to rejoin in the desktop app';
        who.appendChild(why);
        li.appendChild(who);
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
  renderFeeds();
  /* station room: MAIN + every breakout, applied via the room verb */
  $('roomfield').style.display=S.bostarted?'':'none';
  if(S.bostarted){
    const rs=$('roomsel');
    const sig=(S.rooms||[]).map(r=>r.id).join('|')+'@'+(S.boroom||'');
    if(sig!==roomsig.v){
      roomsig.v=sig;rs.innerHTML='';
      const mk=(v,label,sel)=>{const o=document.createElement('option');
        o.value=v;o.textContent=label;o.selected=sel;rs.appendChild(o);};
      mk('main','MAIN',!(S.boroom));
      (S.rooms||[]).forEach(r=>mk(r.id,r.name,r.name===S.boroom));
      rs.onchange=()=>act('room',rs.value);
    }
  }
  $('s-send').textContent=S.sends;
  $('s-chsend').textContent=S.chsends||0;
  $('s-under').textContent=S.underruns;
  $('s-fail').textContent=S.fails;
  meter(S.peak||0);
}
</script></body></html>
)ZCUI";

}  // namespace zc
