const slides = [
  {
    title: 'The OFF System',
    subtitle: 'Decentralized, content-addressed storage with OFF streams',
    type: 'title',
    fragments: []
  },
  {
    title: 'What is OFFS?',
    type: 'content',
    fragments: [
      '<p>The OFF System splits files into fixed-size, content-addressed blocks.</p>',
      '<p>Blocks are obfuscated and reassembled on demand using descriptors and ORIs.</p>',
      '<p>No central authority knows what any node is holding.</p>'
    ]
  },
  {
    title: 'Core Data Model',
    type: 'content',
    fragments: [
      '<ul>' +
      '<li><strong>Block</strong>: a fixed-size chunk of transformed data.</li>' +
      '<li><strong>Descriptor</strong>: the recipe that turns blocks back into a file.</li>' +
      '<li><strong>ORI</strong>: an OFF Resource Identifier (URL) pointing to a descriptor.</li>' +
      '<li><strong>OFD</strong>: an OFF File Descriptor for directories and archives.</li>' +
      '</ul>'
    ]
  },
  {
    title: 'Architecture',
    type: 'content',
    fragments: [
      '<ul>' +
      '<li><strong>BlockCache</strong>: fixed-size block storage, LRU, index, sections.</li>' +
      '<li><strong>OFFStreams</strong>: ORI/OFD/tuple encoding and stream descriptors.</li>' +
      '<li><strong>Network</strong>: QUIC/P2P, gossip, relay, peer discovery.</li>' +
      '<li><strong>ClientAPI</strong>: HTTP, Unix, TCP, WebSocket, WebTransport servers.</li>' +
      '<li><strong>Actor/Scheduler</strong>: async actor system and timing.</li>' +
      '</ul>'
    ]
  },
  {
    title: 'Network',
    type: 'content',
    fragments: [
      '<p>Direct peer connections over QUIC for same-LAN fast path.</p>',
      '<p>Gossip and relay support NAT traversal and multi-hop lookups.</p>',
      '<p>Content is fetched by block hash, not by server identity.</p>'
    ]
  },
  {
    title: 'Security & Trust',
    type: 'content',
    fragments: [
      '<p>Transport-level TLS and optional CA-based client certificates.</p>',
      '<p>Admin endpoints use bcrypt-secured API keys.</p>',
      '<p>Content is anonymized through the OFF block transform.</p>'
    ]
  },
  {
    title: 'Client Libraries',
    type: 'content',
    fragments: [
      '<ul>' +
      '<li><strong>C client</strong>: unix://, tcp://, ws://, wss://, wt://, wts://.</li>' +
      '<li><strong>Flutter example</strong>: uses the HTTP REST API.</li>' +
      '<li><strong>JavaScript client</strong>: browser-only, supports HTTP, WebSocket, and WebTransport.</li>' +
      '</ul>'
    ]
  }
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
  const fragmentHtml = (slide.fragments || [])
    .map((html, index) => `<div class="fragment ${index < fragmentIndex ? 'visible' : ''}" data-index="${index}">${html}</div>`)
    .join('');
  slideContent.innerHTML = `
    <div class="slide-content">
      ${slide.subtitle ? `<h3>${slide.subtitle}</h3>` : ''}
      <h2>${slide.title}</h2>
      ${fragmentHtml}
    </div>
  `;
  void slideContent.offsetWidth;
  updateProgress();
}

function next() {
  const slide = slides[current];
  const totalFragments = slide.fragments ? slide.fragments.length : 0;
  if (fragmentIndex < totalFragments) {
    fragmentIndex++;
    renderSlide();
    return;
  }
  if (current < slides.length - 1) {
    current++;
    fragmentIndex = 0;
    renderSlide();
  }
}

function previous() {
  if (fragmentIndex > 0) {
    fragmentIndex--;
    renderSlide();
    return;
  }
  if (current > 0) {
    current--;
    const prevSlide = slides[current];
    fragmentIndex = prevSlide.fragments ? prevSlide.fragments.length : 0;
    renderSlide();
  }
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
