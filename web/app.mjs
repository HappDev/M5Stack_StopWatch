import {Device} from './serial.mjs';
import {normalizeRecord, parseOtpAuth} from './otp.mjs';
import {encryptBackup, decryptBackup} from './backup.mjs';
import {MigrationBatch, recordKey, selectMigrationRecords} from './migration.mjs';
import {readQrImage, scanQrCamera} from './qr.mjs';
import {importBatch, requireBatchImport} from './import.mjs';

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
    connected ? (state?.state === 'empty' ? 'Создайте графический ключ на экране StopWatch, затем разрешите USB в настройках.' :
      ['damaged','storage_error'].includes(state?.state) ? 'Хранилище недоступно. Выполните сброс на устройстве и восстановите копию.' :
      'Разблокируйте StopWatch и завершите настройку графического ключа. Откройте «Настройки» → «Разрешить USB».') :
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
      node('p','',authorized()?'Нажмите «Добавить аккаунт». QR-картинка и секрет обрабатываются локально.':'Подключите устройство и разрешите USB-доступ в его настройках после разблокировки.'),
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
    else{state=await device.connect();if(authorized())await loadRecords();notice(authorized()?'Устройство подключено. Можно управлять аккаунтами.':'Теперь разблокируйте StopWatch и разрешите USB в настройках.');}
  }catch(error){notice(error.name==='NotFoundError'?'USB-порт не выбран.':error.message,true);}
  finally{busy=false;render();}
});
$('sync').addEventListener('click',()=>operate(()=>device.request('time_set',{epoch:Math.floor(Date.now()/1000),utcOffsetMinutes:-new Date().getTimezoneOffset()}),'Время и часовой пояс переданы устройству.'));
$('lock').addEventListener('click',()=>operate(()=>device.request('lock'),'StopWatch заблокирован.'));
const fields=`<label class="field">Сервис<input id="issuer" maxlength="96" placeholder="Например, GitHub" autocomplete="off"></label>
<label class="field">Аккаунт<input id="account" maxlength="128" placeholder="name@example.com" autocomplete="off"></label>`;
function add() {
  openModal('Добавить аккаунт',`<label class="qr-upload">QR-картинка с настройками 2FA<input id="qr-file" type="file" accept="image/png,image/jpeg,image/webp"></label>
    <label class="field">Ссылка otpauth://<textarea id="uri" placeholder="otpauth://totp/…" spellcheck="false"></textarea></label>
    <p class="divider">ИЛИ ВВЕДИТЕ СЕКРЕТ ВРУЧНУЮ</p>${fields}
    <label class="field">Секрет Base32<input id="secret" type="password" autocomplete="new-password" spellcheck="false" placeholder="JBSWY3DPEHPK3PXP"></label>
    <div class="field-row"><label class="field">Алгоритм<select id="algorithm"><option>SHA1</option><option>SHA256</option><option>SHA512</option></select></label><label class="field">Цифр<select id="digits"><option>6</option><option>8</option></select></label><label class="field">Период, с<select id="period"><option>30</option><option>60</option></select></label></div>
    <p class="modal-help">Подтвердите сервис и аккаунт на экране StopWatch. Для экспорта Google используйте кнопку «Из Google Authenticator» в списке аккаунтов.</p>`,async()=>{
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
      const decoded=await readQrImage(file);
      if(generation!==modalGeneration || !modal.open || modalBusy)return;
      if(decoded.startsWith('otpauth-migration:'))throw new Error('Закройте это окно и нажмите «Из Google Authenticator» для переноса одного или всех аккаунтов.');
      const parsed=parseOtpAuth(decoded);$('uri').value=decoded;fillParsed(parsed);
      $('modal-error').textContent='QR-код прочитан. Проверьте название сервиса и аккаунта.';
    }catch(error){if(generation===modalGeneration && modal.open)$('modal-error').textContent=error.message;}
    finally{e.target.value='';}
  });
}
$('add').addEventListener('click',add);

