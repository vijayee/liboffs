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
      '<p>The unit of storage in the Owner Free File System is the <strong>block</strong>. A block is a 128 KB chunk of data.</p>',
      '<p>Blocks are grouped into <strong>tuples</strong>, and tuples are grouped into <strong>descriptors</strong>. Descriptors reveal the representation of a file.</p>',
      '<p>When the correct math is applied along with the user\'s ORI, you can get the original file from these underlying structures.</p>'
    ]
  },
  {
    title: 'The algorithms',
    type: 'content',
    fragments: [
      '<h3>Store Procedure</h3>' +
      '<ol>' +
      '<li>Choose tuple size <strong>t</strong> (default 3) and split the source file into 128 KiB blocks <strong>sᵢ</strong>.</li>' +
      '<li>For each block, pick <strong>t − 1</strong> randomizer blocks <strong>r₁, r₂, ...</strong> from cache, or generate fresh random data.</li>' +
      '<li>Store <strong>oᵢ = sᵢ ⊕ r₁ ⊕ r₂ ⊕ ... ⊕ rₜ₋₁</strong> in the cache (⊕ is XOR).</li>' +
      '<li>Record the set <strong>{ oᵢ, r₁, r₂, ..., rₜ₋₁ }</strong> in the descriptor list.</li>' +
      '<li>Store the descriptor list in its own block(s) and output the OFFS URL.</li>' +
      '</ol>' +
      '<div class="algo-diagram">' +
        '<div class="algo-box"><strong>sᵢ</strong><br>(source block)</div>' +
        '<span class="algo-op">⊕</span>' +
        '<div class="algo-box"><strong>r₁</strong><br>(randomizer)</div>' +
        '<span class="algo-op">⊕</span>' +
        '<div class="algo-box"><strong>r₂</strong><br>(randomizer)</div>' +
        '<span class="algo-arrow">→</span>' +
        '<div class="algo-box algo-result"><strong>oᵢ</strong><br>(stored block)</div>' +
      '</div>',

      '<h3>Retrieve Procedure</h3>' +
      '<ol>' +
      '<li>Obtain the descriptor block(s) for the requested OFFS URL.</li>' +
      '<li>For each stored set <strong>{ oᵢ, r₁, r₂, ..., rₜ₋₁ }</strong>, fetch all listed blocks from cache or network.</li>' +
      '<li>Recover the source block by XORing them together: <strong>sᵢ = oᵢ ⊕ r₁ ⊕ r₂ ⊕ ... ⊕ rₜ₋₁</strong>.</li>' +
      '<li>Assemble the recovered blocks into the original file.</li>' +
      '</ol>' +
      '<div class="algo-diagram">' +
        '<div class="algo-box algo-result"><strong>oᵢ</strong><br>(stored block)</div>' +
        '<span class="algo-op">⊕</span>' +
        '<div class="algo-box"><strong>r₁</strong><br>(randomizer)</div>' +
        '<span class="algo-op">⊕</span>' +
        '<div class="algo-box"><strong>r₂</strong><br>(randomizer)</div>' +
        '<span class="algo-arrow">→</span>' +
        '<div class="algo-box"><strong>sᵢ</strong><br>(source block)</div>' +
      '</div>'
    ]
  },
  {
    title: 'The Architecture',
    type: 'content',
    fragments: [
      '<div class="arch-diagram">' +
        '<div class="arch-layer">' +
          '<div class="arch-title">Applications</div>' +
          '<div class="arch-boxes">' +
            '<div class="arch-box">offs CLI</div>' +
            '<div class="arch-box">C client library</div>' +
            '<div class="arch-box">JS client</div>' +
            '<div class="arch-box">your application</div>' +
          '</div>' +
        '</div>' +
        '<div class="arch-connector"></div>' +
        '<div class="arch-layer">' +
          '<div class="arch-title">ClientAPI</div>' +
          '<div class="arch-boxes">' +
            '<div class="arch-box">HTTP</div>' +
            '<div class="arch-box">Unix</div>' +
            '<div class="arch-box">TCP</div>' +
            '<div class="arch-box">WebSocket</div>' +
            '<div class="arch-box">WebTransport</div>' +
          '</div>' +
        '</div>' +
        '<div class="arch-connector"></div>' +
        '<div class="arch-layer">' +
          '<div class="arch-title">offsd / liboffs</div>' +
          '<div class="arch-boxes">' +
            '<div class="arch-box">BlockCache</div>' +
            '<div class="arch-box">OFFStreams</div>' +
            '<div class="arch-box">Network</div>' +
            '<div class="arch-box">Actor/Scheduler</div>' +
          '</div>' +
        '</div>' +
      '</div>' +
      '<p>Clients connect through the ClientAPI transports into the daemon, which is built on liboffs. The core layers handle block storage, OFF stream encoding, peer-to-peer networking, and asynchronous execution.</p>'
    ]
  },
  {
    title: 'The network layer',
    type: 'content',
    fragments: [
      '<p>The network of the modern implementation uses a modified version of the Meridian routing protocol. It is a self-organizing peer-to-peer network.</p>',
      '<p>It uses Hebbian learning and short-term memory to optimize connections and the usage of storage.</p>',
      '<p>Data migrates where it is needed most through respiration.</p>'
    ]
  },
  {
    title: 'Why are we doing this?',
    type: 'content',
    fragments: [
      '<ul>' +
      '<li>Bit torrent files\' life span stems from the popularity of individual files. Files represented in the off system are constantly extending their availability as new files are being represented.</li>' +
      '<li>IPFS has no inherent mechanisme that creates redundancy or popularity of data.</li>' +
      '<li>The OFF System\'s method creates a universal public storage cloud with similar properties as national public radio or public broadcasting.</li>' +
      '<li>It reduces the security burden of datat to its ORI rather than the complete file.</li>' +
      '<li>It is censorship proof and decentralized.</li>' +
      '</ul>'
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
