// dashboard.cpp — the embedded dashboard HTML/JS, kept separate from server logic.
// Two stacked charts (humidity + temperature). The humidity chart overlays
// dashed threshold lines (suboptimal/critical) pulled from /api/config, and
// shades the optimal band. Sensor-error points (null) leave a gap in the line.
const char* kDashboardHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>AM2302 Dashboard</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin:0; font-family: system-ui, sans-serif; background:#0e1116; color:#e6edf3; }
  header { padding:18px 24px; border-bottom:1px solid #222a33; display:flex;
           align-items:baseline; gap:16px; flex-wrap:wrap; }
  header h1 { margin:0; font-size:18px; font-weight:600; }
  #status { color:#7d8590; font-size:13px; }
  #status.stale { color:#f85149; }
  #status.err   { color:#d29922; }
  .cards { display:flex; gap:16px; padding:20px 24px 8px; flex-wrap:wrap; }
  .card { background:#161b22; border:1px solid #222a33; border-radius:12px;
          padding:16px 22px; min-width:150px; }
  .card .label { color:#7d8590; font-size:12px; text-transform:uppercase; letter-spacing:.04em; }
  .card .value { font-size:36px; font-weight:700; margin-top:2px; }
  .card .unit  { font-size:16px; color:#7d8590; }
  .pill { display:inline-block; padding:2px 8px; border-radius:999px; font-size:11px;
          font-weight:600; margin-top:6px; }
  .pill.optimal   { background:#1b3a23; color:#3fb950; }
  .pill.suboptimal{ background:#3a2f12; color:#d29922; }
  .pill.critical  { background:#3a1518; color:#f85149; }
  .chart-wrap { padding:8px 24px; }
  .chart-wrap h2 { font-size:13px; font-weight:600; color:#adbac7; margin:8px 0 4px; }
  svg { width:100%; height:220px; background:#161b22; border:1px solid #222a33; border-radius:12px; }
  .actions { padding:12px 24px 28px; }
  .btn { background:#21262d; border:1px solid #30363d; color:#e6edf3; padding:8px 16px;
         border-radius:8px; text-decoration:none; font-size:13px; cursor:pointer; }
  .btn:hover { background:#2d333b; }
  footer { color:#7d8590; font-size:12px; padding:0 24px 24px; }
  .legend { font-size:11px; color:#7d8590; margin-left:auto; }
</style>
</head>
<body>
  <header>
    <h1>AM2302 Sensor</h1>
    <span id="status">connecting...</span>
  </header>

  <div class="cards">
    <div class="card">
      <div class="label">Humidity</div>
      <div class="value"><span id="h">--</span><span class="unit"> %</span></div>
      <div id="hpill" class="pill">--</div>
    </div>
    <div class="card">
      <div class="label">Temperature</div>
      <div class="value"><span id="t">--</span><span class="unit"> &deg;C</span></div>
    </div>
  </div>

  <div class="chart-wrap">
    <h2>Humidity (%RH) &mdash; dashed: suboptimal &middot; dotted: critical</h2>
    <svg id="humChart" preserveAspectRatio="none"></svg>
  </div>
  <div class="chart-wrap">
    <h2>Temperature (&deg;C)</h2>
    <svg id="tempChart" preserveAspectRatio="none"></svg>
  </div>

  <div class="actions">
    <a class="btn" href="/api/export.csv" download>Export CSV</a>
  </div>
  <footer>Updates every 2s &middot; disk-backed history &middot; dht_server (C++)</footer>

<script>
const H=220, padL=44, padR=14, padT=14, padB=22;
const MIN_TEMP_RANGE = 4;   // degrees C: floor so small wiggles stay small
const MIN_HUM_PAD    = 3;   // %RH padding around data/thresholds
let cfg = null;
// Width is read live from each chart element so 1 SVG unit == 1 screen pixel
// (no horizontal stretch -> axis text and dashes render undistorted).
function chartW(svg){ return Math.max(320, Math.round(svg.clientWidth || svg.parentNode.clientWidth || 800)); }

function classify(h){
  if(h==null) return null;
  if(h < cfg.hum_crit_low || h > cfg.hum_crit_high) return 'critical';
  if(h < cfg.hum_subopt_low || h > cfg.hum_subopt_high) return 'suboptimal';
  return 'optimal';
}

// Build an SVG line that breaks at null gaps (sensor errors).
function linePath(vals, min, max, W){
  const span=(max-min)||1; const n=vals.length;
  let d=""; let pen=false;
  for(let i=0;i<n;i++){
    const v=vals[i];
    if(v==null){ pen=false; continue; }
    const x=padL+(W-padL-padR)*(n<2?0:i/(n-1));
    const y=H-padB-(H-padT-padB)*(v-min)/span;
    d += (pen?" L":" M")+x.toFixed(1)+","+y.toFixed(1);
    pen=true;
  }
  return d.trim();
}
function yFor(v,min,max){ const span=(max-min)||1; return H-padB-(H-padT-padB)*(v-min)/span; }

function hline(yVal,min,max,color,dash,W){
  const y=yFor(yVal,min,max);
  return `<line x1="${padL}" y1="${y.toFixed(1)}" x2="${W-padR}" y2="${y.toFixed(1)}" stroke="${color}" stroke-width="1" stroke-dasharray="${dash}"/>`;
}
function band(yLo,yHi,min,max,fill,W){
  const y1=yFor(yHi,min,max), y2=yFor(yLo,min,max);
  return `<rect x="${padL}" y="${y1.toFixed(1)}" width="${W-padL-padR}" height="${(y2-y1).toFixed(1)}" fill="${fill}"/>`;
}
function axisLabels(min,max){
  const mid=(min+max)/2;
  const mk=(v)=>`<text x="6" y="${(yFor(v,min,max)+4).toFixed(1)}" fill="#7d8590" font-size="11">${v.toFixed(0)}</text>`;
  return mk(max)+mk(mid)+mk(min);
}

function renderHum(vals){
  const svg=document.getElementById('humChart');
  const W=chartW(svg);
  svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
  const real=vals.filter(v=>v!=null);
  let lo = Math.min(cfg.hum_crit_low, real.length?Math.min(...real):cfg.hum_crit_low)-MIN_HUM_PAD;
  let hi = Math.max(cfg.hum_crit_high, real.length?Math.max(...real):cfg.hum_crit_high)+MIN_HUM_PAD;
  let parts="";
  parts += band(cfg.hum_subopt_low, cfg.hum_subopt_high, lo, hi, "rgba(63,185,80,0.08)", W);
  parts += hline(cfg.hum_subopt_low,  lo, hi, "#d29922", "6 4", W);
  parts += hline(cfg.hum_subopt_high, lo, hi, "#d29922", "6 4", W);
  parts += hline(cfg.hum_crit_low,  lo, hi, "#f85149", "2 4", W);
  parts += hline(cfg.hum_crit_high, lo, hi, "#f85149", "2 4", W);
  parts += axisLabels(lo,hi);
  const d=linePath(vals, lo, hi, W);
  if(d) parts += `<path d="${d}" fill="none" stroke="#58a6ff" stroke-width="2"/>`;
  svg.innerHTML=parts;
}
function renderTemp(vals){
  const svg=document.getElementById('tempChart');
  const W=chartW(svg);
  svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
  const real=vals.filter(v=>v!=null);
  if(!real.length){ svg.innerHTML=""; return; }
  let dmin=Math.min(...real), dmax=Math.max(...real), mid=(dmin+dmax)/2;
  // enforce a minimum visible range so a ~1C wiggle doesn't fill the chart
  let half=Math.max((dmax-dmin)/2, MIN_TEMP_RANGE/2)+0.5;
  let lo=mid-half, hi=mid+half;
  let parts=axisLabels(lo,hi);
  const d=linePath(vals, lo, hi, W);
  if(d) parts += `<path d="${d}" fill="none" stroke="#f0883e" stroke-width="2"/>`;
  svg.innerHTML=parts;
}

async function tick(){
  try{
    if(!cfg){ cfg = await (await fetch('/api/config')).json(); }
    const d = await (await fetch('/api/latest')).json();
    const st=document.getElementById('status');
    const hpill=document.getElementById('hpill');
    if(d.ok && d.status==='ok'){
      document.getElementById('h').textContent=d.humidity.toFixed(1);
      document.getElementById('t').textContent=d.temperature.toFixed(1);
      const cls=classify(d.humidity);
      hpill.textContent=cls; hpill.className='pill '+cls;
      const ageS=Math.round(d.age_ms/1000);
      const stale=d.age_ms>8000;
      st.textContent = stale ? ('stale ('+ageS+'s old)') : ('live, '+ageS+'s ago');
      st.className = stale ? 'stale' : '';
    } else if(d.ok && d.status==='error'){
      document.getElementById('h').textContent='--';
      document.getElementById('t').textContent='--';
      hpill.textContent='sensor error'; hpill.className='pill critical';
      st.textContent='connected, sensor read error';
      st.className='err';
    } else {
      st.textContent='waiting for first reading...';
    }
    const hist = await (await fetch('/api/history')).json();
    lastHist = hist;
    renderHum(hist.humidity);
    renderTemp(hist.temperature);
  }catch(e){
    document.getElementById('status').textContent='server unreachable';
  }
}
let lastHist = null;
let rzTimer = null;
window.addEventListener('resize', ()=>{
  clearTimeout(rzTimer);
  rzTimer = setTimeout(()=>{
    if(lastHist){ renderHum(lastHist.humidity); renderTemp(lastHist.temperature); }
  }, 150);
});
tick(); setInterval(tick, 2000);
</script>
</body>
</html>)HTML";
