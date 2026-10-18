import {Device} from './serial.mjs';
import {normalizeRecord, parseOtpAuth} from './otp.mjs';
import {encryptBackup, decryptBackup} from './backup.mjs';

const $ = id => document.getElementById(id);
let state = null, records = [], busy = false, modalBusy = false;
let submitAction = null, modalCleanup = null;
let modalGeneration = 0;
const device = new Device(() => { state = null; records = []; render(); notice('USB отключён. Доступ к устройству закрыт.'); });
const modal = $('modal');
function notice(message, error = false) {
  $('notice').hidden = !message; $('notice').textContent = message; $('notice').classList.toggle('error', error);
}
function authorized() { return device.connected && state?.authorized && state.state === 'unlocked'; }
function render() {
  const connected = device.connected;
  $('connect').textContent = connected ? 'Отключить ↗' : 'Подключить для настройки ↗';
  $('connect').disabled = busy || !window.isSecureContext;
  const ready = authorized();
  $('connection-pill').textContent = ready ? 'ДОСТУП ОТКРЫТ' : connected ? 'ПОДКЛЮЧЁН' : 'НЕ ПОДКЛЮЧЁН';
  $('connection-pill').classList.toggle('ready', ready);
  $('connection-heading').textContent = ready ? 'StopWatch готов к настройке' : connected ? 'Разрешите доступ на устройстве' : 'Подключите прошитый StopWatch';
  $('connection-detail').textContent = ready ? `Прошивка ${state.firmware} · ${state.batteryMv} мВ · Подтверждайте действия на экране StopWatch.` :
    connected ? (state?.state === 'empty' ? 'Создайте PIN на экране StopWatch, затем разрешите USB в настройках.' :
      ['damaged','storage_error'].includes(state?.state) ? 'Хранилище недоступно. Выполните сброс на устройстве и восстановите копию.' :
      'Введите PIN на StopWatch. Откройте «Настройки» → «Разрешить USB».') :
    'После установки прошивки подключите устройство для настройки аккаунтов.';
  document.querySelectorAll('[data-auth]').forEach(b => { b.disabled = !ready || busy; });
  const count = ready ? records.length : 0;
  $('count').textContent = count;
  $('account-total').textContent = `/ ${String(count).padStart(2,'0')}`;
  $('time-dot').classList.toggle('valid', Boolean(state?.timeValid));
  $('time-status').textContent = !connected ? 'Состояние часов неизвестно' : state?.timeValid ? 'Время установлено · UTC' : 'Требуется синхронизация';
  renderRecords();
}
function node(tag, className, text) { const o=document.createElement(tag);o.className=className;if(text!==undefined)o.textContent=text;return o; }
function renderRecords() {
  const container=$('account-list');container.replaceChildren();
  if (!authorized() || !records.length) {
    const empty=node('div','empty');empty.append(node('div','empty-icon','＋'),node('h3','',authorized()?'Добавьте первый аккаунт':'Здесь будут ваши аккаунты'),
      node('p','',authorized()?'Нажмите «Добавить аккаунт». QR-картинка и секрет обрабатываются локально.':'Подключите устройство и разрешите USB-доступ в его настройках после ввода PIN.'),
      node('span','empty-footnote','Секреты аккаунтов скрыты в списке'));container.append(empty);return;
  }
  const q=$('search').value.toLocaleLowerCase('ru');
  const filtered=records.filter(r => `${r.issuer} ${r.account}`.toLocaleLowerCase('ru').includes(q));
  if(!filtered.length) container.append(node('div','empty','По вашему запросу ничего не найдено'));
  for(const r of filtered) {
    const row=node('div','account-row');row.append(node('span','account-avatar',(r.issuer || r.account).slice(0,1).toUpperCase()));
    const name=node('div','account-name');name.append(node('strong','',r.issuer || 'Аккаунт'),node('small','',r.account));
    row.append(name,node('span','account-meta',`${r.algorithm} · ${r.digits} цифр · ${r.period} с`));
    for(const [text,action] of [['Изменить',()=>rename(r)],['Удалить',()=>remove(r)]]) {
      const b=node('button','row-action',text);b.disabled=busy;b.addEventListener('click',action);row.append(b);
    }
    container.append(row);
  }
}
async function loadRecords() {
  const result=[];
  for(let offset=0;offset<100;offset+=5) {
    const page=await device.request('list',{offset});
    if(!Array.isArray(page.records) || !Number.isInteger(page.total) || page.total<0 || page.total>100 || page.records.length>5) throw new Error('Некорректный список аккаунтов');
    result.push(...page.records);
    if(result.length>=page.total) {records=result;return;}
    if(!page.records.length) throw new Error('Список аккаунтов получен не полностью');
  }
  records=result;
}
async function refresh(load=false) {
  if(!device.connected) return;
  const was=authorized();state=await device.request('status');
  if(authorized() && (load || !was)) await loadRecords();
  if(!authorized()) records=[];
  render();
}
async function operate(fn, success='Готово.') {
  if(busy) return;
  busy=true;render();notice('Проверьте экран StopWatch и подтвердите действие.');
  try {await fn();notice(success);await refresh(true);}
  catch(error){notice(error.message,true);try{await refresh();}catch{}}
  finally{busy=false;render();}
}
function cleanupModal() {
  modalGeneration++;
  for(const input of modal.querySelectorAll('input,textarea')) input.value='';
  modalCleanup?.();modalCleanup=null;submitAction=null;$('modal-body').replaceChildren();
}
function closeModal() {if(modalBusy)return;modal.close();cleanupModal();}
function openModal(title,body,submit,button='Продолжить',cleanup=null) {
  cleanupModal();$('modal-title').textContent=title;$('modal-body').innerHTML=body;
  $('modal-error').textContent='';$('modal-submit').textContent=button;submitAction=submit;modalCleanup=cleanup;modal.showModal();
}
$('modal-form').addEventListener('submit',async e=>{
  e.preventDefault();if(modalBusy || !submitAction)return;
  modalBusy=true;const submit=submitAction;
  $('modal-submit').disabled=$('modal-cancel').disabled=$('modal-close').disabled=true;$('modal-error').textContent='';
  try{const result=await submit();if(result!==false){modal.close();cleanupModal();}}
  catch(error){$('modal-error').textContent=error.message;}
  finally{modalBusy=false;$('modal-submit').disabled=$('modal-cancel').disabled=$('modal-close').disabled=false;}
});
$('modal-close').addEventListener('click',closeModal);$('modal-cancel').addEventListener('click',closeModal);
modal.addEventListener('cancel',e=>{e.preventDefault();closeModal();});
$('search').addEventListener('input',renderRecords);
$('connect').addEventListener('click',async()=>{
  if(busy)return;busy=true;render();
  try{
    if(device.connected) await device.disconnect();
    else{state=await device.connect();if(authorized())await loadRecords();notice(authorized()?'Устройство подключено. Можно управлять аккаунтами.':'Теперь введите PIN на StopWatch и разрешите USB в настройках.');}
  }catch(error){notice(error.name==='NotFoundError'?'USB-порт не выбран.':error.message,true);}
  finally{busy=false;render();}
});
$('sync').addEventListener('click',()=>operate(()=>device.request('time_set',{epoch:Math.floor(Date.now()/1000)}),'Время передано устройству.'));
$('lock').addEventListener('click',()=>operate(()=>device.request('lock'),'StopWatch заблокирован.'));
const fields=`<label class="field">Сервис<input id="issuer" maxlength="96" placeholder="Например, GitHub" autocomplete="off"></label>
<label class="field">Аккаунт<input id="account" maxlength="128" placeholder="name@example.com" autocomplete="off"></label>`;
function add() {
  openModal('Добавить аккаунт',`<label class="qr-upload">QR-картинка с настройками 2FA<input id="qr-file" type="file" accept="image/png,image/jpeg,image/webp"></label>
    <label class="field">Ссылка otpauth://<textarea id="uri" placeholder="otpauth://totp/…" spellcheck="false"></textarea></label>
    <p class="divider">ИЛИ ВВЕДИТЕ СЕКРЕТ ВРУЧНУЮ</p>${fields}
    <label class="field">Секрет Base32<input id="secret" type="password" autocomplete="new-password" spellcheck="false" placeholder="JBSWY3DPEHPK3PXP"></label>
    <div class="field-row"><label class="field">Алгоритм<select id="algorithm"><option>SHA1</option><option>SHA256</option><option>SHA512</option></select></label><label class="field">Цифр<select id="digits"><option>6</option><option>8</option></select></label><label class="field">Период, с<select id="period"><option>30</option><option>60</option></select></label></div>
    <p class="modal-help">Подтвердите сервис и аккаунт на экране StopWatch. QR-коды переноса Google Authenticator не поддерживаются.</p>`,async()=>{
      const uri=$('uri').value.trim();let record;
      try{
        if(uri)parseOtpAuth(uri);
        record=normalizeRecord({issuer:$('issuer').value,account:$('account').value,secret:$('secret').value,
          algorithm:$('algorithm').value,digits:Number($('digits').value),period:Number($('period').value)});
        let existing=records.find(r=>r.issuer===record.issuer && r.account===record.account);
        if(existing) throw new Error('Аккаунт с таким сервисом и именем уже существует. Для замены сначала удалите прежнюю запись.');
        $('modal-error').textContent='Подтвердите добавление на StopWatch…';
        busy=true;render();await device.request('upsert',{record});await refresh(true);notice('Аккаунт добавлен. Сохраните новую резервную копию.');
      }finally{if(record)record.secret='';busy=false;render();}
    },'Добавить');
  function fillParsed(parsed){
    $('issuer').value=parsed.issuer;$('account').value=parsed.account;$('secret').value=parsed.secret;
    $('algorithm').value=parsed.algorithm;$('digits').value=parsed.digits;$('period').value=parsed.period;
    parsed.secret='';
  }
  $('uri').addEventListener('input',()=>{
    const uri=$('uri').value.trim();if(!uri)return;
    try{fillParsed(parseOtpAuth(uri));}catch{/* Explain incomplete/invalid URLs when submitting. */}
  });
  for(const id of ['issuer','account','secret','algorithm','digits','period']){
    $(id).addEventListener('input',()=>{$('uri').value='';});
  }
  $('qr-file').addEventListener('change',async e=>{
    const generation=modalGeneration;
    const file=e.target.files[0];if(!file)return;
    try{
      if(file.size>10*1024*1024)throw new Error('Картинка должна быть меньше 10 МБ');
      const image=await createImageBitmap(file);
      try{
        if(image.width*image.height>16000000)throw new Error('Картинка слишком большая');
        const scale=Math.min(1,1600/Math.max(image.width,image.height));const canvas=document.createElement('canvas');
        canvas.width=Math.round(image.width*scale);canvas.height=Math.round(image.height*scale);
        const context=canvas.getContext('2d',{willReadFrequently:true});context.drawImage(image,0,0,canvas.width,canvas.height);
        const pixels=context.getImageData(0,0,canvas.width,canvas.height);
        const decoded=window.jsQR(pixels.data,pixels.width,pixels.height,{inversionAttempts:'attemptBoth'});
        pixels.data.fill(0);context.clearRect(0,0,canvas.width,canvas.height);canvas.width=canvas.height=0;
        if(!decoded)throw new Error('QR-код не найден. Выберите более чёткую картинку.');
        if(generation!==modalGeneration || !modal.open)return;
        const parsed=parseOtpAuth(decoded.data);$('uri').value=decoded.data;fillParsed(parsed);
        $('modal-error').textContent='QR-код прочитан. Проверьте название сервиса и аккаунта.';
      }finally{image.close();}
    }catch(error){$('modal-error').textContent=error.message;}
    finally{e.target.value='';}
  });
}
$('add').addEventListener('click',add);
function rename(r) {
  openModal('Изменить название',fields,async()=>{
    const issuer=$('issuer').value,account=$('account').value;
    // Validate UTF-8 byte limits with the same schema, using a public test secret.
    normalizeRecord({...r,issuer,account,secret:'GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ'});
    busy=true;render();$('modal-error').textContent='Подтвердите переименование на StopWatch…';
    try{await device.request('rename',{id:r.id,issuer,account});await refresh(true);notice('Название изменено.');}
    finally{busy=false;render();}
  },'Сохранить');$('issuer').value=r.issuer;$('account').value=r.account;
}
function remove(r) {
  openModal('Удалить аккаунт','<p class="modal-help" id="delete-name"></p><p class="modal-help">Потребуется подтверждение на StopWatch.</p>',async()=>{
    busy=true;render();$('modal-error').textContent='Подтвердите удаление на StopWatch…';
    try{await device.request('delete',{id:r.id});await refresh(true);notice('Аккаунт удалён. Обновите резервную копию.');}
    finally{busy=false;render();}
  },'Удалить');$('delete-name').textContent=`${r.issuer} · ${r.account}`;
}
const passwordFields=`<label class="field">Парольная фраза<input id="backup-password" type="password" minlength="16" required autocomplete="new-password"></label>`;
$('backup-save').addEventListener('click',()=>{
  openModal('Создать резервную копию',`<p class="modal-help">Отдельная парольная фраза от 16 символов. Без неё восстановить файл нельзя. Сохраните её отдельно от копии.</p>${passwordFields}<label class="field">Повторите фразу<input id="backup-repeat" type="password" required autocomplete="new-password"></label>`,async()=>{
    if($('backup-password').value!==$('backup-repeat').value)throw new Error('Парольные фразы не совпадают');
    if($('backup-password').value.length<16)throw new Error('Используйте не менее 16 символов');
    const exported=[];busy=true;render();
    try{
      $('modal-error').textContent='Разрешите передачу секретов на экране StopWatch…';
      const {count}=await device.request('export_begin');
      if(!Number.isInteger(count)||count<0||count>100)throw new Error('Некорректное число аккаунтов');
      for(let i=0;i<count;i++)exported.push((await device.request('export_next')).record);
      $('modal-error').textContent='Шифрование копии…';
      const encrypted=await encryptBackup(exported,$('backup-password').value);
      const blob=new Blob([encrypted],{type:'application/octet-stream'});const url=URL.createObjectURL(blob);
      const a=document.createElement('a');a.href=url;a.download=`stopwatch-${new Date().toISOString().slice(0,10)}.otpvault`;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
      notice('Зашифрованная копия сохранена. Проверьте файл в папке загрузок.');
    }finally{for(const r of exported)if(r)r.secret='';exported.length=0;busy=false;render();}
  },'Создать копию');
});
$('backup-restore').addEventListener('click',()=>{
  let decrypted=null;
  openModal('Восстановить хранилище',`<p class="modal-help">Восстановление полностью заменит текущие аккаунты. Сначала файл будет проверен без изменения устройства.</p><label class="field">Файл .otpvault<input id="restore-file" type="file" accept=".otpvault" required></label>${passwordFields}<p class="modal-help" id="restore-preview"></p>`,async()=>{
    if(!decrypted){
      const file=$('restore-file').files[0];if(!file)throw new Error('Выберите файл копии');
      if(file.size>256*1024)throw new Error('Файл слишком большой');
      decrypted=await decryptBackup(await file.text(),$('backup-password').value);
      $('restore-preview').textContent=`Копия проверена. Аккаунтов: ${decrypted.length}. Нажмите «Заменить аккаунты», затем подтвердите на StopWatch.`;
      $('modal-submit').textContent='Заменить аккаунты';$('backup-password').value='';$('backup-password').required=false;
      $('restore-file').disabled=true;
      // A second explicit submit is required after validation.
      return false;
    }
    busy=true;render();
    try{
      await device.request('restore_begin',{count:decrypted.length});
      for(let i=0;i<decrypted.length;i++)await device.request('restore_item',{index:i,record:decrypted[i]});
      $('modal-error').textContent='Подтвердите полную замену аккаунтов на StopWatch…';
      await device.request('restore_commit');await refresh(true);notice('Хранилище восстановлено. Проверьте время устройства.');
    }catch(error){try{await device.request('restore_abort');}catch{}throw error;}
    finally{busy=false;render();}
  },'Проверить копию',()=>{if(decrypted)for(const r of decrypted)r.secret='';decrypted=null;});
});
setInterval(async()=>{
  if(!device.connected||busy||modalBusy||device.pending.size)return;
  try{await refresh();}catch(error){notice(error.message,true);}
},1500);
window.addEventListener('pagehide',()=>{device.disconnect();cleanupModal();records=[];});
document.querySelector('.topbar-local').lastChild.textContent = ` ${location.hostname}`;
if(!window.isSecureContext){
  notice('Первая прошивка доступна по кнопке «Прошить в ESPHome Web» выше. Для следующего шага — настройки аккаунтов по USB — откройте этот сайт по HTTPS с доверенным сертификатом или запустите его на компьютере с браузером через http://127.0.0.1:8765/. На Mac: python3 tools/server.py из папки проекта; на Windows: start-web.cmd.');
}else if(!('serial' in navigator))notice('Для доступа по USB откройте этот адрес в Chrome или Edge на компьютере.',true);
render();
