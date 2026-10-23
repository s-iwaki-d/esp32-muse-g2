# Even AI → Cloudflare → Muse Gadget

This Worker exposes an OpenAI Chat Completions compatible endpoint for the Even app. A single M5Stack StopWatch opens an outbound TLS WebSocket to a Durable Object, receives the latest user utterance, and returns the Muse response. No Muse credentials, browser cookies, model inference, or quota API are handled by this Worker.

## Configuration

For first-time deployment, follow the [Cloudflare Workers setup guide (Japanese)](../docs/cloudflare-setup.md).
It covers account login, creating the Worker and Durable Object, obtaining the actual URL,
generating local settings, uploading secrets, and configuring the Even app.

Use Node.js 22 or later. From this directory:

```sh
npm ci --ignore-scripts
npm test
```

The setup guide uses `scripts/configure.py` to generate separate random secrets and `.dev.vars` (ignored by Git). For local-only development, copy `.dev.vars.example` to `.dev.vars` if it does not already exist, then replace both example secrets with separate random values. `EVEN_API_KEY` belongs in the Even app; `DEVICE_TOKEN` belongs only in the StopWatch configuration. Both travel in `Authorization: Bearer …` headers; URL tokens are not supported.

`wrangler.toml` is a public template with an empty session ID. Create the ignored local configuration once, without overwriting an existing deployment:

```sh
test -f wrangler.local.toml || cp wrangler.toml wrangler.local.toml
```

Set your Worker name, optional account ID, and a dedicated `MUSE_SESSION_ID` UUID in `wrangler.local.toml`. It selects one stable Muse conversation for this bridge. Change it deliberately to begin a separate conversation. Generate your deployment's UUID before pairing and retain it when updating code. `REQUEST_TIMEOUT_MS` defaults to 120000 and may be set between 1 and 300000 milliseconds. The Even app's own timeout is undocumented and needs device validation.

A new session is created by the first message, then the firmware subscribes immediately after verifying the acknowledgement. Later messages subscribe before sending. The public SDK does not replay past replies, so an exceptionally fast first reply may be missed and time out. Verify a short setup message before use; never automatically replay a timed-out turn.

`npm run dev` and `npm run deploy` use `wrangler.local.toml`. Run `npm run dev` for local workerd testing. Local HTTP and `ws://` are development-only; the production StopWatch must use `wss://` with certificate validation. Wrangler can create local state under `.wrangler/`.

Deploy your own Worker in your Cloudflare account. The first `npm run deploy` in the setup guide creates the Worker and its Durable Object namespace; without secrets, chat and device requests return `503 not_configured`. After generating settings using the URL returned by that deployment, upload the credentials from this directory:

```sh
npx wrangler secret bulk ../.private/cloudflare-secrets.json --config wrangler.local.toml
npx wrangler secret list --config wrangler.local.toml
```

For subsequent code updates, use `npm run deploy` with the same Worker name and account.

In Even app → Settings → Even AI → Agent Configuration:

- Endpoint: `https://YOUR-WORKER.YOUR-SUBDOMAIN.workers.dev/v1/chat/completions`
- API Key: the `EVEN_API_KEY` secret

This menu requires EvenHub developer access. The official setup documents the endpoint/key but does not specify the app's precise request fields or timeout. See the [verification record](../docs/verification.md) for tested hardware behavior, and verify your own app and device setup.

## HTTP contract

