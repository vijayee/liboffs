import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { WebSocket } from 'ws';
import { OffsClient } from '../../src/index.js';

global.WebSocket = WebSocket;

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

describe('OffsClient WebSocket integration', () => {
  /** @type {string} */
  let cacheDir;
  /** @type {import('node:child_process').ChildProcess} */
  let server;
  /** @type {string} */
  let httpUrl;
  /** @type {string} */
  let wsUrl;

  beforeAll(async () => {
    cacheDir = mkdtempSync(join(tmpdir(), 'offs-ws-test-'));
    httpUrl = 'http://localhost:23412';
    wsUrl = 'ws://localhost:23413';

    server = spawn(
      '/home/victor/Workspace/src/github.com/vijayee/liboffs/build-test/examples/off_server',
      ['--host', '0.0.0.0', '--port', '23412', '--ws-port', '23413', '--cache-dir', cacheDir],
      { stdio: 'pipe' }
    );

    await waitForServer(httpUrl, 10000);
  }, 15000);

  afterAll(() => {
    server?.kill();
    try { rmSync(cacheDir, { recursive: true, force: true }); } catch { /* ignore cleanup errors */ }
  });

  it('uploads a small file and downloads it back over WebSocket', async () => {
    await new Promise((resolve, reject) => {
      const probe = new WebSocket(wsUrl);
      probe.onopen = () => { probe.close(); resolve(); };
      probe.onerror = () => reject(new Error('probe failed'));
    });
    const client = new OffsClient(wsUrl);
    await client.connect();

    const payload = new TextEncoder().encode('hello from websocket');
    const { oriString } = await client.put({
      contentType: 'text/plain',
      fileName: 'ws-greeting.txt',
      streamLength: payload.length
    }, payload);

    expect(oriString).toContain('/offsystem/v3/');

    const chunks = [];
    await client.get(oriString, {
      onData: (chunk) => chunks.push(chunk)
    });

    const downloaded = new TextDecoder().decode(_concat(chunks));
    expect(downloaded).toBe('hello from websocket');
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
