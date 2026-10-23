const encoder = new TextEncoder();
export const MAX_BODY_BYTES = 32768;
export const MAX_INPUT_BYTES = 8192;
export const MAX_OUTPUT_BYTES = 65536;
const MAX_FRAME_BYTES = MAX_OUTPUT_BYTES + 1024;
const JSON_HEADERS = { "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store" };

export function apiError(status, code, message) {
  return new Response(JSON.stringify({ error: { message, type: "bridge_error", code } }), {
    status, headers: JSON_HEADERS,
  });
}

export function authorized(request, secret) {
  if (typeof secret !== "string" || !secret) return false;
  const expected = encoder.encode(`Bearer ${secret}`);
  const actual = encoder.encode(request.headers.get("Authorization") || "");
  let different = expected.length ^ actual.length;
  for (let i = 0; i < expected.length; i++) different |= expected[i] ^ (actual[i] || 0);
  return different === 0;
}

class InputError extends Error {
  constructor(status, code, message) { super(message); this.status = status; this.code = code; }
}

export async function readBody(request) {
  if (!request.headers.get("Content-Type")?.toLowerCase().startsWith("application/json")) {
    throw new InputError(415, "unsupported_media_type", "Use application/json.");
  }
  const declared = Number(request.headers.get("Content-Length"));
  if (Number.isFinite(declared) && declared > MAX_BODY_BYTES) {
    throw new InputError(413, "body_too_large", "Request body exceeds 32768 bytes.");
  }
  if (!request.body) throw new InputError(400, "invalid_json", "A JSON request body is required.");
  const reader = request.body.getReader();
  const chunks = [];
  let bytes = 0;
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      bytes += value.byteLength;
      if (bytes > MAX_BODY_BYTES) {
        await reader.cancel();
        throw new InputError(413, "body_too_large", "Request body exceeds 32768 bytes.");
      }
      chunks.push(value);
    }
  } finally {
    reader.releaseLock();
  }
  const joined = new Uint8Array(bytes);
  let offset = 0;
  for (const chunk of chunks) { joined.set(chunk, offset); offset += chunk.byteLength; }
  try { return JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(joined)); }
  catch { throw new InputError(400, "invalid_json", "Request must contain valid UTF-8 JSON."); }
}

export function validateChat(body) {
  if (!body || typeof body !== "object" || Array.isArray(body)) {
    throw new InputError(400, "invalid_request", "Expected a JSON object.");
  }
  if (body.stream !== undefined && typeof body.stream !== "boolean") {
    throw new InputError(400, "invalid_request", "stream must be a boolean.");
  }
  if (!Array.isArray(body.messages) || body.messages.length < 1 || body.messages.length > 64) {
    throw new InputError(400, "invalid_messages", "messages must contain 1 to 64 entries.");
  }
  for (const message of body.messages) {
    if (!message || !["system", "developer", "user", "assistant"].includes(message.role)) {
      throw new InputError(400, "invalid_messages", "Only text system, developer, user and assistant messages are supported.");
    }
    textContent(message.content);
  }
  const latest = body.messages.at(-1);
  if (latest.role !== "user") {
    throw new InputError(400, "invalid_messages", "The last message must be a user message.");
  }
  const text = textContent(latest.content);
  if (text.includes("\u0000") || !text.isWellFormed()) {
    throw new InputError(400, "unsupported_content", "The user message must not contain NUL or unpaired UTF-16 surrogates.");
  }
  if (!text.trim()) throw new InputError(400, "empty_prompt", "The user message must not be empty.");
  if (encoder.encode(text).byteLength > MAX_INPUT_BYTES) {
    throw new InputError(413, "prompt_too_large", "The user message exceeds 8192 UTF-8 bytes.");
  }
  if (body.tools?.length || body.tool_choice !== undefined) {
    throw new InputError(400, "unsupported_tools", "Client tool calling is not supported.");
  }
  return { text, stream: body.stream === true };
}

function textContent(content) {
  if (typeof content === "string") return content;
  if (Array.isArray(content) && content.every((part) => part?.type === "text" && typeof part.text === "string")) {
    return content.map((part) => part.text).join("\n");
  }
  throw new InputError(400, "unsupported_content", "Only text message content is supported.");
}

export default {
  async fetch(request, env) {
    const path = new URL(request.url).pathname;
    if (path === "/health" && request.method === "GET") {
      return new Response(JSON.stringify({ ok: true, service: "even-muse-bridge" }), { headers: JSON_HEADERS });
    }
    let secret;
    if (path === "/v1/chat/completions" && request.method === "POST") secret = env.EVEN_API_KEY;
    else if (path === "/device/ws" && request.method === "GET") secret = env.DEVICE_TOKEN;
    else return apiError(404, "not_found", "Endpoint not found.");
    if (!secret) return apiError(503, "not_configured", "Bridge credentials are not configured.");
    if (!authorized(request, secret)) return apiError(401, "unauthorized", "Invalid credentials.");
    return env.BRIDGE.get(env.BRIDGE.idFromName("paired-device")).fetch(request);
  },
};

