import { describe, it, expect } from 'vitest';
import { OffsClient } from '../../src/index.js';

describe('OffsClient smoke', () => {
  it('can be constructed for each supported scheme', () => {
    expect(() => new OffsClient('http://localhost:23402')).not.toThrow();
    expect(() => new OffsClient('https://localhost:23402')).not.toThrow();
    expect(() => new OffsClient('ws://localhost:23402')).not.toThrow();
    expect(() => new OffsClient('wss://localhost:23402')).not.toThrow();
    expect(() => new OffsClient('wt://localhost:23402')).not.toThrow();
    expect(() => new OffsClient('wts://localhost:23402')).not.toThrow();
  });
});
