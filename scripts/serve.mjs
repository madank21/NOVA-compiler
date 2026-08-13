// Production entry point for the NOVA Studio Docker image.
// - spawns the native compiler backend (nova_server) on 127.0.0.1:8080
//   and restarts it if it exits (watchdog)
// - serves the built frontend from dist/
// - proxies /api/* to the native backend (same-origin, no CORS needed)
//
// Env: PORT (default 3000), HOST (default 0.0.0.0), NOVA_PORT (default 8080),
//      NOVA_SERVER_BIN (default <root>/nova_server)

import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { readFile, stat } from 'node:fs/promises';
import { extname, join, normalize, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { connect } from 'node:net';

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(__dirname, '..');
const DIST = join(ROOT, 'dist');
const NOVA_SERVER_BIN = process.env.NOVA_SERVER_BIN || join(ROOT, 'nova_server');

const PORT = Number(process.env.PORT || 3000);
const HOST = process.env.HOST || '0.0.0.0';
const BACKEND_HOST = '127.0.0.1';
const BACKEND_PORT = Number(process.env.NOVA_PORT || 8080);
const MAX_PROXY_BODY = 256 * 1024;

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.ico': 'image/x-icon',
  '.woff': 'font/woff',
  '.woff2': 'font/woff2',
  '.map': 'application/json'
};

// ---------------------------------------------------------------------------
// native backend lifecycle (watchdog)
// ---------------------------------------------------------------------------

function startBackend() {
  const backend = spawn(NOVA_SERVER_BIN, [], {
    env: { ...process.env, NOVA_HOST: BACKEND_HOST, NOVA_PORT: String(BACKEND_PORT) },
    stdio: ['ignore', 'inherit', 'inherit']
  });
  backend.on('exit', (code) => {
    console.error(`[serve] nova_server exited (${code}) — restarting in 1s`);
    setTimeout(startBackend, 1000);
  });
  backend.on('error', (err) => {
    console.error(`[serve] failed to start nova_server: ${err.message}`);
  });
}

// ---------------------------------------------------------------------------
// /api proxy to the native backend
// ---------------------------------------------------------------------------

function proxy(req, res) {
  const chunks = [];
  let size = 0;
  let aborted = false;
  req.on('data', (c) => {
    size += c.length;
    if (size > MAX_PROXY_BODY) {
      aborted = true;
      res.writeHead(413, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: 'request too large' }));
      req.destroy();
      return;
    }
    chunks.push(c);
  });
  req.on('end', () => {
    if (aborted) return;
    const body = Buffer.concat(chunks);
    const upstream = connect({ host: BACKEND_HOST, port: BACKEND_PORT });
    const path = req.url || '/';
    upstream.on('connect', () => {
      upstream.write(
        `${req.method} ${path} HTTP/1.1\r\n` +
        `Host: ${BACKEND_HOST}:${BACKEND_PORT}\r\n` +
        `Content-Type: ${req.headers['content-type'] || 'application/json'}\r\n` +
        `Content-Length: ${body.length}\r\n` +
        `Connection: close\r\n\r\n`
      );
      upstream.write(body);
    });
    const respChunks = [];
    upstream.on('data', (c) => respChunks.push(c));
    upstream.on('end', () => {
      const raw = Buffer.concat(respChunks);
      const split = raw.indexOf('\r\n\r\n');
      if (split === -1) {
        res.writeHead(502, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: 'backend returned malformed response' }));
        return;
      }
      const headers = raw.slice(0, split).toString();
      const respBody = raw.slice(split + 4);
      const statusMatch = headers.match(/^HTTP\/1\.[01] (\d{3})/);
      const status = statusMatch ? Number(statusMatch[1]) : 200;
      const contentType = (headers.match(/Content-Type:\s*([^\r\n]+)/i) || [])[1] || 'application/json';
      res.writeHead(status, {
        'Content-Type': contentType,
        'Access-Control-Allow-Origin': '*',
        'Cache-Control': 'no-store'
      });
      res.end(respBody);
    });
    upstream.on('error', () => {
      if (!res.headersSent) {
        res.writeHead(502, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: 'native backend unavailable' }));
      }
    });
  });
}

// ---------------------------------------------------------------------------
// static files
// ---------------------------------------------------------------------------

async function serveStatic(req, res) {
  const urlPath = decodeURIComponent((req.url || '/').split('?')[0]);
  let filePath = normalize(join(DIST, urlPath));
  if (!filePath.startsWith(DIST)) {
    res.writeHead(403, { 'Content-Type': 'text/plain' });
    res.end('forbidden');
    return;
  }
  try {
    const st = await stat(filePath);
    if (st.isDirectory()) filePath = join(filePath, 'index.html');
  } catch {
    filePath = join(DIST, 'index.html'); // SPA fallback
  }
  try {
    const data = await readFile(filePath);
    const type = MIME[extname(filePath)] || 'application/octet-stream';
    const cache = extname(filePath) === '.html' ? 'no-cache' : 'public, max-age=31536000, immutable';
    res.writeHead(200, { 'Content-Type': type, 'Cache-Control': cache });
    res.end(data);
  } catch {
    res.writeHead(404, { 'Content-Type': 'text/plain' });
    res.end('not found');
  }
}

// ---------------------------------------------------------------------------

const server = createServer((req, res) => {
  const url = req.url || '/';
  if (url === '/api/health-proxy') {
    // liveness of this front server itself (used by the Docker HEALTHCHECK)
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ status: 'ok' }));
    return;
  }
  if (url.startsWith('/api/')) {
    if (req.method === 'OPTIONS') {
      res.writeHead(204, {
        'Access-Control-Allow-Origin': '*',
        'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
        'Access-Control-Allow-Headers': 'Content-Type'
      });
      res.end();
      return;
    }
    proxy(req, res);
    return;
  }
  serveStatic(req, res);
});

startBackend();
server.listen(PORT, HOST, () => {
  console.log(`[serve] NOVA Studio on http://${HOST}:${PORT} (API proxied to nova_server ${BACKEND_HOST}:${BACKEND_PORT})`);
});