/**
 * Bitcoin-style Base58 encoding/decoding.
 * Matches the C implementation in liboffs/src/Util/base58.c.
 */
const ALPHABET = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz';

/** @type {Int8Array} */
const INDICES = new Int8Array(128);
INDICES.fill(-1);
for (let index = 0; index < ALPHABET.length; index++) {
  INDICES[ALPHABET.charCodeAt(index)] = index;
}

/**
 * Decode a base58 string into a Uint8Array.
 * @param {string} input
 * @returns {Uint8Array|null}
 */
export function base58Decode(input) {
  if (input.length === 0) return null;

  let leadingZeros = 0;
  while (leadingZeros < input.length && input[leadingZeros] === '1') {
    leadingZeros++;
  }

  const bytes = [];
  for (let index = leadingZeros; index < input.length; index++) {
    const codeUnit = input.charCodeAt(index);
    if (codeUnit >= 128) return null;
    const digit = INDICES[codeUnit];
    if (digit < 0) return null;

    let carry = digit;
    for (let byteIndex = 0; byteIndex < bytes.length; byteIndex++) {
      carry += bytes[byteIndex] * 58;
      bytes[byteIndex] = carry & 0xff;
      carry >>= 8;
    }
    while (carry > 0) {
      bytes.push(carry & 0xff);
      carry >>= 8;
    }
  }

  for (let index = 0; index < leadingZeros; index++) {
    bytes.push(0);
  }

  bytes.reverse();
  return new Uint8Array(bytes);
}

/**
 * Encode a Uint8Array into a base58 string.
 * @param {Uint8Array|number[]} input
 * @returns {string}
 */
export function base58Encode(input) {
  if (input.length === 0) return '';

  const bytes = Array.from(input);
  let leadingZeros = 0;
  while (leadingZeros < bytes.length && bytes[leadingZeros] === 0) {
    leadingZeros++;
  }

  const resultCodes = [];
  for (let index = leadingZeros; index < bytes.length; index++) {
    let carry = bytes[index];
    for (let resultIndex = 0; resultIndex < resultCodes.length; resultIndex++) {
      carry += resultCodes[resultIndex] * 256;
      resultCodes[resultIndex] = carry % 58;
      carry = Math.floor(carry / 58);
    }
    while (carry > 0) {
      resultCodes.push(carry % 58);
      carry = Math.floor(carry / 58);
    }
  }

  const prefix = '1'.repeat(leadingZeros);
  return prefix + resultCodes.reverse().map((code) => ALPHABET[code]).join('');
}

/**
 * Parsed OFFS URL components.
 * @typedef {Object} ParsedOffUrl
 * @property {string} fileHashB58
 * @property {string} descriptorHashB58
 * @property {number} streamLength
 * @property {string} fileName
 */

/**
 * Parse an offs:// or http(s) OFFS URL.
 * Format: .../offsystem/v3/{type}/{length}/{hash1}/{hash2}/{name}
 * @param {string} url
 * @returns {ParsedOffUrl|null}
 */
export function parseOffUrl(url) {
  const prefixIndex = url.indexOf('/offsystem/v3/');
  if (prefixIndex < 0) return null;

  const afterPrefix = url.slice(prefixIndex + '/offsystem/v3/'.length);
  const allParts = afterPrefix.split('/');
  if (allParts.length < 4) return null;

  const streamLengthStr = allParts[allParts.length - 4];
  const fileHashB58 = allParts[allParts.length - 3];
  const descriptorHashB58 = allParts[allParts.length - 2];
  const fileName = allParts.slice(allParts.length - 1).join('/');

  const streamLength = parseInt(streamLengthStr, 10);
  if (!Number.isFinite(streamLength)) return null;
  if (base58Decode(fileHashB58) === null) return null;
  if (base58Decode(descriptorHashB58) === null) return null;

  return {
    fileHashB58,
    descriptorHashB58,
    streamLength,
    fileName: decodeURIComponent(fileName)
  };
}

/**
 * Guess a MIME type from a filename extension.
 * @param {string} filename
 * @returns {string}
 */
