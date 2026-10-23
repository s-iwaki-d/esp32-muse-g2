// Real workerd/Miniflare integration. Uses Wrangler's lockfile-pinned runtime.
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { Miniflare, convertV4MiniflareOptions } from 'miniflare';
import { fileURLToPath } from 'node:url';

function nextMessage(socket, predicate = () => true) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { socket.removeEventListener('message', listener); reject(new Error('device receive timeout')); }, 5000);
    const listener = (event) => {
      if (!predicate(event.data)) return;
      clearTimeout(timer);
      socket.removeEventListener('message', listener);
      resolve(event.data);
    };
    socket.addEventListener('message', listener);
  });
}

test('real Worker + Durable Object: authenticated WSS, Japanese JSON/SSE, reconnect replacement, timeout, disconnect', async () => {
  const mf = new Miniflare(convertV4MiniflareOptions({
    modules: ['entry.js', 'index.js'].map((name) => ({
      type: 'ESModule', path: fileURLToPath(new URL(`../src/${name}`, import.meta.url)),
    })),
    compatibilityDate: '2026-10-23',
    durableObjects: { BRIDGE: { className: 'MuseBridge', useSQLite: true } },
    bindings: {
      EVEN_API_KEY: 'test-only-even-key', DEVICE_TOKEN: 'test-only-device-key',
      MUSE_SESSION_ID: '11111111-1111-4111-8111-111111111111', REQUEST_TIMEOUT_MS: '1500',
    },
  }));
  const call = (stream = false) => mf.dispatchFetch('http://localhost/v1/chat/completions', {
    method: 'POST', headers: { Authorization: 'Bearer test-only-even-key', 'Content-Type': 'application/json' },
    body: JSON.stringify({ messages: [{ role: 'user', content: 'こんにちは、Muse' }], stream }),
  });
  try {
    assert.equal((await call()).status, 503);
    assert.equal((await mf.dispatchFetch('http://localhost/device/ws', {
      headers: { Upgrade: 'websocket', Authorization: 'Bearer test-only-even-key' },
    })).status, 401);
    const upgrade = await mf.dispatchFetch('http://localhost/device/ws', {
      headers: { Upgrade: 'websocket', Authorization: 'Bearer test-only-device-key' },
    });
    assert.equal(upgrade.status, 101);
    let ws = upgrade.webSocket;
    ws.accept();
    const pong = nextMessage(ws);
    ws.send('ping');
    assert.equal(await pong, 'pong');

    for (const stream of [false, true]) {
      const receive = nextMessage(ws);
      const responsePromise = call(stream);
      const job = JSON.parse(await receive);
      assert.equal(job.text, 'こんにちは、Muse');
      assert.equal(job.session_id, '11111111-1111-4111-8111-111111111111');
      assert.equal((await call()).status, 409);
      ws.send(JSON.stringify({ type: 'started', id: job.id }));
      ws.send(JSON.stringify({ type: 'delta', id: 'different-turn', text: 'MUST NOT LEAK' }));
      ws.send(JSON.stringify({ type: 'delta', id: job.id, text: 'こんにちは！🐱' }));
      ws.send(JSON.stringify({ type: 'done', id: job.id }));
      const response = await responsePromise;
      assert.equal(response.status, 200);
      if (stream) {
        const body = await response.text();
        assert.ok(body.includes('こんにちは！🐱'));
        assert.ok(body.endsWith('data: [DONE]\n\n'));
        assert.ok(!body.includes('MUST NOT LEAK'));
      } else {
        assert.equal((await response.json()).choices[0].message.content, 'こんにちは！🐱');
      }
    }
    for (const stream of [false, true]) {
      const old = ws;
      const receive = nextMessage(old);
      const interrupted = call(stream);
      const oldJob = JSON.parse(await receive);
      const cancelled = nextMessage(old);
      const reconnected = await mf.dispatchFetch('http://localhost/device/ws', {
        headers: { Upgrade: 'websocket', Authorization: 'Bearer test-only-device-key' },
      });
      assert.equal(reconnected.status, 101, 'authenticated reconnect replaces old connection');
      ws = reconnected.webSocket;
      ws.accept();
      assert.deepEqual(JSON.parse(await cancelled), { type: 'cancel', id: oldJob.id, reason: 'device_replaced' });
      const failed = await interrupted;
      if (stream) {
        const body = await failed.text();
        assert.ok(body.includes('device_replaced'));
        assert.ok(body.endsWith('data: [DONE]\n\n'));
      } else {
        assert.equal(failed.status, 502);
        assert.equal((await failed.json()).error.code, 'device_replaced');
      }
      const next = nextMessage(ws);
      const resumed = call();
      const job = JSON.parse(await next);
      old.close(1000, 'delayed old close');
      ws.send(JSON.stringify({ type: 'delta', id: oldJob.id, text: 'MUST NOT REPLAY' }));
      ws.send(JSON.stringify({ type: 'delta', id: job.id, text: '再接続後の回答' }));
      ws.send(JSON.stringify({ type: 'done', id: job.id }));
      assert.equal((await (await resumed).json()).choices[0].message.content, '再接続後の回答');
    }
    const receive = nextMessage(ws);
    const timed = call();
    const job = JSON.parse(await receive);
    const cancellation = nextMessage(ws);
    assert.equal((await timed).status, 504);
    assert.deepEqual(JSON.parse(await cancellation), { type: 'cancel', id: job.id, reason: 'timeout' });
    ws.close(1000, 'test finished');
    // Observe the close through a subsequent fetch, without a fixed sleep.
    let status;
    for (let i = 0; i < 5; i++) {
      status = (await call()).status;
      if (status === 503) break;
    }
    assert.equal(status, 503);
  } finally {
    await mf.dispose();
  }
});
