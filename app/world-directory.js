import zones from './timezones.js';

// Search never runs on the cube; no stock API secret is copied to the phone.
export const asciiLabel=(text,max=24)=>String(text||'').normalize('NFKD').replace(/[\u0300-\u036f]/g,'').replace(/[^\x20-\x7e]/g,'').trim().slice(0,max)||'Location';
export function cityRecord(city,app) {
  if(!Number.isFinite(city.latitude)||!Number.isFinite(city.longitude))throw Error('Location has no coordinates.');
  if(app==='weather')return {name:asciiLabel(city.name),country:city.country_code||'??',lat:city.latitude,lon:city.longitude};
  const alias={'Europe/Kyiv':'Europe/Kiev','Asia/Calcutta':'Asia/Kolkata','Asia/Katmandu':'Asia/Kathmandu','UTC':'Etc/UTC'};
  const zone=alias[city.timezone]||city.timezone;
  // These regions have irregular/political transitions not representable by recurring POSIX rules.
  if(['Africa/Casablanca','Africa/El_Aaiun','Asia/Gaza','Asia/Hebron'].includes(zone))throw Error('This zone has irregular clock changes; automatic clock support is not available yet. Weather is supported.');
  let tz=zones[zone];if(!tz)throw Error('No verified clock rules for this time zone yet.');
  let part=0;tz=tz.replace(/<[^>]+>/g,()=>part++?'DST':'LOC');
  return {name:asciiLabel(city.name),tz};
}
export function radioRecord(station) {
  if(String(station.codec).toUpperCase()!=='MP3'||Number(station.hls)!==0)throw Error('Requires a direct MP3 stream (no HLS).');
  if(Number(station.bitrate)>192)throw Error('Bitrate exceeds the cube’s 192 kbps selection limit.');
  const url=new URL(station.url_resolved||station.url);
  if(url.protocol!=='http:'||url.username||url.password)throw Error('This cube currently requires a public HTTP MP3 stream; HTTPS is not downgraded.');
  if(!/^[a-z0-9.-]{1,96}$/i.test(url.hostname)||url.pathname.length+url.search.length>192)throw Error('Stream address is too long for this firmware.');
  if(/\.(m3u8?|pls)$/i.test(url.pathname))throw Error('Playlist address: a direct MP3 stream is required.');
  return {name:asciiLabel(station.name,32),host:url.hostname,path:url.pathname+url.search,port:Number(url.port||80)};
}
export function stockRecord(stock) {
  if(!/^[A-Z0-9.-]{1,12}$/.test(stock.symbol)||!stock.exchange||stock.exchange.length>24)throw Error('Symbol or exchange exceeds this firmware’s supported format.');
  return {symbol:stock.symbol,exchange:stock.exchange,currency:/^[A-Z]{3}$/.test(stock.currency)?stock.currency:''};
}
export async function searchDirectory(app,query,getJSON,offset=0,includeUnsupported=false) {
  query=query.trim();if(app!=='radio'&&query.length<(app==='stock'?1:2))throw Error(app==='stock'?'Enter a ticker.':'Enter at least two characters.');
  if(app==='clock'||app==='weather') {
    const data=await getJSON('https://geocoding-api.open-meteo.com/v1/search?count=100&language=en&name='+encodeURIComponent(query));
    if(data.error)throw Error(data.reason||'City search failed.');
    return {items:(data.results||[]).map(city=>({label:[city.name,city.admin1,city.country,city.timezone].filter(Boolean).join(' · '),convert:()=>cityRecord(city,app)})),more:false};
  }
  if(app==='stock') {
    if(!/^[A-Z0-9.-]{1,12}$/i.test(query))throw Error('Enter an exact ticker, for example AAPL, SAP or 7203.');
    const data=await getJSON('https://api.twelvedata.com/stocks?symbol='+encodeURIComponent(query.toUpperCase()));
    if(data.status!=='ok'||!Array.isArray(data.data))throw Error(data.message||'Stock directory unavailable.');
    return {items:data.data.filter(s=>s.symbol===query.toUpperCase()).map(s=>({label:`${s.symbol} · ${s.name} · ${s.exchange} · ${s.country} · ${s.currency}`,convert:()=>stockRecord(s)})),more:false};
  }
  let error;
  for(const server of ['de1','nl1','at1'])try {
    const filters=includeUnsupported?'':'&codec=MP3&is_https=false&bitrateMax=192';
    const data=await getJSON(`https://${server}.api.radio-browser.info/json/stations/search?name=${encodeURIComponent(query)}&hidebroken=true&order=clickcount&reverse=true&limit=50&offset=${offset}${filters}`);
    if(!Array.isArray(data))throw Error('Invalid radio directory response.');
    return {items:data.map(s=>({label:`${s.name} · ${s.country} · ${s.codec} ${s.bitrate||'?'} kbps`,convert:()=>radioRecord(s)})),more:data.length===50};
  }catch(e){error=e;}
  throw error||Error('Radio directory unavailable.');
}
export const encodeRecord=record=>{
  const json=JSON.stringify(record);
  if(!/^[\x20-\x7e]+$/.test(json)||json.length>511)throw Error('Selection is too large or unsupported.');
  return json;
};
const toHex=text=>[...text].map(c=>c.charCodeAt(0).toString(16).padStart(2,'0')).join('');
export function createRecordLink(send) {
  let pending=null,id=100,tx=Date.now()%1000000,epoch=0;
  const check=token=>{if(token!==epoch)throw Error('Settings transfer cancelled. Reload before retrying.');};
  const command=payload=>new Promise((resolve,reject)=>{
    if(pending)return reject(Error('Another settings transfer is active.'));
    const packet={type:'face_record',id:++id,...payload};
    if(JSON.stringify(packet).length>182)return reject(Error('Bluetooth packet exceeds safe MTU.'));
    const timer=setTimeout(()=>{pending=null;reject(Error('No confirmation. Reconnect/reload; the latest firmware is required.'));},12000);
    pending={id,resolve,reject,timer};
    Promise.resolve(send(packet)).then(ok=>{if(!ok&&pending?.id===packet.id){clearTimeout(timer);pending=null;reject(Error('Bluetooth write failed.'));}},e=>{if(pending?.id===packet.id){clearTimeout(timer);pending=null;reject(e);}});
  });
  return {
    receive(data){if(data.type!=='face_record')return false;if(pending&&data.id===pending.id){const p=pending;pending=null;clearTimeout(p.timer);data.status==='ok'?p.resolve(data):p.reject(Error('Cube rejected this selection.'));}return true;},
    cancel(){epoch++;if(pending){const p=pending;pending=null;clearTimeout(p.timer);p.reject(Error('Disconnected. Reload to confirm saved settings.'));}},
    async read(app,side){const token=epoch;let text='',length;do{check(token);const r=await command({app,side,op:'read',offset:text.length});check(token);if(!/^(?:[0-9a-f]{2}){1,40}$/.test(r.data)||!Number.isInteger(r.length)||r.length>511)throw Error('Invalid settings response.');length=r.length;text+=r.data.match(/../g).map(h=>String.fromCharCode(parseInt(h,16))).join('');}while(text.length<length);if(text.length!==length)throw Error('Incomplete settings response.');return JSON.parse(text);},
    async save(app,side,record){const token=epoch,text=encodeRecord(record),transaction=++tx;await command({app,side,op:'begin',tx:transaction,length:text.length});check(token);for(let offset=0;offset<text.length;offset+=40){await command({app,side,op:'part',tx:transaction,offset,data:toHex(text.slice(offset,offset+40))});check(token);}await command({app,side,op:'save',tx:transaction});check(token);},
  };
}


