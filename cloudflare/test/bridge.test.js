import test from "node:test";
import assert from "node:assert/strict";
import worker, { MuseBridge, MAX_BODY_BYTES, MAX_OUTPUT_BYTES, readBody, validateChat } from "../src/index.js";

const EVEN = "test-even-secret";
const DEVICE = "test-device-secret";
const SESSION = "11111111-1111-4111-8111-111111111111";
class Socket {
  readyState = 1;
  sent = [];
  attachment = { active_id: null };
  send(value) { if (this.readyState !== 1) throw new Error("closed"); this.sent.push(value); }
  close(code, reason) { this.readyState = 3; this.closed = { code, reason }; }
  serializeAttachment(value) { this.attachment = structuredClone(value); }
  deserializeAttachment() { return structuredClone(this.attachment); }
}
function fixture({ connected = true, timeout = 1000, activeId = null } = {}) {
  const socket = new Socket();
  socket.attachment.active_id = activeId;
  const sockets = connected ? [socket] : [];
  const ctx = { getWebSockets: () => sockets, setWebSocketAutoResponse() {}, acceptWebSocket: (s) => sockets.push(s) };
  const env = { EVEN_API_KEY: EVEN, DEVICE_TOKEN: DEVICE, MUSE_SESSION_ID: SESSION, REQUEST_TIMEOUT_MS: String(timeout) };
  const relay = new MuseBridge(ctx, env);
  return { socket, sockets, env, relay };
}
function request(body = {}, options = {}) {
  return new Request("https://bridge.example/v1/chat/completions", {
    method: "POST", headers: { "Content-Type": "application/json", Authorization: `Bearer ${EVEN}`, ...options.headers },
    body: JSON.stringify({ messages: [{ role: "user", content: "こんにちは" }], ...body }), signal: options.signal,
  });
}
async function dispatched(relay, promise) {
  for (let i = 0; i < 20 && !relay.pending; i++) await new Promise((resolve) => setImmediate(resolve));
  assert.ok(relay.pending, "chat dispatched");
  return relay.pending.id;
}
function frame(relay, socket, id, type, rest = {}) { relay.webSocketMessage(socket, JSON.stringify({ type, id, ...rest })); }

test("Worker separates Even and device credentials and refuses absent secrets", async () => {
  let calls = 0;
  const env = { EVEN_API_KEY: EVEN, DEVICE_TOKEN: DEVICE, BRIDGE: {
    idFromName: (name) => name, get: () => ({ fetch: () => { calls++; return new Response("ok"); } }),
  } };
  assert.equal((await worker.fetch(request({}, { headers: { Authorization: `Bearer ${DEVICE}` } }), env)).status, 401);
  assert.equal((await worker.fetch(new Request("https://bridge.example/device/ws", { headers: { Authorization: `Bearer ${EVEN}` } }), env)).status, 401);
  assert.equal((await worker.fetch(request(), { ...env, EVEN_API_KEY: undefined })).status, 503);
  assert.equal((await worker.fetch(request(), env)).status, 200);
  assert.equal(calls, 1);
  assert.equal((await worker.fetch(new Request("https://bridge.example/health"), env)).status, 200);
  assert.equal((await worker.fetch(new Request("https://bridge.example/admin"), env)).status, 404);
});

test("text validation rejects empty, malformed, image, tool and oversized UTF-8 prompts", () => {
  for (const body of [null, [], {}, { messages: [] }, { stream: "true", messages: [{ role: "user", content: "hi" }] },
    { messages: [{ role: "assistant", content: "hello" }] }, { messages: [{ role: "user", content: "  " }] },
    { messages: [{ role: "user", content: [{ type: "image_url", image_url: "x" }] }] },
    { messages: [{ role: "tool", content: "x" }] }, { messages: [{ role: "user", content: "あ".repeat(2731) }] },
    { messages: [{ role: "user", content: "prefix\u0000suffix" }] },
    { messages: [{ role: "user", content: "lone\ud800" }] },
    { messages: [{ role: "user", content: "lone\udc00" }] },
    { messages: [{ role: "user", content: "hi" }], tools: [{ type: "function" }] }]) {
    assert.throws(() => validateChat(body));
  }
  assert.deepEqual(validateChat({ messages: [
    { role: "system", content: "context" }, { role: "user", content: [{ type: "text", text: "a" }, { type: "text", text: "b" }] },
  ], stream: true }), { text: "a\nb", stream: true });
  assert.equal(validateChat({ messages: [{ role: "user", content: "emoji 🎵" }] }).text, "emoji 🎵");
});

