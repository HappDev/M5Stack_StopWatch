export const MAX_FRAME = 4096;
const encoder = new TextEncoder();
export class FrameReader {
  constructor(onFrame, onError = () => {}) { this.onFrame = onFrame; this.onError = onError; this.bytes = []; this.dropping = false; }
  push(chunk) {
    for (const byte of chunk) {
      if (byte === 10) {
        if (!this.dropping && this.bytes.length) {
          try { this.onFrame(JSON.parse(new TextDecoder('utf-8', {fatal:true}).decode(new Uint8Array(this.bytes)))); }
          catch { this.onError(new Error('Повреждён ответ устройства')); }
        }
        this.bytes.fill(0); this.bytes = []; this.dropping = false;
      } else if (!this.dropping) {
        if (this.bytes.length >= MAX_FRAME - 1) { this.bytes.fill(0); this.bytes = []; this.dropping = true; this.onError(new Error('Превышен размер сообщения')); }
        else this.bytes.push(byte);
      }
    }
  }
  clear() { this.bytes.fill(0); this.bytes = []; this.dropping = false; }
}
export class Device {
  constructor(onDisconnect = () => {}) { this.onDisconnect = onDisconnect; this.pending = new Map(); this.nextId = 1; this.port = null; this.reader = null; this.queue = Promise.resolve(); }
  get connected() { return this.port !== null; }
  async connect() {
    if (!navigator.serial) throw new Error('Откройте сайт в Chrome или Edge на компьютере');
    const port = await navigator.serial.requestPort();
    await port.open({baudRate: 115200, bufferSize: 8192});
    this.port = port;
    await port.setSignals({dataTerminalReady: true, requestToSend: false});
    this.readTask = this.readLoop(port);
    try { return await this.request('status'); } catch (error) { await this.disconnect(); throw error; }
  }
  async readLoop(port) {
    const frames = new FrameReader(frame => {
      const p = this.pending.get(frame.id);
      if (!p) return;
      this.pending.delete(frame.id); clearTimeout(p.timer);
      if (frame.v !== 1) p.reject(new Error('Несовместимая версия протокола'));
      else if (frame.ok) p.resolve(frame.result);
      else p.reject(new Error(frame.error?.message || 'Ошибка устройства'));
    }, error => this.rejectAll(error));
    try {
      this.reader = port.readable.getReader();
      while (true) {
        const {value, done} = await this.reader.read(); if (done) break;
        frames.push(value); value.fill(0);
      }
    } catch { /* Cable removal and explicit cancellation share cleanup. */ }
    finally {
      frames.clear(); this.reader?.releaseLock(); this.reader = null;
      if (this.port === port) { this.port = null; this.rejectAll(new Error('USB отключён')); this.onDisconnect(); }
    }
  }
  rejectAll(error) { for (const p of this.pending.values()) { clearTimeout(p.timer); p.reject(error); } this.pending.clear(); }
  request(op, args = {}) {
    const expectedPort = this.port;
    const job = this.queue.then(() => {
      if (this.port !== expectedPort) throw new Error('USB-соединение изменилось');
      return this.sendRequest(op, args);
    });
    this.queue = job.catch(() => {});
    return job;
  }
  async sendRequest(op, args) {
    if (!this.port?.writable) throw new Error('Подключите StopWatch');
    if (this.pending.size) throw new Error('Дождитесь завершения текущей операции');
    const id = this.nextId++;
    const bytes = encoder.encode(JSON.stringify({v:1,id,op,args}) + '\n');
    if (bytes.length > MAX_FRAME) throw new Error('Слишком большое сообщение');
    const response = new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.pending.delete(id); reject(new Error('Время ожидания истекло. Проверьте экран StopWatch.')); }, 45000);
      this.pending.set(id, {resolve, reject, timer});
    });
    try {
      const writer = this.port.writable.getWriter();
      try { await writer.write(bytes); } finally { writer.releaseLock(); bytes.fill(0); }
    } catch (error) { this.rejectAll(error); }
    return response;
  }
  async disconnect() {
    const port = this.port; if (!port) return;
    this.port = null; this.rejectAll(new Error('Соединение закрыто'));
    try { await port.setSignals({dataTerminalReady: false}); } catch {}
    try { await this.reader?.cancel(); await this.readTask; } catch {}
    try { await port.close(); } catch {}
    this.onDisconnect();
  }
}
