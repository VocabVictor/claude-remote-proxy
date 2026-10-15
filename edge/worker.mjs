// Authenticated, bounded echo only. No upstream SSH or arbitrary TCP forwarding.
function authenticated(request, secret) {
  if (!secret) return false;
  const protocols = (request.headers.get("Sec-WebSocket-Protocol") || "").split(",").map(s => s.trim());
  const supplied = request.headers.get("Authorization")?.replace(/^Bearer /, "") ||
    protocols.find(s => s.startsWith("ccproxy-auth."))?.slice(13) || "";
  if (supplied.length !== secret.length) return false;
  let difference = 0;
  for (let i = 0; i < secret.length; ++i) difference |= supplied.charCodeAt(i) ^ secret.charCodeAt(i);
  return difference === 0;
}

export default {
  async fetch(request, env) {
    if (!authenticated(request, env.EDGE_AUTH_TOKEN)) return new Response("Unauthorized", { status: 401 });
    if (new URL(request.url).pathname !== "/probe" || request.method !== "GET" ||
        request.headers.get("Upgrade")?.toLowerCase() !== "websocket") return new Response("Not found", { status: 404 });
    const [client, server] = Object.values(new WebSocketPair());
    server.binaryType = "arraybuffer";
    server.accept();
    const headers = { "X-Edge-Colo": request.cf?.colo || "unknown" };
    if ((request.headers.get("Sec-WebSocket-Protocol") || "").split(",").some(p => p.trim() === "ccproxy-v1"))
      headers["Sec-WebSocket-Protocol"] = "ccproxy-v1";
    let bytes = 0, messages = 0;
    server.addEventListener("message", event => {
      if (typeof event.data === "string" || event.data.byteLength > 65536 ||
          ++messages > 2048 || (bytes += event.data.byteLength) > 1048576) server.close(1008, "Probe limit");
      else server.send(event.data);
    });
    return new Response(null, { status: 101, webSocket: client, headers });
  }
};