test("body reader enforces streamed byte limit without Content-Length", async () => {
  const huge = new Request("https://bridge.example", { method: "POST", headers: { "Content-Type": "application/json" },
    body: new ReadableStream({ start(controller) { controller.enqueue(new Uint8Array(MAX_BODY_BYTES)); controller.enqueue(new Uint8Array(1)); controller.close(); } }), duplex: "half" });
  await assert.rejects(() => readBody(huge), (error) => error.status === 413);
  const bad = new Request("https://bridge.example", { method: "POST", headers: { "Content-Type": "application/json" }, body: "{bad" });
  await assert.rejects(() => readBody(bad), (error) => error.code === "invalid_json");
});

test("offline device and unsupported media fail before dispatch", async () => {
  const { relay } = fixture({ connected: false });
  assert.equal((await relay.fetch(request())).status, 503);
  assert.equal((await relay.fetch(request({}, { headers: { "Content-Type": "text/plain" } }))).status, 415);
  assert.equal(relay.pending, null);
});

test("JSON request sends dedicated session and preserves Japanese response", async () => {
  const { relay, socket } = fixture();
  const response = relay.fetch(request());
  const id = await dispatched(relay, response);
  const sent = JSON.parse(socket.sent[0]);
  assert.equal(sent.type, "chat"); assert.equal(sent.id, id); assert.equal(sent.session_id, SESSION);
  assert.equal(sent.text, "こんにちは"); assert.ok(sent.expires_at > Date.now());
  frame(relay, socket, id, "started");
  frame(relay, socket, id, "delta", { text: "こんにちは、" });
  frame(relay, socket, id, "delta", { text: "テストユーザー。" });
  frame(relay, socket, id, "done");
  const result = await response;
  assert.equal(result.status, 200);
  const body = await result.json();
  assert.equal(body.choices[0].message.content, "こんにちは、テストユーザー。");
  assert.equal(body.object, "chat.completion"); assert.equal(body.usage, undefined);
  assert.equal(relay.pending, null); assert.equal(socket.attachment.active_id, null);
});

test("concurrent requests rejected and wrong ids or sockets cannot complete current turn", async () => {
  const { relay, socket } = fixture();
  const first = relay.fetch(request());
  const id = await dispatched(relay, first);
  assert.equal((await relay.fetch(request())).status, 409);
  frame(relay, socket, "stale", "delta", { text: "wrong" });
  frame(relay, new Socket(), id, "done");
  assert.equal(relay.pending.text, "");
  frame(relay, socket, id, "delta", { text: "right" }); frame(relay, socket, id, "done");
  assert.equal((await (await first).json()).choices[0].message.content, "right");
});

test("SSE is valid Chat Completion chunks and terminates with DONE", async () => {
  const { relay, socket } = fixture();
  const response = await relay.fetch(request({ stream: true }));
  const id = relay.pending.id;
  frame(relay, socket, id, "delta", { text: "hello" }); frame(relay, socket, id, "done");
  assert.match(response.headers.get("Content-Type"), /text\/event-stream/);
  const frames = (await response.text()).trim().split("\n\n").map((part) => part.slice(6));
  assert.equal(JSON.parse(frames[0]).choices[0].delta.role, "assistant");
  assert.equal(JSON.parse(frames[1]).choices[0].delta.content, "hello");
  assert.equal(JSON.parse(frames[2]).choices[0].finish_reason, "stop");
  assert.equal(frames[3], "[DONE]");
});

test("deadline cancels device and finishes JSON with 504", async () => {
  const { relay, socket } = fixture({ timeout: 10 });
  const response = await relay.fetch(request());
  assert.equal(response.status, 504); assert.equal((await response.json()).error.code, "timeout");
  assert.equal(JSON.parse(socket.sent.at(-1)).type, "cancel"); assert.equal(relay.pending, null);
});

test("SSE reader cancellation and request abort release pending work", async () => {
  const { relay, socket } = fixture();
  const response = await relay.fetch(request({ stream: true }));
  await response.body.cancel();
  assert.equal(relay.pending, null); assert.equal(JSON.parse(socket.sent.at(-1)).reason, "cancelled");
  const controller = new AbortController();
  const second = relay.fetch(request({}, { signal: controller.signal }));
  await dispatched(relay, second); controller.abort();
  assert.equal((await second).status, 499); assert.equal(relay.pending, null);
});

