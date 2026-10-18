import {validateRecords} from './otp.mjs';

export function requireBatchImport(status) {
  if (status?.batchImport !== true) throw new Error('Для одного подтверждения обновите прошивку StopWatch с сохранением аккаунтов по инструкции. Эта прошивка пока принимает аккаунты только по одному.');
}

// The device stages a merge with its existing vault. Only import_commit asks
// for physical consent and writes one atomic snapshot; never retry a write.
export async function importBatch(device, status, input, progress = () => {}) {
  requireBatchImport(status);
  const records = validateRecords(input);
  let started = false, committing = false;
  try {
    if (!records.length) throw new Error('Выберите хотя бы один аккаунт');
    started = true;
    await device.request('import_begin', {count: records.length});
    for (let index = 0; index < records.length; index++) {
      progress({phase: 'sending', index, total: records.length});
      await device.request('import_item', {index, record: records[index]});
    }
    progress({phase: 'confirming', total: records.length});
    committing = true;
    await device.request('import_commit');
  } catch (error) {
    if (started) { try { await device.request('import_abort'); } catch {} }
    throw new Error(committing ? `Сохранение набора не подтверждено. ${error.message} Проверьте список на StopWatch перед повторным импортом.` :
      `Импорт остановлен до сохранения набора. ${error.message}`);
  } finally { for (const record of records) record.secret = ''; }
}
