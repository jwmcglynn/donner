import { createReadStream } from "node:fs";
import { stat } from "node:fs/promises";
import { createServer } from "node:http";
import path from "node:path";
import { pipeline } from "node:stream/promises";

function argument(name) {
  const index = process.argv.indexOf(name);
  if (index < 0 || index + 1 >= process.argv.length) {
    throw new Error(`${name} is required`);
  }
  return process.argv[index + 1];
}

const port = Number.parseInt(argument("--port"), 10);
const root = path.resolve(argument("--dir"));
if (!Number.isInteger(port) || port <= 0 || port > 65535) {
  throw new Error("--port must be an integer from 1 through 65535");
}

const contentTypes = new Map([
  [".css", "text/css; charset=utf-8"],
  [".html", "text/html; charset=utf-8"],
  [".js", "text/javascript; charset=utf-8"],
  [".json", "application/json; charset=utf-8"],
  [".svg", "image/svg+xml"],
  [".wasm", "application/wasm"],
]);

function resolveRequestPath(requestUrl) {
  const pathname = decodeURIComponent(new URL(requestUrl, "http://127.0.0.1").pathname);
  const relativePath = pathname === "/" ? "index.html" : pathname.slice(1);
  const candidate = path.resolve(root, relativePath);
  if (candidate !== root && !candidate.startsWith(`${root}${path.sep}`)) {
    return null;
  }
  return candidate;
}

const server = createServer(async (request, response) => {
  response.setHeader("Cross-Origin-Embedder-Policy", "require-corp");
  response.setHeader("Cross-Origin-Opener-Policy", "same-origin");
  response.setHeader("Cache-Control", "no-store");

  if (request.method !== "GET" && request.method !== "HEAD") {
    response.writeHead(405, { Allow: "GET, HEAD" });
    response.end();
    return;
  }

  try {
    const filePath = resolveRequestPath(request.url || "/");
    if (!filePath || !(await stat(filePath)).isFile()) {
      response.writeHead(404);
      response.end("Not found\n");
      return;
    }

    response.writeHead(200, {
      "Content-Type": contentTypes.get(path.extname(filePath)) || "application/octet-stream",
    });
    if (request.method === "HEAD") {
      response.end();
      return;
    }
    await pipeline(createReadStream(filePath), response);
  } catch (error) {
    if (!response.headersSent) {
      response.writeHead(error?.code === "ENOENT" ? 404 : 500);
    }
    response.end(error?.code === "ENOENT" ? "Not found\n" : "Server error\n");
  }
});

server.listen(port, "127.0.0.1");
