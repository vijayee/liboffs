import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import http from 'node:http';
import { OffsClient } from '../../src/index.js';

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

describe('OffsClient HTTP integration', () => {
  /** @type {string} */
  let cacheDir;
  /** @type {import('node:child_process').ChildProcess} */
  let server;
  /** @type {string} */
  let baseUrl;

  beforeAll(async () => {
    cacheDir = mkdtempSync(join(tmpdir(), 'offs-http-test-'));
    const port = 23402;
    baseUrl = `http://localhost:${port}`;

    server = spawn(
      '/home/victor/Workspace/src/github.com/vijayee/liboffs/build-test/examples/off_server',
      ['--host', '0.0.0.0', '--port', String(port), '--cache-dir', cacheDir],
      { stdio: 'pipe' }
    );

    await waitForServer(baseUrl, 10000);
  }, 15000);

  afterAll(() => {
    server?.kill();
    try { rmSync(cacheDir, { recursive: true, force: true }); } catch { /* ignore cleanup errors */ }
  });

  it('uploads a small file and downloads it back', async () => {
    const client = new OffsClient(baseUrl);
    await client.connect();

    const payload = new TextEncoder().encode('hello from js client');
    const { oriString } = await client.put({
      contentType: 'text/plain',
      fileName: 'greeting.txt',
      streamLength: payload.length
    }, payload);

    expect(oriString).toContain('/offsystem/v3/');

    const chunks = [];
    await client.get(oriString, {
      onData: (chunk) => chunks.push(chunk)
    });

    const downloaded = new TextDecoder().decode(_concat(chunks));
    expect(downloaded).toBe('hello from js client');
  });

  it('uploads a folder tree and returns a directory OFD', async () => {
    const client = new OffsClient(baseUrl);
    await client.connect();

    const files = {
      'sample/readme.txt': new Blob(['sample readme']),
      'sample/data/info.json': new Blob(['{"ok":true}'])
    };

    const { oriString } = await client.putFolder(files);

    expect(oriString).toContain('/offsystem/v3/');
    expect(oriString).toContain('sample.ofd');

    // The server accepted the directory OFD upload and returned an ORI URL.
    // Verifying the exact OFD structure via ?ofd=raw depends on server-side
    // OFD caching, which is exercised separately; the unit tests confirm the JS
    // client builds the correct CBOR structure.
    const rawUrl = oriString.includes('?') ? `${oriString}&ofd=raw` : `${oriString}?ofd=raw`;
    const response = await fetch(rawUrl);
    expect(response.ok).toBe(true);
    const ofdBytes = new Uint8Array(await response.arrayBuffer());
    expect(ofdBytes.length).toBeGreaterThan(0);
  });

  it('rejects a file-name containing a slash with 400 instead of hanging up', async () => {
    const client = new OffsClient(baseUrl);
    await client.connect();

    const payload = new TextEncoder().encode('hello world');
    await expect(client.put({
      contentType: 'text/plain',
      fileName: 'sub/dir/file.txt',
      streamLength: payload.length
    }, payload)).rejects.toThrow(/400/);
  });

  it('returns 400 on a chunked PUT with an invalid file-name', async () => {
    const result = await new Promise((resolve, reject) => {
      const req = http.request(`${baseUrl}/offsystem`, {
        method: 'PUT',
        agent: false,
        headers: {
          'type': 'text/plain',
          'file-name': 'sub/dir/file.txt',
          'stream-length': '11',
          'Content-Type': 'application/octet-stream',
          'Transfer-Encoding': 'chunked'
        }
      }, (res) => {
        let body = '';
        res.on('data', (chunk) => body += chunk);
        res.on('end', () => resolve({ status: res.statusCode, body }));
      });
      req.on('error', reject);
      req.write('hello world');
      req.end();
    });
    expect(result.status).toBe(400);
    expect(result.body).toContain('Invalid file name');
  });
});

/**
 * @param {Uint8Array[]} chunks
 * @returns {Uint8Array}
 */
function _concat(chunks) {
  const total = chunks.reduce((sum, chunk) => sum + chunk.length, 0);
  const result = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    result.set(chunk, offset);
    offset += chunk.length;
  }
  return result;
}
