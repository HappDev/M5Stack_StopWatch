function decodeImage(source, width, height) {
  const scale = Math.min(1, 1600 / Math.max(width, height));
  const canvas = document.createElement('canvas');
  canvas.width = Math.max(1, Math.round(width * scale)); canvas.height = Math.max(1, Math.round(height * scale));
  const context = canvas.getContext('2d', {willReadFrequently: true});
  let pixels;
  try {
    context.drawImage(source, 0, 0, canvas.width, canvas.height);
    pixels = context.getImageData(0, 0, canvas.width, canvas.height);
    return window.jsQR(pixels.data, pixels.width, pixels.height, {inversionAttempts: 'attemptBoth'})?.data ?? null;
  } finally {
    pixels?.data.fill(0); context.clearRect(0, 0, canvas.width, canvas.height); canvas.width = canvas.height = 0;
  }
}

export async function readQrImage(file) {
  if (file.size > 10 * 1024 * 1024) throw new Error('Картинка должна быть меньше 10 МБ');
  const image = await createImageBitmap(file);
  try {
    if (image.width * image.height > 16000000) throw new Error('Картинка слишком большая');
    const value = decodeImage(image, image.width, image.height);
    if (!value) throw new Error('QR-код не найден. Выберите более чёткую картинку.');
    return value;
  } finally { image.close(); }
}

// Returns a stop function immediately, including while browser permission is pending.
export function scanQrCamera(video, onCode, onError) {
  let stopped = false, stream = null, timer = null, previous = null;
  function stop() {
    stopped = true; clearTimeout(timer); stream?.getTracks().forEach(track => track.stop());
    video.pause(); video.srcObject = null; previous = null;
  }
  function frame() {
    if (stopped) return;
    try {
      if (video.readyState >= 2 && video.videoWidth && video.videoHeight) {
        const text = decodeImage(video, video.videoWidth, video.videoHeight);
        if (text && text !== previous) { previous = text; onCode(text); }
        if (!text) previous = null;
      }
      if (!stopped) timer = setTimeout(frame, 250);
    } catch (error) { stop(); onError(error); }
  }
  (async () => {
    try {
      if (!navigator.mediaDevices?.getUserMedia) throw new Error('Камера недоступна. Откройте сайт на localhost в Chrome или Edge либо загрузите фото QR.');
      stream = await navigator.mediaDevices.getUserMedia({audio: false, video: {width: {ideal: 1280}, height: {ideal: 720}}});
      if (stopped) { stream.getTracks().forEach(track => track.stop()); return; }
      video.srcObject = stream; await video.play(); frame();
    } catch (error) {
      if (stopped) return;
      stop(); onError(new Error(error.name === 'NotAllowedError' ? 'Разрешите камеру браузеру или загрузите фото QR.' : error.message));
    }
  })();
  return stop;
}
