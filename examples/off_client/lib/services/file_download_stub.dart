// Stub implementation for unsupported platforms.
//
Future<String> downloadToDisk(String url, String? saveDirectory, String fileName) async {
  throw UnsupportedError('Download is not supported on this platform');
}