$('google-import').addEventListener('click',()=>{
  const batch=new MigrationBatch(), selected=new Set();
  let stopCamera=null, reading=false;
  function stopScan(){
    stopCamera?.();stopCamera=null;
    if($('migration-video'))$('migration-video').hidden=true;
    if($('migration-camera'))$('migration-camera').textContent='Сканировать камерой';
  }
  openModal('Из Google Authenticator',`
    <p class="modal-help">На телефоне: меню → Перенос аккаунтов → Экспорт аккаунтов. Выберите один или все аккаунты и покажите каждый QR-код камере компьютера.</p>
    <fieldset id="migration-source" class="migration-fieldset">
      <button type="button" class="secondary" id="migration-camera">Сканировать камерой</button>
      <video id="migration-video" class="migration-video" muted playsinline hidden></video>
      <label class="field">Или загрузите фото / картинки QR<input id="migration-files" type="file" accept="image/png,image/jpeg,image/webp" multiple></label>
      <details><summary>Вставить уже считанную ссылку переноса</summary><label class="field">Ссылка otpauth-migration://<input id="migration-uri" type="password" autocomplete="new-password" spellcheck="false"></label><button type="button" class="secondary" id="migration-read">Прочитать ссылку</button></details>
    </fieldset>
    <p class="modal-help" id="migration-status" role="status">QR-коды ещё не прочитаны.</p>
    <fieldset id="migration-selection" class="migration-fieldset" hidden>
      <div class="migration-actions"><button type="button" class="text-button" id="migration-all">Выбрать все</button><button type="button" class="text-button" id="migration-none">Снять выбор</button><button type="button" class="text-button" id="migration-clear">Начать заново</button></div>
      <div id="migration-records" class="migration-records"></div>
    </fieldset>
    <p class="modal-help">QR обрабатывается только в этом браузере. На StopWatch потребуется одно подтверждение для всей выбранной группы. После переноса сравните коды с телефоном.</p>`,async()=>{
      if(reading)throw new Error('Дождитесь чтения картинки');
      stopScan();
      busy=true;render();$('migration-source').disabled=$('migration-selection').disabled=true;
      let started=false, saved=false, total=0;
      try{
        await refresh(true);
        if(!authorized())throw new Error('Разблокируйте StopWatch и разрешите USB в его настройках');
        requireBatchImport(state);
        const chosen=selectMigrationRecords(batch,selected,records);total=chosen.length;
        started=true;
        await importBatch(device,state,chosen,progress=>{
          $('modal-error').textContent=progress.phase==='sending' ? `Передача аккаунтов: ${progress.index+1} из ${total}…` :
            `Подтвердите импорт ${total} аккаунтов на StopWatch один раз…`;
        });
        saved=true;
        await refresh(true);
        notice(`Перенесено аккаунтов: ${total}. Сравните коды с Google Authenticator и сохраните резервную копию.`);
      }catch(error){
        if(!started)throw error;
        try{await refresh(true);}catch{}
        notice(saved ? `Сохранено аккаунтов: ${total}, но не удалось обновить список. ${error.message}` : error.message,true);
        // A lost commit reply may mean the whole batch succeeded. Never retry automatically.
      }finally{
        busy=false;render();$('migration-source').disabled=$('migration-selection').disabled=false;
      }
    },'Перенести выбранные',()=>{stopScan();batch.clear();selected.clear();});
  const generation=modalGeneration;
  const active=()=>generation===modalGeneration && modal.open;
  function preview(){
    const list=$('migration-records');list.replaceChildren();
    const seen=new Set(records.map(recordKey));
    for(const record of batch.records){
      const duplicate=seen.has(recordKey(record));seen.add(recordKey(record));
      if(duplicate)selected.delete(record.id);
      const label=node('label','migration-record'),checkbox=document.createElement('input');
      checkbox.type='checkbox';checkbox.checked=selected.has(record.id);checkbox.disabled=duplicate;
      checkbox.addEventListener('change',()=>{if(checkbox.checked)selected.add(record.id);else selected.delete(record.id);preview();});
      const title=node('span','');title.append(node('strong','',record.issuer || 'Аккаунт'),node('small','',record.account));
      if(duplicate)title.append(node('small','','Уже есть в списке или на устройстве — пропущен'));
      label.append(checkbox,title);list.append(label);
    }
    $('migration-selection').hidden=!batch.records.length;
    const expected=batch.metadata?.batchSize ?? 0;
    $('migration-status').textContent=expected ? `Прочитано QR: ${batch.parts.size} из ${expected}. Аккаунтов: ${batch.records.length}. Выбрано: ${selected.size}.${batch.complete?'':' Прочитайте оставшиеся QR этого экспорта.'}` : 'QR-коды ещё не прочитаны.';
  }
  function accept(uri){
    if(!active() || modalBusy)return;
    const before=new Set(batch.records.map(r=>r.id));
    const added=batch.add(uri);
    for(const r of batch.records)if(!before.has(r.id))selected.add(r.id);
    preview();$('modal-error').textContent=added?'QR прочитан. Проверьте аккаунты в списке.':'Этот QR уже прочитан.';
    if(batch.complete)stopScan();
  }
  $('migration-read').addEventListener('click',()=>{
    if(reading || modalBusy)return;
    try{accept($('migration-uri').value);}catch(error){$('modal-error').textContent=error.message;}
    finally{$('migration-uri').value='';}
  });
  $('migration-uri').addEventListener('keydown',e=>{if(e.key==='Enter'){e.preventDefault();$('migration-read').click();}});
  $('migration-files').addEventListener('change',async e=>{
    if(reading || modalBusy)return;
    const files=[...e.target.files];stopScan();reading=true;
    $('migration-source').disabled=$('migration-selection').disabled=$('modal-submit').disabled=true;
    try{
      for(const file of files){
        const uri=await readQrImage(file);
        if(!active())return;
        accept(uri);
      }
    }catch(error){if(active())$('modal-error').textContent=error.message;}
    finally{reading=false;e.target.value='';if(active())$('migration-source').disabled=$('migration-selection').disabled=$('modal-submit').disabled=false;}
  });
  $('migration-camera').addEventListener('click',()=>{
    if(stopCamera){stopScan();return;}
    const video=$('migration-video');video.hidden=false;$('migration-camera').textContent='Остановить камеру';
    stopCamera=scanQrCamera(video,uri=>{
      if(!active() || reading || modalBusy)return;
      try{accept(uri);}catch(error){$('modal-error').textContent=error.message;}
    },error=>{if(active()){stopScan();$('modal-error').textContent=error.message;}});
  });
  $('migration-all').addEventListener('click',()=>{for(const r of batch.records)selected.add(r.id);preview();});
  $('migration-none').addEventListener('click',()=>{selected.clear();preview();});
  $('migration-clear').addEventListener('click',()=>{stopScan();batch.clear();selected.clear();preview();$('modal-error').textContent='';});
});

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