- `POST /v1/chat/completions` requires the Even key and `application/json`.
- Body: `messages` containing 1–64 text messages, ending in a user message; optional boolean `stream`.
- Text content may be a string or an array of `{ "type": "text", "text": "…" }`.
- The entire UTF-8 request is limited to 32768 bytes; the latest user utterance is limited to 8192 bytes. Images, audio and client tool calls are rejected.
- NUL characters and unpaired UTF-16 surrogates are rejected to preserve text through the firmware's C-string JSON parser.
- Only the latest user utterance is forwarded. Muse owns history in the dedicated session. Earlier `messages`, `user`, `model`, sampling and token-limit fields do not change that session or agent. This is a single-user, single-conversation bridge.
- `stream: false` (default) returns a `chat.completion` JSON result. `stream: true` returns Chat Completion SSE chunks followed by `data: [DONE]`. Muse firmware currently waits for the authoritative full turn before sending answer chunks, so text is not streamed live from Muse.
- Answers are bounded to 65536 UTF-8 bytes. Usage/token counts are omitted because they are not known.
- Offline device: `503`; another active request: `409`; device failure: `502`; deadline: `504`.
- Once SSE headers have been sent, errors are SSE `{ "error": { "code", "message", "type" } }` followed by `[DONE]`, rather than an HTTP status change.
- During a turn, SSE sends a keepalive comment every 15 seconds. This does not extend the request deadline or prove agent progress.
- `/health` is a public liveness-only endpoint. It does not assert a connected device or working Muse session.

The bridge requests cancellation on timeout or client disconnect. WebSocket loss fails the current request. The bridge never replays a request automatically; users should check Muse's state before retrying an action that may already have run. ReadableStream cancellation is handled for SSE; request cancellation depends on the runtime's request abort signal. A restart closes a socket carrying an interrupted turn so firmware can reset cleanly.

## Device WebSocket contract

Connect to `GET /device/ws` using WebSocket upgrade and the device bearer token. A new authenticated connection replaces the previous connection, including one left half-open after a reboot or Wi-Fi loss. An interrupted request fails with `502 device_replaced` (or an SSE error); it is never replayed. There is one active connection, and retired sockets' late events are ignored. Use this token for one physical device. Cloudflare handles WebSocket fragmentation/reassembly; each complete text message is JSON. Binary messages are rejected.

Worker → StopWatch:

```json
{"type":"chat","id":"request-uuid","session_id":"dedicated-session-uuid","text":"こんにちは","expires_at":1792740000000}
{"type":"cancel","id":"request-uuid","reason":"timeout"}
```

`expires_at` is Unix time in milliseconds. IDs are new for every request. A device must not replay requests after reconnecting, and should reject work already past its deadline.

StopWatch → Worker:

```json
{"type":"started","id":"request-uuid"}
{"type":"delta","id":"request-uuid","text":"こんにちは。"}
{"type":"done","id":"request-uuid"}
{"type":"error","id":"request-uuid","code":"muse_unavailable","message":"Muse is unavailable."}
```

`delta` text is append-only. If Muse revises a provisional response, firmware must wait for final text before sending it. `done` without any non-whitespace text is an error. Late IDs and messages from an old socket are ignored. An optional `{ "type": "status", … }` is ignored and never completes a request. Keep individual encoded JSON frames small (firmware should use bounded chunks); the Worker rejects complete frames above 66560 UTF-8 bytes.

The device sends the literal text frame `ping` periodically; the server replies `pong`. Cloudflare's WebSocket auto-response handles this while the Durable Object sleeps. No quota message or manual quota feature is implemented.

## Verification

`npm test` uses Node's built-in test runner. It covers key separation, body and Unicode limits, offline/concurrent requests, request isolation, JSON and SSE answers, deadline/cancellation cleanup, malformed frames, output overflow, disconnect and restart behavior. It does not prove physical StopWatch, Muse Gadget or Even G2 behavior. Tests using fake sockets operate on complete WebSocket messages; frame fragmentation is the runtime's responsibility.

Sources:

- [Even's official BYOA setup](https://support.evenrealities.com/hc/en-us/articles/16280926743695-Tutorial-Bridging-G2-to-OpenClaw-Bring-Your-Own-Agent)
- [Cloudflare Durable Object WebSockets](https://developers.cloudflare.com/durable-objects/best-practices/websockets/)
- [Cloudflare WebSocket hibernation example](https://developers.cloudflare.com/durable-objects/examples/websocket-hibernation-server/)
