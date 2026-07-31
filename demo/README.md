# OFF System HTML5 Presentation

A static HTML5 slide deck with three live OFFS upload demos.

## Run

1. Start an OFFS server on port 23402:

   ```bash
   ./build-test/examples/off_server --host 0.0.0.0 --port 23402 --cache-dir /tmp/offs-demo
   ```

2. Serve the demo folder:

   ```bash
   cd demo
   python3 -m http.server 8080
   ```

3. Open `http://localhost:8080` in a browser.

## Controls

- Right arrow / Space: next fragment or slide
- Left arrow: previous fragment or slide
- On-screen buttons: previous / next

## Demos

- PDF upload → displayed in iframe
- Video upload → displayed in iframe
- Static site upload → displayed in iframe
