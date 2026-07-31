const slides = [
  {
    title: 'The OFF System',
    subtitle: 'Owner-Free File System — a brightnet for content-addressed storage',
    type: 'title',
    fragments: []
  },
  {
    title: 'What is OFFS?',
    type: 'content',
    fragments: [
      '<p>The <strong>Owner Free File System</strong> (OFF System)(OFFS) is the world\'s first "brightnet". It facilitates legal data sharing.</p>',
      '<p>The storage mechanism is unique in that it never stores whole files but instead stores completely random data blocks which contain large randomly generated numbers.</p>',
      '<p>These blocks have no discrete mapping to any single file but instead are shared by infinite combinations of data.</p>'
    ]
  },
  {
    title: 'A brief history',
    type: 'content',
    fragments: [
      '<p>The original Owner Free File System was created by the hacktivist organization Big Hack in 2003.</p>',
      '<p>Since then it has been maintained by Prometheus, the social currency network.</p>',
      '<p>It is a keystone implementation in the Earth Services model, representing layer one storage.</p>'
    ]
  },
  {
    title: 'Core data model',
    type: 'content',
    fragments: [
      '<p>Data lives in fixed-size blocks: Mega (1 MB), Standard (128 KB), Mini (64 KB), and Nano (136 bytes).</p>',
      '<p>Every block is identified by its BLAKE3 hash. Identical blocks collapse to one physical copy and are referenced many times.</p>',
      '<p>An <strong>ORI</strong> records the descriptor hash, block type, tuple size, file offset, and final byte count. Its string form is the OFF URL people pass around.</p>',
      '<p>A <strong>tuple</strong> is the ordered list of block hashes. Writing XORs source blocks with randomizer blocks; reading fetches them and XORs back.</p>',
      '<p><strong>OFDs</strong> (OFF File Directories) map names to file ORIs or nested OFD hashes, so one OFF URL can name an entire directory tree.</p>'
    ]
  },
  {
    title: 'Architecture layers',
    type: 'content',
    fragments: [
      '<ul>' +
      '<li><strong>BlockCache</strong> — fixed-size block storage with LRU, index, section files, and a write-ahead log.</li>' +
      '<li><strong>OFFStreams</strong> — readable/writeable descriptors, tuple encoding, ORI/OFD handling.</li>' +
      '<li><strong>Network</strong> — QUIC peer connections, gossip rings, EABF block routing, relay-assisted NAT traversal.</li>' +
      '<li><strong>ClientAPI</strong> — HTTP, Unix socket, TCP, WebSocket, and WebTransport servers.</li>' +
      '<li><strong>Actor / Scheduler / Timer</strong> — async actor runtime that makes every put/get a promise.</li>' +
      '</ul>'
    ]
  },
  {
    title: 'The network layer',
    type: 'content',
    fragments: [
      '<p>Each node has a stable identity derived from its public key. The authority subsystem signs salutation messages and loads the node certificate.</p>',
      '<p>Peer discovery uses a Meridian-style ring overlay: bootstrap, copy rings, measure latency, place peers into concentric latency rings.</p>',
      '<p>Block lookup sends <code>WIRE_FIND_BLOCK</code>; recipients check local cache and peers\' EABFs, then return the block or forward the request with a visited filter to prevent loops.</p>',
      '<p>Hebbian learning strengthens weights along successful routes; faster responses get larger positive updates, while stale weights decay.</p>',
      '<p>NAT traversal uses relays for address discovery and forwarding, then attempts UDP hole punching. mDNS handles same-LAN discovery without a relay.</p>'
    ]
  },
  {
    title: 'Security and trust',
    type: 'content',
    fragments: [
      '<p>Transport-level TLS protects peer and client connections; optional CA-based client certificates add access control.</p>',
      '<p>Admin endpoints use bcrypt-secured API keys; the <code>offs</code> CLI connects over a Unix socket and sends CBOR messages.</p>',
      '<p>Content is anonymized through the OFF block transform: possession and meaning are separated, so stored blocks look like random noise.</p>',
      '<p>No central authority knows what any node is storing; the brightnet model anonymizes data rather than hiding routes.</p>'
    ]
  },
  {
    title: 'Client libraries',
    type: 'content',
    fragments: [
      '<ul>' +
      '<li><strong>C client</strong> — connects over unix://, tcp://, ws://, wss://, wt://, wts://; supports buffered and streaming put, get, block, peer, friend, and config operations.</li>' +
      '<li><strong>Flutter example</strong> — talks to the HTTP REST API for imports, exports, and folder uploads.</li>' +
      '<li><strong>JavaScript client</strong> — browser-only, supports HTTP fetch, WebSocket binary CBOR, and WebTransport over HTTP/3.</li>' +
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

function toHttpUrl(oriString) {
  if (typeof OffsClient !== 'undefined' && OffsClient.offUrlToHttpUrl) {
    return OffsClient.offUrlToHttpUrl(oriString, 'http://localhost:23402');
  }
  return oriString;
}

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
    const httpUrl = ori ? toHttpUrl(ori) : null;
    slideContent.className = 'slide slide-transition-enter';
    slideContent.innerHTML = `
      <div class="slide-content" style="display:flex;flex-direction:column;height:100%">
        <h2>${slide.title}</h2>
        ${httpUrl ?
          `<div class="ori-url">${httpUrl}</div>
           <div class="iframe-wrapper"><iframe src="${httpUrl}" title="${slide.demoType} result"></iframe></div>` :
          '<p class="status error">No upload result available. Go back and run the demo first.</p>'}
      </div>
    `;
    void slideContent.offsetWidth;
    updateProgress();
    return;
  }

  slideContent.className = 'slide slide-transition-enter';
  const fragmentHtml = (slide.fragments || [])
    .map((html) => `<div class="fragment visible">${html}</div>`)
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
  if (current < slides.length - 1) {
    current++;
    renderSlide();
  }
}

function previous() {
  if (current > 0) {
    current--;
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