export function mimeFromExtension(filename) {
  const map = {
    html: 'text/html',
    htm: 'text/html',
    css: 'text/css',
    js: 'application/javascript',
    json: 'application/json',
    png: 'image/png',
    jpg: 'image/jpeg',
    jpeg: 'image/jpeg',
    gif: 'image/gif',
    svg: 'image/svg+xml',
    ico: 'image/x-icon',
    webp: 'image/webp',
    bmp: 'image/bmp',
    tiff: 'image/tiff',
    tif: 'image/tiff',
    mp4: 'video/mp4',
    webm: 'video/webm',
    mkv: 'video/x-matroska',
    avi: 'video/x-msvideo',
    mov: 'video/quicktime',
    wmv: 'video/x-msvideo',
    flv: 'video/x-flv',
    mp3: 'audio/mpeg',
    ogg: 'audio/ogg',
    wav: 'audio/wav',
    flac: 'audio/flac',
    aac: 'audio/mp4',
    m4a: 'audio/mp4',
    woff: 'font/woff',
    woff2: 'font/woff2',
    ttf: 'font/ttf',
    otf: 'font/otf',
    pdf: 'application/pdf',
    zip: 'application/zip',
    gz: 'application/gzip',
    tar: 'application/x-tar',
    rar: 'application/vnd.rar',
    '7z': 'application/x-7z-compressed',
    doc: 'application/msword',
    docx: 'application/vnd.openxmlformats-officedocument.wordprocessingml.document',
    xls: 'application/vnd.ms-excel',
    xlsx: 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
    ppt: 'application/vnd.ms-powerpoint',
    pptx: 'application/vnd.openxmlformats-officedocument.presentationml.presentation',
    txt: 'text/plain',
    csv: 'text/csv',
    xml: 'application/xml',
    md: 'text/markdown',
    ofd: 'application/cbor'
  };
  const dotIndex = filename.lastIndexOf('.');
  if (dotIndex < 0 || dotIndex === filename.length - 1) return 'application/octet-stream';
  const extension = filename.slice(dotIndex + 1).toLowerCase();
  return map[extension] || 'application/octet-stream';
}

/**
 * Read a browser File or Blob into a Uint8Array.
 * @param {Blob} file
 * @returns {Promise<Uint8Array>}
 */
export function readFileBytes(file) {
  if (typeof file.arrayBuffer === 'function') {
    return file.arrayBuffer().then((buffer) => new Uint8Array(buffer));
  }
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(new Uint8Array(reader.result));
    reader.onerror = () => reject(reader.error);
    reader.readAsArrayBuffer(file);
  });
}

/**
 * Return the last path segment, stripping any directory separators or parent
 * references so the value is safe to use as a `file-name` header.
 * @param {string} path
 * @returns {string}
 */
export function basename(path) {
  const normalized = path.replace(/\\\\/g, '/');
  const parts = normalized.split('/').filter(Boolean);
  return parts.length > 0 ? parts[parts.length - 1] : 'file';
}

/**
 * Create a ReadableStream from a browser File.
 * @param {File} file
 * @param {number} [chunkSize=65536]
 * @returns {ReadableStream<Uint8Array>}
 */
export function fileToReadableStream(file, chunkSize = 65536) {
  let offset = 0;
  return new ReadableStream({
    pull(controller) {
      if (offset >= file.size) {
        controller.close();
        return;
      }
      const end = Math.min(offset + chunkSize, file.size);
      const slice = file.slice(offset, end);
      return readFileBytes(slice).then((bytes) => {
        controller.enqueue(bytes);
        offset = end;
      });
    }
  });
}

/**
 * A file-like entry with a relative path.
 * @typedef {Object} FolderEntry
 * @property {string} path
 * @property {File|Blob} file
 */

/**
 * Normalize various folder input shapes into a flat list of {path, file}.
 * Accepts FileList (from <input webkitdirectory>), Array<File>,
 * Array<{path, file}>, or Record<string, File|Blob>.
 * @param {FileList|File[]|FolderEntry[]|Record<string, File|Blob>} items
 * @returns {FolderEntry[]}
 */
export function normalizeFolderEntries(items) {
  if (typeof FileList !== 'undefined' && items instanceof FileList) {
    const entries = [];
    for (let index = 0; index < items.length; index++) {
      const file = items[index];
      /** @type {string} */
      let path = file.webkitRelativePath || file.name;
      entries.push({ path, file });
    }
    return entries;
  }

  if (Array.isArray(items)) {
    return items.map((item) => {
      if (item instanceof File || item instanceof Blob) {
        return { path: item.webkitRelativePath || item.name, file: item };
      }
      return { path: item.path, file: item.file };
    });
  }

  return Object.entries(items).map(([path, file]) => ({ path, file }));
}

/**
 * Ensure an OFF URL points at an HTTP endpoint so a browser can fetch it.
 * @param {string} oriString
 * @param {string} [baseUrl='http://localhost:23402']
 * @returns {string}
 */
export function offUrlToHttpUrl(oriString, baseUrl = 'http://localhost:23402') {
  if (!oriString) return oriString;
  if (/^https?:\/\//i.test(oriString)) return oriString;

  let path = oriString;
  if (path.startsWith('offs://')) {
    path = path.slice('offs://'.length);
  }

  const prefix = '/offsystem/v3/';
  const index = path.indexOf(prefix);
  if (index >= 0) {
    path = path.slice(index);
  }

  if (path.startsWith(prefix)) {
    const base = baseUrl.replace(/\/$/, '');
    return `${base}${path}`;
  }

  return oriString;
}
