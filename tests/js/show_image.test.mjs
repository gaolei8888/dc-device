import test from 'node:test';
import assert from 'node:assert/strict';
import { showImage, imageToRgb565 } from '../../sdk/js/show_image.mjs';

function solid(r, g, b, width = 320, height = 240) {
  const data = new Uint8ClampedArray(width * height * 4);
  for (let i = 0; i < data.length; i += 4) { data[i] = r; data[i + 1] = g; data[i + 2] = b; data[i + 3] = 255; }
  return { width, height, data };
}
const fakeAct = (script = []) => {
  const calls = [];
  const act = async (cap, args) => { calls.push([cap, args]); const o = script.shift(); if (o instanceof Error) throw o; return o ?? { ok: true }; };
  return { act, calls };
};
const limited = () => ({ ok: false, error: 'RateLimited', message: 'too fast' });

test('solid red landscape becomes all red portrait, big-endian', () => {
  const d = imageToRgb565(solid(255, 0, 0));
  assert.equal(d.length, 240 * 320 * 2);
  for (let i = 0; i < d.length; i += 2) { assert.equal(d[i], 0xF8); assert.equal(d[i + 1], 0x00); }
});

test('cover and crop keeps the centre: red left, blue right, edge at x=120', () => {
  const img = solid(255, 0, 0);
  for (let y = 0; y < 240; y++) for (let x = 160; x < 320; x++) { const i = (y * 320 + x) * 4; img.data[i] = 0; img.data[i + 2] = 255; }
  const d = imageToRgb565(img);
  const px = (x, y) => [d[(y * 240 + x) * 2], d[(y * 240 + x) * 2 + 1]];
  assert.deepEqual(px(100, 10), [0xF8, 0x00]);
  assert.deepEqual(px(140, 10), [0x00, 0x1F]);
  assert.deepEqual(px(100, 310), [0xF8, 0x00]);
});

test('RGB data and a screen-size portrait are accepted unscaled', () => {
  const data = new Uint8Array(240 * 320 * 3);
  for (let i = 0; i < data.length; i += 3) data[i + 1] = 255;
  const d = imageToRgb565({ width: 240, height: 320, data });
  assert.deepEqual([d[0], d[1]], [0x07, 0xE0]);
});

test('twenty blocks of 7680 bytes, paced, with progress', async () => {
  const { act, calls } = fakeAct(); const progress = [];
  const t0 = Date.now();
  const r = await showImage({ act, image: solid(255, 255, 255), pause: 5, onProgress: (p) => progress.push(p.sent) });
  assert.deepEqual(r, { ok: true, blocks: 20, sent: 20 });
  assert.equal(calls.length, 20);
  calls.forEach(([cap, a], i) => {
    assert.equal(cap, 'screen.write_block');
    assert.deepEqual([a.x, a.y, a.width, a.height], [0, i * 16, 240, 16]);
    assert.equal(Buffer.from(a.data_base64, 'base64').length, 7680);
  });
  assert.deepEqual(progress, Array.from({ length: 20 }, (_, i) => i + 1));
  assert.ok(Date.now() - t0 >= 19 * 5);
});

test('default pause is at least 100 ms', async () => {
  const { act } = fakeAct(); const t0 = Date.now();
  await showImage({ act, image: solid(0, 0, 0, 240, 320), signal: undefined, retries: 0, onProgress: ({ sent }) => { if (sent === 3) throw new Error('stop'); } }).catch(() => {});
  assert.ok(Date.now() - t0 >= 200);
});

test('RateLimited is retried on the same block', async () => {
  const { act, calls } = fakeAct([limited(), limited()]);
  const r = await showImage({ act, image: solid(0, 0, 0), pause: 1 });
  assert.equal(r.ok, true); assert.equal(calls.length, 22);
  assert.equal(calls[0][1].y, calls[2][1].y);
});

test('gives up after the retries and says which block', async () => {
  const { act, calls } = fakeAct(Array.from({ length: 10 }, limited));
  const r = await showImage({ act, image: solid(0, 0, 0), pause: 1, retries: 3 });
  assert.deepEqual([r.ok, r.failedAt, r.error, r.sent], [false, 0, 'RateLimited', 0]);
  assert.equal(calls.length, 4);
});

test('another error stops at once and carries the code', async () => {
  const bad = { ok: false, error: 'InvalidArguments', code: 'bad_block_size', message: 'm' };
  const { act, calls } = fakeAct([{ ok: true }, { ok: true }, bad]);
  const r = await showImage({ act, image: solid(0, 0, 0), pause: 1 });
  assert.deepEqual([r.ok, r.failedAt, r.code, r.sent], [false, 2, 'bad_block_size', 2]);
  assert.equal(calls.length, 3);
});

test('a thrown error with a body is read, and RateLimited in it is retried', async () => {
  const e = Object.assign(new Error('HTTP 429'), { body: JSON.stringify({ ok: false, error: 'RateLimited' }) });
  const { act, calls } = fakeAct([e]);
  const r = await showImage({ act, image: solid(0, 0, 0), pause: 1 });
  assert.equal(r.ok, true); assert.equal(calls.length, 21);
  const { act: act2 } = fakeAct([new Error('network down')]);
  const r2 = await showImage({ act: act2, image: solid(0, 0, 0), pause: 1 });
  assert.deepEqual([r2.ok, r2.error, r2.failedAt], [false, 'ActError', 0]);
});

test('abort stops before the next block', async () => {
  const { act, calls } = fakeAct(); const ac = new AbortController();
  const r = await showImage({ act, image: solid(0, 0, 0), pause: 1, signal: ac.signal, onProgress: ({ sent }) => { if (sent === 3) ac.abort(); } });
  assert.deepEqual([r.ok, r.cancelled, r.sent], [false, true, 3]);
  assert.equal(calls.length, 3);
});

test('a jpeg needs decode; with decode it works; nothing is sent on bad input', async () => {
  const { act, calls } = fakeAct();
  await assert.rejects(showImage({ act, jpeg: new Uint8Array([1]) }), /decode/);
  const r = await showImage({ act, jpeg: new Uint8Array([1]), decode: async () => solid(0, 0, 255), pause: 1 });
  assert.equal(r.ok, true);
  const n = calls.length;
  await assert.rejects(showImage({ act, image: { width: 2, height: 2, data: new Uint8Array(5) } }), /RGBA or RGB/);
  assert.equal(calls.length, n);
});
