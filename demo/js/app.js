const slides = [
  { title: 'The OFF System', type: 'title', fragments: [] }
];

let current = 0;
let fragmentIndex = 0;

function getSlideContent() {
  return document.getElementById('slide-content');
}

function getProgressFill() {
  return document.getElementById('progress-fill');
}

function getSlideCounter() {
  return document.getElementById('slide-counter');
}

function getButtonPrev() {
  return document.getElementById('btn-prev');
}

function getButtonNext() {
  return document.getElementById('btn-next');
}

function updateProgress() {
  const progressFill = getProgressFill();
  const slideCounter = getSlideCounter();
  if (!progressFill || !slideCounter) return;
  const pct = ((current + 1) / slides.length) * 100;
  progressFill.style.width = `${pct}%`;
  slideCounter.textContent = `${current + 1} / ${slides.length}`;
}

function renderSlide() {
  const slideContent = getSlideContent();
  if (!slideContent) return;
  const slide = slides[current];
  slideContent.className = 'slide slide-transition-enter';
  slideContent.innerHTML = `
    <div class="slide-content">
      <h2>${slide.title}</h2>
    </div>
  `;
  void slideContent.offsetWidth;
  updateProgress();
}

function next() {
  if (current >= slides.length - 1) return;
  current++;
  fragmentIndex = 0;
  renderSlide();
}

function previous() {
  if (current <= 0) return;
  current--;
  fragmentIndex = 0;
  renderSlide();
}

function initDeck() {
  const slideContent = getSlideContent();
  const btnPrev = getButtonPrev();
  const btnNext = getButtonNext();
  if (!slideContent) return;
  btnPrev?.addEventListener('click', previous);
  btnNext?.addEventListener('click', next);
  document.addEventListener('keydown', (event) => {
    if (event.key === 'ArrowRight' || event.key === ' ') {
      event.preventDefault();
      next();
    }
    if (event.key === 'ArrowLeft') {
      event.preventDefault();
      previous();
    }
  });
  renderSlide();
}

if (typeof document !== 'undefined') {
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', initDeck);
  } else {
    initDeck();
  }
}

export { renderSlide };
