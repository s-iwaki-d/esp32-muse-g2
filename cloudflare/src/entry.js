import { DurableObject } from "cloudflare:workers";
import worker, { MuseBridge as BridgeCore } from "./index.js";

// Keep the production module's named exports limited to Durable Object classes.
// The dependency-free core remains directly testable under Node.
export default worker;

export class MuseBridge extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.bridge = new BridgeCore(ctx, env);
  }

  fetch(request) { return this.bridge.fetch(request); }
  webSocketMessage(socket, message) { return this.bridge.webSocketMessage(socket, message); }
  webSocketClose(socket, code, reason, wasClean) { return this.bridge.webSocketClose(socket, code, reason, wasClean); }
  webSocketError(socket, error) { return this.bridge.webSocketError(socket, error); }
}
