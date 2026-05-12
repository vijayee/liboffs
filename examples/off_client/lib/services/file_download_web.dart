// Web implementation: trigger a browser-native download via anchor tag.
//
import 'dart:html';
import 'dart:typed_data';
import 'package:http/http.dart' as http;

Future<String> downloadToDisk(String url, String? saveDirectory, String fileName) async {
  final response = await http.get(Uri.parse(url));
  if (response.statusCode != 200) {
    throw Exception('Download failed: ${response.statusCode}');
  }

  final blob = Blob([Uint8List.fromList(response.bodyBytes)]);
  final objectUrl = Url.createObjectUrlFromBlob(blob);
  AnchorElement(href: objectUrl)
    ..setAttribute('download', fileName)
    ..click();
  Url.revokeObjectUrl(objectUrl);

  return fileName;
}
