import { describe, it, expect, beforeEach } from 'vitest';
import { renderSlide } from '../js/app.js';

describe('navigation', () => {
  beforeEach(() => {
    document.body.innerHTML = '<div id="slide-content"></div>';
  });

  it('renders the first slide', () => {
    renderSlide();
    expect(document.getElementById('slide-content').textContent).toContain('The OFF System');
  });
});
