// Public QA data only; never use these values for a real account.
import {mkdir,writeFile} from 'node:fs/promises';
import {encryptBackup} from '../web/backup.mjs';
import {normalizeRecord} from '../web/otp.mjs';
await mkdir(new URL('../.cache/',import.meta.url),{recursive:true});
const record=normalizeRecord({issuer:'Restored QA',account:'fixture@example.com',secret:'GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ'});
await writeFile(new URL('../.cache/qa.otpvault',import.meta.url),await encryptBackup([record],'Public test backup phrase'));
console.log('Public test-only encrypted fixture: .cache/qa.otpvault');