export class MuseBridge {
  constructor(ctx, env) {
    this.ctx = ctx;
    this.env = env;
    this.pending = null;
    this.retiredSockets = new WeakSet();
    if (typeof WebSocketRequestResponsePair !== "undefined") {
      ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair("ping", "pong"));
    }
    // A hibernation restart must never replay a possibly executed Muse request.
    for (const socket of ctx.getWebSockets()) {
      if (socket.deserializeAttachment()?.active_id) {
        this.retireSocket(socket);
        try { socket.close(1012, "Interrupted turn; reconnect"); } catch { /* already closed */ }
      }
    }
  }

  async fetch(request) {
    const path = new URL(request.url).pathname;
    if (path === "/device/ws" && request.method === "GET") {
      if (!authorized(request, this.env.DEVICE_TOKEN)) return apiError(401, "unauthorized", "Invalid credentials.");
      return this.connectDevice(request);
    }
    if (path !== "/v1/chat/completions" || request.method !== "POST") {
      return apiError(404, "not_found", "Endpoint not found.");
    }
    if (!authorized(request, this.env.EVEN_API_KEY)) return apiError(401, "unauthorized", "Invalid credentials.");
    let chat;
    try { chat = validateChat(await readBody(request)); }
    catch (error) {
      return error instanceof InputError
        ? apiError(error.status, error.code, error.message)
        : apiError(400, "invalid_request", "Unable to read request body.");
    }
    if (this.pending) return apiError(409, "device_busy", "Muse is handling another request.");
    const socket = this.device();
    if (!socket) return apiError(503, "device_offline", "StopWatch is not connected.");
    if (request.signal.aborted) return apiError(499, "cancelled", "Request was cancelled.");
    if (!/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(this.env.MUSE_SESSION_ID || "")) {
      return apiError(503, "not_configured", "MUSE_SESSION_ID must be a UUID.");
    }
    return this.startChat(request, socket, chat);
  }

  device() {
    return this.ctx.getWebSockets().find((socket) => socket.readyState === 1 && !this.isRetired(socket)) || null;
  }

  isRetired(socket) {
    return this.retiredSockets.has(socket) || socket.deserializeAttachment()?.retired;
  }

  retireSocket(socket) {
    this.retiredSockets.add(socket);
    try { this.updateSocket(socket, { active_id: null, retired: true }); } catch { /* closing socket */ }
  }

  updateSocket(socket, fields) {
    socket.serializeAttachment({ ...socket.deserializeAttachment(), ...fields });
  }

  replaceDevice(socket) {
    if (this.pending?.socket === socket) {
      this.finish(this.pending, "device_replaced", "StopWatch reconnected during the request. The request was not replayed.", 502, true);
    }
    // A dropped Wi-Fi link may leave the server-side socket OPEN indefinitely.
    // Mark it retired before closing, so late events cannot reselect it.
    this.retireSocket(socket);
    try { socket.close(4001, "Replaced by authenticated reconnect"); } catch { /* already closed */ }
  }

  connectDevice(request) {
    if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket") {
      return apiError(426, "websocket_required", "A WebSocket upgrade is required.");
    }
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.ctx.acceptWebSocket(server);
    server.serializeAttachment({ active_id: null, retired: false });
    // Both entry points authenticate the same paired device token before here.
    // New authentication replaces an old connection; it never replays a turn.
    for (const socket of this.ctx.getWebSockets()) {
      if (socket !== server && !this.isRetired(socket)) this.replaceDevice(socket);
    }
    return new Response(null, { status: 101, webSocket: client });
  }

  startChat(request, socket, chat) {
    const id = crypto.randomUUID();
    const created = Math.floor(Date.now() / 1000);
    const timeoutValue = Number(this.env.REQUEST_TIMEOUT_MS);
    const timeout = Number.isFinite(timeoutValue) && timeoutValue > 0 && timeoutValue <= 300000 ? timeoutValue : 120000;
    let resolve;
    let response;
    const job = { id, socket, created, stream: chat.stream, text: "", bytes: 0, controller: null, resolve: null, timer: null };
    this.pending = job;
    if (chat.stream) {
      const stream = new ReadableStream({
        start: (controller) => { job.controller = controller; },
        cancel: () => { this.finish(job, "cancelled", "The response reader disconnected.", 499, true, true); },
      });
      response = new Response(stream, { headers: {
        "Content-Type": "text/event-stream; charset=utf-8", "Cache-Control": "no-cache, no-transform",
        "X-Accel-Buffering": "no",
      } });
      this.emit(job, this.chunk(job, { role: "assistant", content: "" }));
      job.heartbeat = setInterval(() => {
        try { job.controller.enqueue(encoder.encode(": keepalive\n\n")); } catch { /* cancellation cleans up */ }
      }, 15000);
    } else {
      response = new Promise((complete) => { resolve = complete; });
      job.resolve = resolve;
    }
    const onAbort = () => this.finish(job, "cancelled", "The caller disconnected.", 499, true);
    request.signal.addEventListener("abort", onAbort, { once: true });
    job.removeAbort = () => request.signal.removeEventListener("abort", onAbort);
    job.timer = setTimeout(() => this.finish(job, "timeout", "Muse did not finish before the request deadline.", 504, true), timeout);
    try {
      this.updateSocket(socket, { active_id: id });
      socket.send(JSON.stringify({ type: "chat", id, text: chat.text,
        session_id: this.env.MUSE_SESSION_ID, expires_at: Date.now() + timeout }));
    } catch {
      this.finish(job, "device_disconnected", "StopWatch disconnected before delivery.", 502);
    }
    return response;
  }

  chunk(job, delta, finishReason = null) {
    return { id: `chatcmpl-${job.id}`, object: "chat.completion.chunk", created: job.created,
      model: "muse", choices: [{ index: 0, delta, finish_reason: finishReason }] };
  }

  emit(job, data) {
    if (!job.controller) return;
    try { job.controller.enqueue(encoder.encode(`data: ${typeof data === "string" ? data : JSON.stringify(data)}\n\n`)); }
    catch { /* cancellation performs cleanup separately */ }
  }

  finish(job, code = null, message = null, status = 200, cancel = false, readerCancelled = false) {
    if (this.pending !== job) return;
    this.pending = null;
    clearTimeout(job.timer);
    clearInterval(job.heartbeat);
    job.removeAbort?.();
    try { this.updateSocket(job.socket, { active_id: null }); } catch { /* closed socket */ }
    if (cancel) {
      try { job.socket.send(JSON.stringify({ type: "cancel", id: job.id, reason: code })); } catch { /* disconnected */ }
    }
    if (job.stream) {
      if (!readerCancelled) {
        this.emit(job, code ? { error: { code, message, type: "bridge_error" } } : this.chunk(job, {}, "stop"));
        this.emit(job, "[DONE]");
        try { job.controller.close(); } catch { /* already cancelled */ }
      }
    } else {
      job.resolve(code ? apiError(status, code, message) : new Response(JSON.stringify({
        id: `chatcmpl-${job.id}`, object: "chat.completion", created: job.created, model: "muse",
        choices: [{ index: 0, message: { role: "assistant", content: job.text }, finish_reason: "stop" }],
      }), { headers: JSON_HEADERS }));
    }
  }

  webSocketMessage(socket, raw) {
    if (this.isRetired(socket)) return;
    if (raw === "ping") { socket.send("pong"); return; }
    if (typeof raw !== "string" || encoder.encode(raw).byteLength > MAX_FRAME_BYTES) {
      this.badFrame(socket); return;
    }
    let message;
    try { message = JSON.parse(raw); } catch { this.badFrame(socket); return; }
    if (!message || typeof message !== "object" || Array.isArray(message)) { this.badFrame(socket); return; }
    // Status is deliberately not a substitute for a correlated turn response.
    if (message.type === "status") return;
    const job = this.pending;
    if (!job || socket !== job.socket || message.id !== job.id) return;
    if (message.type === "started") return;
    if (message.type === "delta") {
      if (typeof message.text !== "string") { this.badFrame(socket); return; }
      const bytes = encoder.encode(message.text).byteLength;
      if (job.bytes + bytes > MAX_OUTPUT_BYTES) {
        this.finish(job, "response_too_large", "Muse response exceeded 65536 UTF-8 bytes.", 502, true); return;
      }
      job.bytes += bytes;
      job.text += message.text;
      if (job.stream) this.emit(job, this.chunk(job, { content: message.text }));
      return;
    }
    if (message.type === "done") {
      if (!job.text.trim()) this.finish(job, "empty_response", "Muse returned no text.", 502);
      else this.finish(job);
      return;
    }
    if (message.type === "error") {
      const code = typeof message.code === "string" && /^[a-z0-9_]{1,64}$/.test(message.code) ? message.code : "muse_error";
      const detail = typeof message.message === "string" ? message.message.slice(0, 512) : "Muse could not complete the request.";
      this.finish(job, code, detail, 502);
      return;
    }
    this.badFrame(socket);
  }

  badFrame(socket) {
    if (this.pending?.socket === socket) this.finish(this.pending, "invalid_device_frame", "StopWatch sent an invalid response.", 502, true);
    try { socket.close(1008, "Invalid device frame"); } catch { /* already closed */ }
  }

  webSocketClose(socket, code) {
    if (this.pending?.socket === socket) this.finish(this.pending, "device_disconnected", "StopWatch disconnected during the request.", 502);
    try { socket.close(code === 1005 || code === 1006 ? 1000 : code, "Connection closed"); } catch { /* already closed */ }
  }

  webSocketError(socket) { this.webSocketClose(socket, 1011); }
}
