import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync, createReadStream, statSync } from 'node:fs';
import { Readable } from 'node:stream';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createHash } from 'node:crypto';
import { WebSocket } from 'ws';
import { OffsClient } from '../../src/index.js';

global.WebSocket = WebSocket;

const LARGE_VIDEO = '/home/victor/Videos/Big Hero 6 2014 1080p/Big.Hero.6.2014.1080p.BluRay.x264.YIFY.mp4';

/**
 * @param {string} baseUrl
 * @param {number} timeoutMs
 */
async function waitForServer(baseUrl, timeoutMs = 10000) {
  const deadline = Date.now() + timeoutMs;
  const client = new OffsClient(baseUrl);
  await client.connect();
  while (Date.now() < deadline) {
    try {
      await client.health();
      return;
    } catch {
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
  }
  throw new Error(`Server did not become ready at ${baseUrl}`);
}

/**
 * @param {string} path
 * @returns {Promise<string>}
 */
function sha256File(path) {
  return new Promise((resolve, reject) => {
    const hash = createHash('sha256');
    const stream = createReadStream(path);
    stream.on('data', (chunk) => hash.update(chunk));
    stream.on('end', () => resolve(hash.digest('hex')));
    stream.on('error', reject);
  });
}

/**
 * @param {string} path
 * @param {number} chunkSize
 * @returns {ReadableStream<Uint8Array>}
 */
function fileToWebStream(path, chunkSize = 64 * 1024) {
  const nodeStream = createReadStream(path, { highWaterMark: chunkSize });
  return Readable.toWeb(nodeStream);
}

describe('Large file round-trip', () => {
  /** @type {string} */
  let cacheDir;
  /** @type {import('node:child_process').ChildProcess} */
  let server;
  /** @type {string} */
  let httpUrl;
  /** @type {string} */
  let wsUrl;
  /** @type {string} */
  let expectedHash;

  beforeAll(async () => {
    cacheDir = mkdtempSync(join(tmpdir(), 'offs-large-test-'));
    httpUrl = 'http://localhost:23402';
    wsUrl = 'ws://localhost:23414';

    server = spawn(
      '/home/victor/Workspace/src/github.com/vijayee/liboffs/build-test/examples/off_server',
      ['--host', '0.0.0.0', '--port', '23402', '--ws-port', '23414', '--cache-dir', cacheDir],
      { stdio: 'pipe' }
    );

    await waitForServer(httpUrl, 10000);
    expectedHash = await sha256File(LARGE_VIDEO);
  }, 60000);

  afterAll(() => {
    server?.kill();
    try { rmSync(cacheDir, { recursive: true, force: true }); } catch { /* ignore cleanup errors */ }
  });

  it('streams the large video up and down over HTTP', async () => {
    const client = new OffsClient(httpUrl);
    await client.connect();

    const stream = fileToWebStream(LARGE_VIDEO);

    const { oriString } = await client.put({
      contentType: 'video/mp4',
      fileName: 'Big.Hero.6.2014.1080p.BluRay.x264.YIFY.mp4',
      streamLength: statSync(LARGE_VIDEO).size
    }, stream);

    expect(oriString).toContain('/offsystem/v3/');

    const downloadHash = createHash('sha256');
    let downloadedBytes = 0;
    await client.get(oriString, {
      onData: (chunk) => {
        downloadedBytes += chunk.length;
        downloadHash.update(chunk);
      }
    });

    expect(downloadedBytes).toBeGreaterThan(0);
    expect(downloadHash.digest('hex')).toBe(expectedHash);
  }, 600000);

  it('streams the large video up and down over WebSocket', async () => {
    const client = new OffsClient(wsUrl, undefined, { requestTimeoutMs: 600000 });
    await client.connect();

    await client.putStreamStart({
      contentType: 'video/mp4',
      fileName: 'Big.Hero.6.2014.1080p.BluRay.x264.YIFY.mp4',
      streamLength: statSync(LARGE_VIDEO).size
    });

    const nodeStream = createReadStream(LARGE_VIDEO, { highWaterMark: 64 * 1024 });
    for await (const chunk of nodeStream) {
      await client.putStreamData(chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk));
    }

    const { oriString } = await client.putStreamEnd();
    expect(oriString).toContain('/offsystem/v3/');

    const downloadHash = createHash('sha256');
    let downloadedBytes = 0;
    await client.get(oriString, {
      onData: (chunk) => {
        downloadedBytes += chunk.length;
        downloadHash.update(chunk);
      }
    });

    expect(downloadedBytes).toBeGreaterThan(0);
    expect(downloadHash.digest('hex')).toBe(expectedHash);
  }, 600000);
});
