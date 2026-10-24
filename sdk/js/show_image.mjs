// Send a picture to a device screen: scale to fill, crop the middle, RGB565, 16-row blocks, paced.
// Same algorithm as sdk/python/dc_device/screen_image.py. One file, no dependencies.
//
// The device takes at most 7680 pixel bytes per `screen.write_block` (240 x 16 x 2), so a picture
// is 20 blocks; blocks are sent one at a time, at least 100 ms apart (DC lets 10 actions a second
// through). The caller supplies `act` and decodes the JPEG into pixels (see showImage).

export const SCREEN_WIDTH = 240;
export const SCREEN_HEIGHT = 320;
export const BLOCK_ROWS = 16;

// image = { width, height, data }: data is RGBA (canvas getImageData) or RGB, 8 bits a channel.
function channels(image) {
  const { width, height, data } = image ?? {};
  if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1 || !data) {
    throw new TypeError('image must be { width, height, data }');
  }
  if (data.length === width * height * 4) return 4;
  if (data.length === width * height * 3) return 3;
  throw new TypeError('image.data must be RGBA or RGB, width * height pixels');
}

// Scale to cover width x height (area average, no rotation), crop the middle, pack big-endian
// RGB565 (high byte first). Returns a Uint8Array of width * height * 2 bytes.
export function imageToRgb565(image, width = SCREEN_WIDTH, height = SCREEN_HEIGHT) {
  const ch = channels(image);
  const { width: sw, height: sh, data } = image;
  const scale = Math.max(width / sw, height / sh);
  const scaledW = Math.max(width, Math.round(sw * scale));
  const scaledH = Math.max(height, Math.round(sh * scale));
  const left = Math.floor((scaledW - width) / 2);
  const top = Math.floor((scaledH - height) / 2);
  const out = new Uint8Array(width * height * 2);
  const rx = sw / scaledW, ry = sh / scaledH;  // source pixels per scaled pixel
  let o = 0;
  for (let y = 0; y < height; y++) {
    const y0 = (top + y) * ry, y1 = y0 + ry;
    for (let x = 0; x < width; x++) {
      const x0 = (left + x) * rx, x1 = x0 + rx;
      let r = 0, g = 0, b = 0, w = 0;
      for (let sy = Math.floor(y0); sy < Math.min(sh, Math.ceil(y1)); sy++) {
        const wy = Math.min(sy + 1, y1) - Math.max(sy, y0);
        for (let sx = Math.floor(x0); sx < Math.min(sw, Math.ceil(x1)); sx++) {
          const wt = wy * (Math.min(sx + 1, x1) - Math.max(sx, x0));
          const i = (sy * sw + sx) * ch;
          r += data[i] * wt; g += data[i + 1] * wt; b += data[i + 2] * wt; w += wt;
        }
      }
      const R = Math.round(r / w), G = Math.round(g / w), B = Math.round(b / w);
      const v = ((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3);
      out[o++] = v >> 8; out[o++] = v & 0xFF;
    }
  }
  return out;
}

function toBase64(bytes) {
  if (typeof Buffer !== 'undefined') return Buffer.from(bytes.buffer, bytes.byteOffset, bytes.length).toString('base64');
  let s = '';
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(s);
}

function sleep(ms, signal) {
  return new Promise((resolve) => {
    if (signal?.aborted) return resolve();
    const t = setTimeout(done, ms);
    function done() { signal?.removeEventListener('abort', done); clearTimeout(t); resolve(); }
    signal?.addEventListener('abort', done, { once: true });
  });
}

// A refusal as { error, code, message }: from the reply, or from a thrown error that carries one.
function refusal(reply) {
  if (reply && reply.ok !== false) return null;
  const r = reply ?? {};
  return { error: r.error ?? 'DeviceError', code: r.code ?? '', message: r.message ?? '' };
}

// Shows a picture on the screen.
//   act(capability, args) -> Promise<reply>   reply = { ok, error?, code?, message? }; a thrown
//                                             error with .error/.code/.body is read the same way
//   image  { width, height, data }            decoded pixels (RGBA or RGB), e.g. from a canvas
//   jpeg + decode                             alternatively encoded bytes and decode(jpeg) -> image
//   signal                                    AbortSignal; stops before the next block
//   onProgress({ sent, total })               after each block
//   pause (ms, default 100), retries (RateLimited retries per block, default 5)
// Resolves { ok, blocks, sent } or { ok:false, blocks, sent, failedAt, code, error, message },
// or { ok:false, cancelled:true, blocks, sent }. A refused block stops everything; it is never
// resent except for RateLimited.
export async function showImage({ act, image, jpeg, decode, signal, onProgress, pause = 100, retries = 5 }) {
  if (typeof act !== 'function') throw new TypeError('act is required');
  if (!image) {
    if (!jpeg) throw new TypeError('pass image (decoded pixels) or jpeg with decode');
    if (typeof decode !== 'function') throw new TypeError('a jpeg needs decode(jpeg) -> { width, height, data }; Node cannot decode JPEG itself');
    image = await decode(jpeg);
  }
  const pixels = imageToRgb565(image);
  const rowBytes = SCREEN_WIDTH * 2;
  const total = SCREEN_HEIGHT / BLOCK_ROWS;
  let sent = 0;
  for (let i = 0; i < total; i++) {
    if (signal?.aborted) return { ok: false, cancelled: true, blocks: total, sent };
    if (i) {
      await sleep(pause, signal);
      if (signal?.aborted) return { ok: false, cancelled: true, blocks: total, sent };
    }
    const chunk = pixels.subarray(i * BLOCK_ROWS * rowBytes, (i + 1) * BLOCK_ROWS * rowBytes);
    const args = { x: 0, y: i * BLOCK_ROWS, width: SCREEN_WIDTH, height: BLOCK_ROWS, data_base64: toBase64(chunk) };
    for (let attempt = 0; ; attempt++) {
      let fail;
      try {
        fail = refusal(await act('screen.write_block', args));
      } catch (e) {
        let body = e?.body;
        if (typeof body === 'string') { try { body = JSON.parse(body); } catch { body = undefined; } }
        fail = { error: body?.error ?? e?.error ?? 'ActError', code: body?.code ?? e?.code ?? '', message: body?.message ?? e?.message ?? String(e) };
      }
      if (!fail) break;
      if (fail.error !== 'RateLimited' || attempt >= retries) {
        return { ok: false, blocks: total, sent, failedAt: i, ...fail };
      }
      await sleep(pause, signal);
      if (signal?.aborted) return { ok: false, cancelled: true, blocks: total, sent };
    }
    sent++;
    onProgress?.({ sent, total });
  }
  return { ok: true, blocks: total, sent };
}
