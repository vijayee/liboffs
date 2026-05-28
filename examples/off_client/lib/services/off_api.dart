import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';
import 'package:http/http.dart' as http;

String mimeFromExtension(String filename) {
  const map = <String, String>{
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
    'bmp': 'image/bmp',
    'tiff': 'image/tiff',
    'tif': 'image/tiff',
    'mp4': 'video/mp4',
    'webm': 'video/webm',
    'mkv': 'video/x-matroska',
    'avi': 'video/x-msvideo',
    'mov': 'video/quicktime',
    'wmv': 'video/x-ms-wmv',
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
    'docx':
        'application/vnd.openxmlformats-officedocument.wordprocessingml.document',
    'xls': 'application/vnd.ms-excel',
    'xlsx':
        'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
    'ppt': 'application/vnd.ms-powerpoint',
    'pptx':
        'application/vnd.openxmlformats-officedocument.presentationml.presentation',
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

class OffApi {
  final String baseUrl;
  final String? _apiKey;

  OffApi({this.baseUrl = 'http://localhost:23402', String? apiKey})
      : _apiKey = apiKey;

  /// Stream a file upload without loading the entire file into memory.
  /// The server processes data as it arrives via the streaming PUT pipeline.
  Future<String> uploadFile({
    required String fileName,
    required int streamLength,
    String? contentType,
    String? serverAddress,
    List<String>? recyclerUrls,
    bool temporary = false,
    required String filePath,
    void Function(double progress)? onProgress,
  }) async {
    final type = contentType ?? mimeFromExtension(fileName);
    final uri = Uri.parse('$baseUrl/offsystem');
    final file = File(filePath);
    final fileStream = file.openRead();

    final request = http.StreamedRequest('PUT', uri);
    request.headers['type'] = type;
    request.headers['file-name'] = fileName;
    request.headers['stream-length'] = streamLength.toString();
    request.headers['Content-Type'] = 'application/octet-stream';
    if (serverAddress != null) {
      request.headers['server-address'] = serverAddress;
    }
    if (recyclerUrls != null && recyclerUrls.isNotEmpty) {
      request.headers['recycler'] = jsonEncode(recyclerUrls);
    }
    if (temporary) {
      request.headers['temporary'] = 'true';
    }

    int sent = 0;
    fileStream.listen(
      (data) {
        request.sink.add(data);
        sent += data.length;
        onProgress?.call(sent / streamLength);
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

  /// Buffered upload for small files or backward compatibility.
  /// Loads the entire file into memory before sending — avoid for large files.
  Future<String> uploadFileBuffered({
    required String fileName,
    required int streamLength,
    String? contentType,
    String? serverAddress,
    List<String>? recyclerUrls,
    bool temporary = false,
    required List<int> bodyBytes,
  }) async {
    final type = contentType ?? mimeFromExtension(fileName);
    final uri = Uri.parse('$baseUrl/offsystem');
    final request = http.Request('PUT', uri);
    request.headers['type'] = type;
    request.headers['file-name'] = fileName;
    request.headers['stream-length'] = streamLength.toString();
    request.headers['Content-Type'] = 'application/octet-stream';
    if (serverAddress != null) {
      request.headers['server-address'] = serverAddress;
    }
    if (recyclerUrls != null && recyclerUrls.isNotEmpty) {
      request.headers['recycler'] = jsonEncode(recyclerUrls);
    }
    if (temporary) {
      request.headers['temporary'] = 'true';
    }
    request.bodyBytes = bodyBytes;

    final response = await request.send();
    final responseBody = await response.stream.bytesToString();
    if (response.statusCode == 200) {
      return responseBody;
    } else {
      throw Exception('Upload failed: ${response.statusCode} $responseBody');
    }
  }

  Future<List<int>> downloadFile(String offUrl) async {
    final uri = Uri.parse(offUrl);
    final response = await http.get(uri);
    if (response.statusCode == 200) {
      return response.bodyBytes;
    } else {
      throw Exception('Download failed: ${response.statusCode}');
    }
  }

  Future<void> deleteContent(String offUrl) async {
    final uri = Uri.parse(offUrl);
    final request = http.Request('DELETE', uri);
    final response = await request.send();
    if (response.statusCode != 200) {
      throw Exception('Delete failed: ${response.statusCode}');
    }
  }

  Future<Map<String, dynamic>> healthCheck() async {
    final uri = Uri.parse('$baseUrl/health');
    final response = await http.get(uri);
    if (response.statusCode == 200) {
      return json.decode(response.body) as Map<String, dynamic>;
    }
    throw Exception('Health check failed: ${response.statusCode}');
  }

  /// Fetch raw OFD CBOR bytes from a URL with ?ofd=raw.
  Future<Uint8List> fetchRawOfd(String url) async {
    final separator = url.contains('?') ? '&' : '?';
    final rawUrl = '$url${separator}ofd=raw';
    final response = await http.get(Uri.parse(rawUrl));
    if (response.statusCode == 200) {
      return response.bodyBytes;
    }
    throw Exception('OFD fetch failed: ${response.statusCode}');
  }

  Future<Uint8List> getPeerInfo({String format = 'cbor'}) async {
    final uri = Uri.parse('$baseUrl/peer/info?format=$format');
    final response = await http.get(uri, headers: {
      if (_apiKey != null) 'Authorization': 'Bearer $_apiKey',
    });
    if (response.statusCode == 200) return response.bodyBytes;
    throw Exception('Peer info failed: ${response.statusCode}');
  }

  Future<String> connectPeer(Uint8List peerInfoCbor) async {
    final uri = Uri.parse('$baseUrl/peer/connect');
    final response = await http.post(uri, headers: {
      if (_apiKey != null) 'Authorization': 'Bearer $_apiKey',
      'Content-Type': 'application/cbor',
    }, body: peerInfoCbor);
    if (response.statusCode == 200) return utf8.decode(response.bodyBytes);
    throw Exception('Peer connect failed: ${response.statusCode}');
  }

  Future<String> connectPeerBase58(String peerInfoBase58) async {
    final uri = Uri.parse('$baseUrl/peer/connect');
    final response = await http.post(uri, headers: {
      if (_apiKey != null) 'Authorization': 'Bearer $_apiKey',
      'Content-Type': 'text/plain',
    }, body: peerInfoBase58);
    if (response.statusCode == 200) return utf8.decode(response.bodyBytes);
    throw Exception('Peer connect failed: ${response.statusCode}');
  }

  Future<List<Map<String, dynamic>>> listPeers() async {
    final uri = Uri.parse('$baseUrl/peers');
    final response = await http.get(uri, headers: {
      if (_apiKey != null) 'Authorization': 'Bearer $_apiKey',
    });
    if (response.statusCode == 200) {
      return (json.decode(response.body) as List)
          .map((e) => e as Map<String, dynamic>)
          .toList();
    }
    throw Exception('Peer list failed: ${response.statusCode}');
  }

  Future<void> addFriend(Uint8List peerInfoCbor) async {
    final uri = Uri.parse('$baseUrl/friends');
    final response = await http.post(uri, headers: {
      if (_apiKey != null) 'Authorization': 'Bearer $_apiKey',
      'Content-Type': 'application/cbor',
    }, body: peerInfoCbor);
    if (response.statusCode != 200) {
      throw Exception('Friend add failed: ${response.statusCode}');
    }
  }

  Future<void> removeFriend(String nodeId) async {
    final uri = Uri.parse('$baseUrl/friends/$nodeId');
    final response = await http.delete(uri, headers: {
      if (_apiKey != null) 'Authorization': 'Bearer $_apiKey',
    });
    if (response.statusCode != 200) {
      throw Exception('Friend remove failed: ${response.statusCode}');
    }
  }

  Future<List<Map<String, dynamic>>> listFriends() async {
    final uri = Uri.parse('$baseUrl/friends');
    final response = await http.get(uri, headers: {
      if (_apiKey != null) 'Authorization': 'Bearer $_apiKey',
    });
    if (response.statusCode == 200) {
      return (json.decode(response.body) as List)
          .map((e) => e as Map<String, dynamic>)
          .toList();
    }
    throw Exception('Friend list failed: ${response.statusCode}');
  }
}