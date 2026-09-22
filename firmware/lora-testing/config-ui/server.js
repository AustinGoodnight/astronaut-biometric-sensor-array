import express from "express";
import { createServer } from "node:http";
import { WebSocketServer } from "ws";
import { SerialPort } from "serialport";
import { ReadlineParser } from "@serialport/parser-readline";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const BAUD_RATE = 115200; // must match monitor_speed in platformio.ini
const ROLES = ["tx", "rx"];

const app = express();
app.use(express.static(path.join(__dirname, "public")));
app.get("/api/ports", async (_req, res) => {
  try {
    const ports = await SerialPort.list();
    res.json(ports);
  } catch (err) {
    res.status(500).json({ error: String(err) });
  }
});

const httpServer = createServer(app);
const wss = new WebSocketServer({ server: httpServer });

// role -> { port, parser }
const connections = {};

function broadcast(msg) {
  const data = JSON.stringify(msg);
  for (const client of wss.clients) {
    if (client.readyState === client.OPEN) client.send(data);
  }
}

function disconnectRole(role) {
  const conn = connections[role];
  if (!conn) return;
  clearInterval(conn.helloTimer);
  conn.port.close(() => {});
  delete connections[role];
  broadcast({ type: "status", role, connected: false });
}

// macOS's USB-CDC serial driver can be flaky about handing over data right
// after a native-USB board's port is reopened — the OS reports the port as
// open, but no bytes arrive for several seconds even though the board is
// transmitting the whole time. A single GET sent shortly after open can get
// lost in that window, so retry until we hear *anything* from the board
// rather than assuming one attempt was enough.
const HELLO_RETRY_MS = 2000;
const HELLO_MAX_ATTEMPTS = 6;

function connectRole(role, portPath) {
  disconnectRole(role);

  const port = new SerialPort({ path: portPath, baudRate: BAUD_RATE });
  const parser = port.pipe(new ReadlineParser({ delimiter: "\n" }));
  const conn = { port, parser, path: portPath, helloTimer: null, heardAnything: false };
  connections[role] = conn;

  port.on("open", () => {
    broadcast({ type: "status", role, connected: true, path: portPath, waiting: true });

    let attempts = 0;
    conn.helloTimer = setInterval(() => {
      if (conn.heardAnything) {
        clearInterval(conn.helloTimer);
        return;
      }
      attempts += 1;
      if (attempts > HELLO_MAX_ATTEMPTS) {
        clearInterval(conn.helloTimer);
        broadcast({
          type: "error",
          role,
          message: "no response from board after several attempts — try reconnecting, or unplug/replug the board",
        });
        return;
      }
      sendCommand(role, "GET");
    }, HELLO_RETRY_MS);
    sendCommand(role, "GET"); // fire the first attempt immediately too
  });

  port.on("error", (err) => {
    broadcast({ type: "error", role, message: String(err.message || err) });
  });

  port.on("close", () => {
    clearInterval(conn.helloTimer);
    broadcast({ type: "status", role, connected: false });
  });

  parser.on("data", (line) => {
    const text = line.trim();
    if (!text) return;
    if (!conn.heardAnything) {
      conn.heardAnything = true;
      clearInterval(conn.helloTimer);
      broadcast({ type: "status", role, connected: true, path: portPath, waiting: false });
    }
    broadcast({ type: "line", role, text, ts: Date.now() });
  });
}

function sendCommand(role, command) {
  const conn = connections[role];
  if (!conn || !conn.port.isOpen) {
    broadcast({ type: "error", role, message: "not connected" });
    return;
  }
  conn.port.write(command + "\n");
}

wss.on("connection", (ws) => {
  // let a freshly-opened UI tab see current status right away
  for (const role of ROLES) {
    const conn = connections[role];
    ws.send(
      JSON.stringify({
        type: "status",
        role,
        connected: Boolean(conn),
        path: conn ? conn.path : null,
      })
    );
  }

  ws.on("message", (raw) => {
    let msg;
    try {
      msg = JSON.parse(raw.toString());
    } catch {
      return;
    }
    if (!ROLES.includes(msg.role)) return;

    if (msg.type === "connect" && msg.path) {
      try {
        connectRole(msg.role, msg.path);
      } catch (err) {
        broadcast({ type: "error", role: msg.role, message: String(err) });
      }
    } else if (msg.type === "disconnect") {
      disconnectRole(msg.role);
    } else if (msg.type === "get") {
      sendCommand(msg.role, "GET");
    } else if (msg.type === "set") {
      const parts = [];
      if (Number.isFinite(msg.sf)) parts.push(`sf=${msg.sf}`);
      if (Number.isFinite(msg.bw)) parts.push(`bw=${msg.bw}`);
      if (Number.isFinite(msg.pwr)) parts.push(`pwr=${msg.pwr}`);
      if (parts.length > 0) sendCommand(msg.role, `SET ${parts.join(",")}`);
    }
  });
});

const PORT = process.env.PORT || 4173;
httpServer.listen(PORT, () => {
  console.log(`LoRa config UI running at http://localhost:${PORT}`);
});
