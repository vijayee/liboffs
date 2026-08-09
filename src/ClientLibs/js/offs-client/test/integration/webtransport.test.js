import { describe, it, expect } from 'vitest';

describe('OffsClient WebTransport integration', () => {
  it.skip('uploads a small file and downloads it back over HTTP/3 WebTransport', () => {
    // HTTP/3 WebTransport requires a TLS certificate on the server and a
    // browser/WebTransport-capable runtime. The C webtransport_h3 endpoint is
    // wired into off_server/offsd on --wt-h3-port; this test is skipped until
    // a headless browser or Node WebTransport runtime is integrated.
    expect(true).toBe(true);
  });
});
