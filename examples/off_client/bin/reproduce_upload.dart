import 'dart:convert';
import 'dart:io';
import 'package:http/http.dart' as http;

const String baseUrl = 'http://localhost:23402';

String mimeFromExtension(String filename) {
  const map = {
    'html': 'text/html',
    'htm': 'text/html',
    'css': 'text/css',
    'js': 'application/javascript',
    'json': 'application/json',
    'png': 'image/png',
    'jpg': 'image/jpeg',
    'jpeg': 'image/jpeg',
    'gif': 'image/gif',
    'svg': 'image/svg+xml',
    'ico': 'image/x-icon',
    'webp': 'image/webp',
    'mp4': 'video/mp4',
    'webm': 'video/webm',
    'mkv': 'video/x-matroska',
    'avi': 'video/x-msvideo',
    'mov': 'video/quicktime',
    'wmv': 'video/x-ms-wm',
    'flv': 'video/x-flv',
    'mp3': 'audio/mpeg',
    'ogg': 'audio/ogg',
    'wav': 'audio/wav',
    'flac': 'audio/flac',
    'aac': 'audio/aac',
    'm4a': 'audio/mp4',
    'woff': 'font/woff',
    'woff2': 'font/woff2',
    'ttf': 'font/ttf',
    'otf': 'font/otf',
    'pdf': 'application/pdf',
    'zip': 'application/zip',
    'gz': 'application/gzip',
    'tar': 'application/x-tar',
    'rar': 'application/vnd.rar',
    '7z': 'application/x-7z-compressed',
    'doc': 'application/msword',
    'docx': 'application/vnd.openxmlformats-officedocument.wordprocessingml.document',
    'xls': 'application/vnd.ms-excel',
    'xlsx': 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
    'ppt': 'application/vnd.ms-powerpoint',
    'pptx': 'application/vnd.openxmlformats-officedocument.presentationml.presentation',
    'txt': 'text/plain',
    'csv': 'text/csv',
    'xml': 'application/xml',
    'md': 'text/markdown',
    'ofd': 'application/cbor',
  };
  final dot = filename.lastIndexOf('.');
  if (dot < 0 || dot == filename.length - 1) return 'application/octet-stream';
  final ext = filename.substring(dot + 1).toLowerCase();
  return map[ext] ?? 'application/octet-stream';
}

String basename(String path) {
  final separators = RegExp(r'[\\/]');
  return path
      .split(separators)
      .lastWhere((segment) => segment.isNotEmpty, orElse: () => 'file');
}

Future<String> uploadFile({
  required String fileName,
  required int streamLength,
  required String filePath,
}) async {
  final safeFileName = basename(fileName);
  final type = mimeFromExtension(safeFileName);
  final uri = Uri.parse('$baseUrl/offsystem');
  final file = File(filePath);
  final fileStream = file.openRead();

  final request = http.StreamedRequest('PUT', uri);
  request.headers['type'] = type;
  request.headers['file-name'] = safeFileName;
  request.headers['stream-length'] = streamLength.toString();
  request.headers['Content-Type'] = 'application/octet-stream';

  int sent = 0;
  fileStream.listen(
    (data) {
      request.sink.add(data);
      sent += data.length;
      stderr.writeln('  sent $sent / $streamLength');
    },
    onDone: () => request.sink.close(),
    onError: (error) => request.sink.close(),
    cancelOnError: true,
  );

  final response = await request.send();
  final responseBody = await response.stream.bytesToString();
  if (response.statusCode == 200) {
    return responseBody;
  } else {
    throw Exception('Upload failed: ${response.statusCode} $responseBody');
  }
}

Future<void> main() async {
  final demoDir = Directory('/home/victor/Workspace/src/github.com/vijayee/liboffs/demo');
  if (!demoDir.existsSync()) {
    stderr.writeln('Demo directory not found');
    exit(1);
  }

  final files = demoDir
      .listSync(recursive: true)
      .whereType<File>()
      .where((file) => !file.path.contains('test-crash') && !file.path.contains('node_modules'))
      .toList();

  stderr.writeln('Found ${files.length} files to upload');

  for (final file in files) {
    final length = await file.length();
    final relName = file.path.substring(demoDir.path.length + 1);
    stderr.writeln('Uploading $relName ($length bytes)...');
    try {
      final url = await uploadFile(
        fileName: relName,
        streamLength: length,
        filePath: file.path,
      );
      print('OK $relName -> $url');
    } catch (e) {
      stderr.writeln('FAILED $relName: $e');
      rethrow;
    }
  }

  stderr.writeln('All uploads complete');
}