const starterCities=[
  ['New York','US',40.7128,-74.006,'America/New_York'],
  ['Los Angeles','US',34.0522,-118.2437,'America/Los_Angeles'],
  ['London','GB',51.5074,-0.1278,'Europe/London'],
  ['Tokyo','JP',35.6762,139.6503,'Asia/Tokyo'],
  ['Dubai','AE',25.2048,55.2708,'Asia/Dubai'],
  ['Tehran','IR',35.6892,51.389,'Asia/Tehran'],
  ['Paris','FR',48.8566,2.3522,'Europe/Paris'],
  ['Berlin','DE',52.52,13.405,'Europe/Berlin'],
  ['Sydney','AU',-33.8688,151.2093,'Australia/Sydney'],
  ['Toronto','CA',43.6532,-79.3832,'America/Toronto'],
  ['Vancouver','CA',49.2827,-123.1207,'America/Vancouver'],
  ['Singapore','SG',1.3521,103.8198,'Asia/Singapore'],
  ['Hong Kong','HK',22.3193,114.1694,'Asia/Hong_Kong'],
  ['Mumbai','IN',19.076,72.8777,'Asia/Kolkata'],
  ['Seoul','KR',37.5665,126.978,'Asia/Seoul'],
  ['Rome','IT',41.9028,12.4964,'Europe/Rome'],
  ['Madrid','ES',40.4168,-3.7038,'Europe/Madrid'],
  ['Istanbul','TR',41.0082,28.9784,'Europe/Istanbul'],
  ['Auckland','NZ',-36.8485,174.7633,'Pacific/Auckland'],
  ['Sao Paulo','BR',-23.5505,-46.6333,'America/Sao_Paulo'],
].map(([name,country_code,latitude,longitude,timezone])=>({name,country_code,latitude,longitude,timezone}));
export function starterChoices(app) {
  if(app==='clock'||app==='weather')return starterCities.map(city=>({label:city.name+' · '+city.country_code,convert:()=>cityRecord(city,app)}));
  if(app==='stock')return ['TSLA','GOOGL','AMZN','MSFT','NVDA','AAPL'].map(symbol=>({label:symbol+' · NASDAQ · USD',convert:()=>({symbol,exchange:'NASDAQ',currency:'USD'})}));
  return [];
}