test("device disconnect reports failure and late response cannot pollute next turn", async () => {
  const { relay, socket, sockets } = fixture();
  const first = relay.fetch(request()); const oldId = await dispatched(relay, first);
  relay.webSocketClose(socket, 1006);
  assert.equal((await first).status, 502);
  const replacement = new Socket(); sockets.push(replacement);
  const next = relay.fetch(request()); const newId = await dispatched(relay, next);
  frame(relay, socket, oldId, "delta", { text: "old" });
  frame(relay, replacement, newId, "delta", { text: "new" }); frame(relay, replacement, newId, "done");
  assert.equal((await (await next).json()).choices[0].message.content, "new");
});

test("empty answer, device errors and malformed frames fail explicitly", async () => {
  for (const kind of ["empty", "error", "malformed"]) {
    const { relay, socket } = fixture(); const pending = relay.fetch(request()); const id = await dispatched(relay, pending);
    if (kind === "empty") frame(relay, socket, id, "done");
    if (kind === "error") frame(relay, socket, id, "error", { code: "muse_busy", message: "Muse is busy" });
    if (kind === "malformed") relay.webSocketMessage(socket, "not JSON");
    const response = await pending; assert.equal(response.status, 502);
    assert.equal(relay.pending, null);
    if (kind === "malformed") assert.equal(socket.closed.code, 1008);
  }
});

test("aggregate output bound rejects overflow and reports SSE error before DONE", async () => {
  const { relay, socket } = fixture(); const response = await relay.fetch(request({ stream: true })); const id = relay.pending.id;
  frame(relay, socket, id, "delta", { text: "x".repeat(MAX_OUTPUT_BYTES) });
  frame(relay, socket, id, "delta", { text: "あ" });
  const text = await response.text();
  assert.match(text, /response_too_large/); assert.ok(text.endsWith("data: [DONE]\n\n"));
  assert.equal(JSON.parse(socket.sent.at(-1)).reason, "response_too_large");
});

test("restart closes a socket with an interrupted request instead of replaying it", () => {
  const { socket, relay } = fixture({ activeId: "unfinished" });
  assert.equal(socket.closed.code, 1012); assert.equal(socket.sent.length, 0); assert.equal(relay.device(), null);
});

test("ping responds without entering a chat", () => {
  const { relay, socket } = fixture(); relay.webSocketMessage(socket, "ping");
  assert.deepEqual(socket.sent, ["pong"]); assert.equal(relay.pending, null);
});

test("authenticated replacement fails interrupted JSON and isolates late old-socket events", async () => {
  const { relay, socket, sockets } = fixture();
  // Model a half-open connection whose close event/state transition is delayed.
  socket.close = (code, reason) => { socket.closed = { code, reason }; };
  const interrupted = relay.fetch(request());
  const oldId = await dispatched(relay, interrupted);
  relay.replaceDevice(socket);
  assert.equal((await interrupted).status, 502);
  assert.equal(JSON.parse(socket.sent.at(-1)).reason, "device_replaced");
  assert.equal(socket.closed.code, 4001);
  assert.equal(socket.attachment.retired, true);
  assert.equal(relay.device(), null, "retired OPEN socket is never reused");
  const replacement = new Socket(); sockets.push(replacement);
  const current = relay.fetch(request()); const currentId = await dispatched(relay, current);
  frame(relay, socket, oldId, "done");
  frame(relay, socket, currentId, "delta", { text: "old socket injection" });
  relay.webSocketMessage(socket, "invalid JSON");
  relay.webSocketClose(socket, 4001);
  relay.webSocketError(socket);
  assert.equal(relay.pending.id, currentId);
  assert.equal(relay.device(), replacement);
  frame(relay, replacement, currentId, "delta", { text: "new connection answer" });
  frame(relay, replacement, currentId, "done");
  assert.equal((await (await current).json()).choices[0].message.content, "new connection answer");
});

test("replacement terminates interrupted SSE with an explicit error and DONE", async () => {
  const { relay, socket } = fixture();
  const response = await relay.fetch(request({ stream: true }));
  relay.replaceDevice(socket);
  const body = await response.text();
  assert.match(body, /"code":"device_replaced"/);
  assert.ok(body.endsWith("data: [DONE]\n\n"));
  assert.equal(relay.pending, null);
});
