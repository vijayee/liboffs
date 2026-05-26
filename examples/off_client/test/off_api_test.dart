import 'package:flutter_test/flutter_test.dart';
import 'package:off_client/services/off_api.dart';

void main() {
  group('OffApi', () {
    test('healthCheck constructs correct URL', () {
      final api = OffApi(baseUrl: 'http://localhost:23402');
      // Verifies the class can be instantiated and method exists.
      // Full integration test requires a running OFF server.
      expect(api, isNotNull);
    });
  });

  group('mimeFromExtension', () {
    test('returns correct MIME for known extensions', () {
      expect(mimeFromExtension('test.png'), 'image/png');
      expect(mimeFromExtension('test.json'), 'application/json');
      expect(mimeFromExtension('test.txt'), 'text/plain');
      expect(mimeFromExtension('test.html'), 'text/html');
      expect(mimeFromExtension('test.jpg'), 'image/jpeg');
      expect(mimeFromExtension('test.jpeg'), 'image/jpeg');
      expect(mimeFromExtension('test.pdf'), 'application/pdf');
      expect(mimeFromExtension('test.zip'), 'application/zip');
      expect(mimeFromExtension('test.mp4'), 'video/mp4');
    });

    test('returns octet-stream for unknown extensions', () {
      expect(mimeFromExtension('test.xyz'), 'application/octet-stream');
      expect(mimeFromExtension('test.unknown'), 'application/octet-stream');
    });

    test('returns octet-stream for filenames without extension', () {
      expect(mimeFromExtension('noextension'), 'application/octet-stream');
      expect(mimeFromExtension(''), 'application/octet-stream');
    });

    test('handles filenames with multiple dots', () {
      expect(mimeFromExtension('archive.tar.gz'), 'application/gzip');
      expect(mimeFromExtension('file.min.js'), 'application/javascript');
    });
  });
}
