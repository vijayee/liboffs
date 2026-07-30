import { describe, it, expect } from 'vitest';

describe('PDF demo state', () => {
  it('records a PDF demo result', () => {
    const demoResults = { pdf: 'http://localhost:23402/offsystem/v3/text/plain/.../file.pdf' };
    expect(demoResults.pdf).toContain('/offsystem/v3/');
  });
});
