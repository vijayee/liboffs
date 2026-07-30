const slides = [];
let current = 0;
function renderSlide() {
  const container = document.getElementById('slide-content');
  container.textContent = slides[current]?.title || '';
}
function nextSlide() { if (current < slides.length - 1) { current++; renderSlide(); } }
function prevSlide() { if (current > 0) { current--; renderSlide(); } }
export { renderSlide, nextSlide, prevSlide };
