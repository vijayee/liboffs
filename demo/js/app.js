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
  },
  {
    title: 'Demo 1: Upload a PDF',
    type: 'demo',
    demoType: 'pdf',
    fragments: [
      '<p>Select a PDF file and upload it to the OFFS daemon.</p>'
    ]
  },
  {
    title: 'Demo 1 Result',
    type: 'result',
    demoType: 'pdf',
    fragments: []
  },
  {
    title: 'Demo 2: Upload a Video',
    type: 'demo',
    demoType: 'video',
    fragments: [
      '<p>Select a video file and upload it to the OFFS daemon.</p>'
    ]
  },
  {
    title: 'Demo 2 Result',
    type: 'result',
    demoType: 'video',
    fragments: []
  },
  {
    title: 'Demo 3: Upload a Static Site',
    type: 'demo',
    demoType: 'site',
    fragments: [
      '<p>Select a folder containing a static site with index.html.</p>'
    ]
  },
  {
    title: 'Demo 3 Result',
    type: 'result',
    demoType: 'site',
    fragments: []
  },
  {
    title: 'Contact & Code',
    type: 'content',
    fragments: [
      '<p><strong>GitHub:</strong> <a href="https://github.com/Prometheus-SCN/OFFS" target="_blank">Prometheus-SCN/OFFS</a></p>',
      '<p><strong>Contact:</strong> Victor Morrow &lt;victor.j.morrow@gmail.com&gt;</p>',
      '<p>Thank you.</p>'
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

const client = typeof OffsClient !== 'undefined' ? new OffsClient('http://localhost:23402') : null;
const demoResults = { pdf: null, video: null, site: null };

function prepareSiteEntries(fileInput) {
  const files = Array.from(fileInput.files || []);
  const entries = {};
  for (const file of files) {
    const path = file.webkitRelativePath || file.name;
    entries[path] = file;
  }
  return entries;
}

function updateProgress() {
  const progressFill = getProgressFill();
  const slideCounter = getSlideCounter();
  if (!progressFill || !slideCounter) return;
  const pct = ((current + 1) / slides.length) * 100;
  progressFill.style.width = `${pct}%`;
  slideCounter.textContent = `${current + 1} / ${slides.length}`;
}

function wireDemoControls(slide) {
  const fileInput = document.getElementById('demo-file');
  const uploadBtn = document.getElementById('demo-upload');
  const status = document.getElementById('demo-status');
  if (!fileInput || !uploadBtn || !status) return;

  if (!client) {
    status.textContent = 'OFFS client library is not loaded. Make sure offs-client.umd.js is available.';
    status.className = 'status error';
    uploadBtn.disabled = false;
    return;
  }

  fileInput.addEventListener('change', () => {
    uploadBtn.disabled = !fileInput.files || fileInput.files.length === 0;
  });

  uploadBtn.addEventListener('click', async () => {
    uploadBtn.disabled = true;
    status.textContent = 'Uploading…';
    status.className = 'status';
    try {
      await client.connect();
      let oriString;
      if (slide.demoType === 'site') {
        const entries = prepareSiteEntries(fileInput);
        const result = await client.putFolder(entries, {
          onProgress: (name, uploaded, total) => {
            status.textContent = `Uploading ${uploaded}/${total}: ${name}`;
          }
        });
        oriString = result.oriString;
      } else {
        const file = fileInput.files[0];
        const arrayBuffer = await file.arrayBuffer();
        const data = new Uint8Array(arrayBuffer);
        const result = await client.put({
          contentType: file.type || 'application/octet-stream',
          fileName: file.name,
          streamLength: data.length
        }, data);
        oriString = result.oriString;
      }
      demoResults[slide.demoType] = oriString;
      status.textContent = 'Upload complete. Advancing to result…';
      status.className = 'status success';
      setTimeout(next, 1200);
    } catch (err) {
      status.textContent = `Error: ${err.message}`;
      status.className = 'status error';
      uploadBtn.disabled = false;
    }
  });
}

function renderSlide() {
  const slideContent = getSlideContent();
  if (!slideContent) return;
  const slide = slides[current];

  if (slide.type === 'result') {
    const ori = demoResults[slide.demoType];
    slideContent.className = 'slide slide-transition-enter';
    slideContent.innerHTML = `
      <div class="slide-content" style="display:flex;flex-direction:column;height:100%">
        <h2>${slide.title}</h2>
        ${ori ?
          `<div class="ori-url">${ori}</div>
           <div class="iframe-wrapper"><iframe src="${ori}" title="${slide.demoType} result"></iframe></div>` :
          '<p class="status error">No upload result available. Go back and run the demo first.</p>'}
      </div>
    `;
    void slideContent.offsetWidth;
    updateProgress();
    return;
  }

  slideContent.className = 'slide slide-transition-enter';
  const fragmentHtml = (slide.fragments || [])
    .map((html, index) => `<div class="fragment ${index < fragmentIndex ? 'visible' : ''}" data-index="${index}">${html}</div>`)
    .join('');

  let demoHtml = '';
  if (slide.type === 'demo') {
    const accept =
      slide.demoType === 'pdf' ? '.pdf' :
      slide.demoType === 'video' ? 'video/*' : '';
    const directoryAttr = slide.demoType === 'site' ? 'webkitdirectory directory' : '';
    demoHtml = `
      <div class="demo-controls">
        <input type="file" id="demo-file" accept="${accept}" ${directoryAttr}>
        <button id="demo-upload" disabled>Upload to OFFS</button>
        <div id="demo-status" class="status"></div>
      </div>
    `;
  }

  slideContent.innerHTML = `
    <div class="slide-content">
      ${slide.subtitle ? `<h3>${slide.subtitle}</h3>` : ''}
      <h2>${slide.title}</h2>
      ${fragmentHtml}
      ${demoHtml}
    </div>
  `;
  void slideContent.offsetWidth;
  updateProgress();

  if (slide.type === 'demo') {
    wireDemoControls(slide);
  }
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
