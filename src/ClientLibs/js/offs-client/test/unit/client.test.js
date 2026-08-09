import { describe, it, expect, vi } from 'vitest';
import { OffsClient } from '../../src/index.js';
import { HttpTransport } from '../../src/transports/http-transport.js';
import { base58Encode } from '../../src/util.js';
import { parseOfdCbor } from '../../src/ofd.js';

/**
 * Build a fake OFFS URL for a file/directory name and size.
 * Uses base58-encoded name as both hashes so tests can verify them.
 * @param {string} name
 * @param {number} length
 * @param {string} [contentType='application/octet-stream']
 * @returns {string}
 */
function fakeOffUrl(name, length, contentType = 'application/octet-stream') {
  const typeSlug = contentType.replace(/\//g, '_');
  const hash = base58Encode(new TextEncoder().encode(name));
  return `http://localhost:23402/offsystem/v3/${typeSlug}/${length}/${hash}/${hash}/${encodeURIComponent(name)}`;
}

/**
 * @returns {HttpTransport & {calls: any[]}}
 */
function createMockTransport() {
  const transport = new HttpTransport('http://localhost:23402');
  transport.calls = [];
  transport.put = vi.fn(async (options, body) => {
    let bodyBytes;
    if (body instanceof Uint8Array) {
      bodyBytes = body;
    } else if (body && typeof body.getReader === 'function') {
      // Browser File streams are not consumed in Node unit tests because
      // FileReader is unavailable. Record a marker instead.
      bodyBytes = null;
    } else {
      bodyBytes = new Uint8Array(0);
    }
    transport.calls.push({ method: 'put', options, body: bodyBytes });
    const url = fakeOffUrl(options.fileName, options.streamLength, options.contentType);
    return { oriString: url };
  });
  return transport;
}

describe('OffsClient putFolder', () => {
  it('uploads a flat folder as an OFD', async () => {
    const transport = createMockTransport();
    const client = new OffsClient('http://localhost:23402', undefined, { transport });

    const files = {
      'my-folder/file-a.txt': new Blob(['AAAA']),
      'my-folder/file-b.txt': new Blob(['BBB'])
    };

    const result = await client.putFolder(files);

    // Two files plus one directory OFD were uploaded.
    expect(transport.calls).toHaveLength(3);

    const fileCalls = transport.calls.filter((call) => call.options.contentType !== 'offsystem/directory');
    expect(fileCalls).toHaveLength(2);

    const ofdCall = transport.calls.find((call) => call.options.contentType === 'offsystem/directory');
    expect(ofdCall).toBeDefined();
    expect(ofdCall.options.fileName).toBe('my-folder.ofd');

    const entries = parseOfdCbor(ofdCall.body);
    expect(entries).toHaveLength(2);
    expect(entries.map((entry) => entry.name).sort()).toEqual(['file-a.txt', 'file-b.txt']);
    expect(entries.every((entry) => !entry.isDirectory)).toBe(true);

    // Result URL points at the directory OFD.
    expect(result.oriString).toContain('/offsystem/v3/');
    expect(result.oriString).toContain('my-folder.ofd');
  });

  it('recursively uploads nested directories', async () => {
    const transport = createMockTransport();
    const client = new OffsClient('http://localhost:23402', undefined, { transport });

    const files = {
      'root/top.txt': new Blob(['top']),
      'root/sub/nested.txt': new Blob(['nested'])
    };

    const result = await client.putFolder(files);
    expect(result.oriString).toContain('root.ofd');

    // top.txt, nested.txt, sub.ofd, root.ofd
    expect(transport.calls).toHaveLength(4);

    const ofdCalls = transport.calls.filter((call) => call.options.contentType === 'offsystem/directory');
    expect(ofdCalls).toHaveLength(2);

    const subOfd = ofdCalls.find((call) => call.options.fileName === 'sub.ofd');
    expect(subOfd).toBeDefined();
    const subEntries = parseOfdCbor(subOfd.body);
    expect(subEntries).toHaveLength(1);
    expect(subEntries[0].name).toBe('nested.txt');

    const rootOfd = ofdCalls.find((call) => call.options.fileName === 'root.ofd');
    expect(rootOfd).toBeDefined();
    const rootEntries = parseOfdCbor(rootOfd.body);
    expect(rootEntries).toHaveLength(2);
    const dirEntry = rootEntries.find((entry) => entry.isDirectory);
    expect(dirEntry).toBeDefined();
    expect(dirEntry.name).toBe('sub');
    const fileEntry = rootEntries.find((entry) => !entry.isDirectory);
    expect(fileEntry.name).toBe('top.txt');
  });

  it('reports progress for each file', async () => {
    const transport = createMockTransport();
    const client = new OffsClient('http://localhost:23402', undefined, { transport });

    const progress = [];
    const files = {
      'folder/a.txt': new Blob(['a']),
      'folder/b.txt': new Blob(['b']),
      'folder/c.txt': new Blob(['c'])
    };

    await client.putFolder(files, {
      onProgress: (name, uploaded, total) => progress.push({ name, uploaded, total })
    });

    expect(progress).toHaveLength(3);
    expect(progress[0].uploaded).toBe(1);
    expect(progress[2].uploaded).toBe(3);
    expect(progress.every((p) => p.total === 3)).toBe(true);
  });

  it('throws for empty input', async () => {
    const transport = createMockTransport();
    const client = new OffsClient('http://localhost:23402', undefined, { transport });

    await expect(client.putFolder({})).rejects.toThrow('No files to upload');
  });
});
