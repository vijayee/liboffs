// Web implementation: trigger a browser-native download via anchor tag.
//
import 'dart:js_interop';
import 'package:http/http.dart' as http;
import 'package:web/web.dart' as web;

Future<String> downloadToDisk(String url, String? saveDirectory, String fileName) async {
  final response = await http.get(Uri.parse(url));
  if (response.statusCode != 200) {
    throw Exception('Download failed: ${response.statusCode}');
  }

  final bytes = response.bodyBytes.toJS;
  final blob = web.Blob(
    [bytes].toJS,
    web.BlobPropertyBag(type: 'application/octet-stream'),
  );
  final objectUrl = web.URL.createObjectURL(blob);

  final anchor = web.HTMLAnchorElement()
    ..href = objectUrl
    ..download = fileName;
  anchor.click();

  web.URL.revokeObjectURL(objectUrl);

  return fileName;
}
