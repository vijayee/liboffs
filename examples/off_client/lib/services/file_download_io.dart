// Desktop implementation: stream download directly to file.
//
import 'dart:io';
import 'package:http/http.dart' as http;

Future<String> downloadToDisk(String url, String? saveDirectory, String fileName) async {
  if (saveDirectory == null) {
    throw ArgumentError('saveDirectory is required on desktop');
  }
  final file = File('$saveDirectory/$fileName');
  final sink = file.openWrite();

  final uri = Uri.parse(url);
  final request = http.Request('GET', uri);
  final response = await http.Client().send(request);

  if (response.statusCode != 200) {
    throw Exception('Download failed: ${response.statusCode}');
  }

  try {
    await for (final chunk in response.stream) {
      sink.add(chunk);
    }
  } finally {
    await sink.close();
  }

  return file.path;
}
