import { describe, it, expect, beforeEach } from 'vitest';
import { renderSlide } from '../js/app.js';

describe('PDF demo state', () => {
  beforeEach(() => {
    document.body.innerHTML = `
      <div class="deck">
        <main id="slide-content" class="slide"></main>
        <div class="progress"><div id="progress-fill"></div></div>
        <span id="slide-counter">1 / 9</span>
        <button id="btn-prev"></button>
        <button id="btn-next"></button>
      </div>
    `;
  });

  it('loads the slide deck without the OffsClient global', () => {
    expect(() => renderSlide()).not.toThrow();
    const content = document.getElementById('slide-content').textContent;
    expect(content).toContain('OFF System');
  });
});