// One inline, searchable dropdown per side. A selection is saved only after ACK.
// Popular entries render immediately; online results are bounded and loaded on demand.
export function setupWorldDirectory({send,connected,getJSON,lock}) {
  const link=createRecordLink(send),cache=new Map();
  let generation=0,working=false,rows=[];
  const text=record=>record.symbol?record.symbol+(record.exchange?' · '+record.exchange:''):record.name;
  function setWorking(value) {
    working=value;lock(value);
    for(const row of rows) {
      row.summary.setAttribute('aria-disabled',String(value||!row.ready||!connected()));
      row.query.disabled=value||!row.ready||!connected();
      row.more.disabled=value||!row.ready||!connected();
      row.list.querySelectorAll('button').forEach(b=>b.disabled=value||!row.ready||!connected()||b.dataset.unsupported==='true');
    }
  }
  function cancel() {
    generation++;link.cancel();
    for(const row of rows){clearTimeout(row.timer);row.version++;row.details.open=false;}
    rows=[];setWorking(false);
  }
  async function lookup(app,query,offset) {
    const key=JSON.stringify([app,query,offset]);
    if(cache.has(key))return cache.get(key);
    const result=await searchDirectory(app,query,getJSON,offset);
    if(cache.size>=20)cache.delete(cache.keys().next().value);
    cache.set(key,result);return result;
  }
  return {
    receive:link.receive,cancel,
    bind(app,fields,values=[],catalogs={}) {
      const current=++generation;
      if(!['clock','weather','stock','radio'].includes(app))return;
      rows=[];
      [...fields.querySelectorAll('.face-setting')].forEach((container,side)=>{
        container.querySelector('.face-picker')?.remove();
        const old=container.querySelector('input,select');if(old)old.remove();
        const details=document.createElement('details');details.className='face-picker';
        const summary=document.createElement('summary');summary.textContent='Loading…';summary.setAttribute('aria-label',app+' for side '+(side+1));
        const panel=document.createElement('div');panel.className='face-picker-panel';
        const query=document.createElement('input');query.type='search';query.autocomplete='off';
        query.placeholder=app==='stock'?'Type a ticker…':app==='radio'?'Filter stations…':'Filter cities, any country…';
        query.setAttribute('aria-label',query.placeholder+' Side '+(side+1));
        const message=document.createElement('small');message.setAttribute('role','status');
        const list=document.createElement('div');list.className='face-picker-results';
        const more=document.createElement('button');more.type='button';more.textContent='Load more stations';more.hidden=true;
        panel.append(query,message,list,more);details.append(summary,panel);container.append(details);
        const row={details,summary,query,message,list,more,ready:false,version:0,offset:0,timer:null};
        rows.push(row);
        const alive=()=>current===generation;
        async function choose(record) {
          if(!alive()||working||!row.ready||!connected())return;
          row.version++;clearTimeout(row.timer);setWorking(true);message.textContent='Saving on cube…';
          try {
            await link.save(app,side,record);
            if(!alive())return;
            summary.textContent=text(record);row.record=record;message.textContent='Saved on cube.';details.open=false;summary.focus();
          } catch(e){if(alive())message.textContent=e.message;}
          finally{if(alive())setWorking(false);}
        }
        function display(items,append=false) {
          if(!append)list.replaceChildren();
          for(const item of items) {
            const button=document.createElement('button');button.type='button';button.textContent=item.label;
            try{const record=item.convert();button.onclick=()=>choose(record);}
            catch(e){button.dataset.unsupported='true';button.disabled=true;button.textContent+=' — '+e.message;}
            list.append(button);
          }
          setWorking(working);
        }
        function initial() {
          row.offset=0;more.hidden=true;display(starterChoices(app));
          message.textContent=app==='radio'?'Loading stations…':'Popular choices. Type to find more; selecting saves this side.';
        }
        async function search(next=false) {
          if(!alive()||working||!row.ready||!details.open)return;
          const version=++row.version,term=query.value.trim();
          if(!next)row.offset=0;
          const offset=next?row.offset+50:0;
          if(!term&&app!=='radio'){initial();return;}
          if(term.length<(app==='stock'?1:2)&&app!=='radio'){list.replaceChildren();more.hidden=true;message.textContent='Type at least two characters.';return;}
          message.textContent='Loading choices…';more.hidden=true;
          try {
            const result=await lookup(app,term,offset);
            if(!alive()||version!==row.version||!details.open)return;
            row.offset=offset;display(result.items,next);more.hidden=!result.more;
            message.textContent=result.items.length?'Select a choice to save this side.':'No matches. Try another name or country.';
          } catch(e) {
            if(alive()&&version===row.version) {
              message.textContent='Could not load choices. '+e.message+' Edit the filter or reopen to retry.';
              if(next)more.hidden=false;
            }
          }
        }
        summary.onclick=e=>{if(working||!row.ready||!connected())e.preventDefault();};
        details.addEventListener('toggle',()=>{
          if(!details.open){row.version++;clearTimeout(row.timer);return;}
          if(!alive()||!row.ready||working||!connected()){details.open=false;return;}
          rows.forEach(other=>{if(other!==row)other.details.open=false;});
          query.value='';initial();if(app==='radio')search();
          query.focus();
        });
        query.oninput=()=>{clearTimeout(row.timer);row.version++;more.hidden=true;list.replaceChildren();message.textContent='Loading choices…';row.timer=setTimeout(()=>search(),300);};
        query.onkeydown=e=>{if(e.key==='Enter'){e.preventDefault();clearTimeout(row.timer);search();}if(e.key==='Escape'){details.open=false;summary.focus();}};
        more.onclick=()=>search(true);
      });
      (async()=>{
        setWorking(true);
        try {
          for(let side=0;side<rows.length;side++) {
            const record=await link.read(app,side);if(current!==generation)return;
            const row=rows[side];row.record=record;row.ready=true;
            row.summary.textContent=record.preset?(app==='stock'?values[side]:catalogs[app]?.[values[side]])||'Choose…':text(record)||'Choose…';
          }
        } catch(e) {
          if(current===generation)for(const row of rows)if(!row.ready){row.summary.textContent='Reload required';row.message.textContent=e.message;}
        } finally{if(current===generation)setWorking(false);}
      })();
    },
  };
}
