import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter/foundation.dart';
import 'package:file_picker/file_picker.dart';
import 'package:http/http.dart' as http;
import '../services/file_download.dart';
import '../services/ofd.dart';
import '../services/base58.dart';

class ExportScreen extends StatefulWidget {
  const ExportScreen({super.key});

  @override
  State<ExportScreen> createState() => _ExportScreenState();
}

class _ExportScreenState extends State<ExportScreen> {
  final TextEditingController _urlController = TextEditingController();
  String? _saveDirectory;
  bool _isDownloading = false;
  String? _resultFile;
  String? _error;

  Future<void> _pickDirectory() async {
    final result = await FilePicker.platform.getDirectoryPath();
    if (result != null) {
      setState(() => _saveDirectory = result);
    }
  }

  Future<void> _download() async {
    final url = _urlController.text.trim();
    if (url.isEmpty) return;

    setState(() {
      _isDownloading = true;
      _error = null;
      _resultFile = null;
    });

    try {
      final fileName = url.split('/').last.split('?').first;

      if (!kIsWeb && _saveDirectory == null) {
        setState(() {
          _isDownloading = false;
          _error = 'Please select a save directory';
        });
        return;
      }

      final path = await downloadToDisk(url, _saveDirectory, fileName);
      setState(() => _resultFile = path);
    } catch (e) {
      setState(() => _error = e.toString());
    } finally {
      setState(() => _isDownloading = false);
    }
  }

  /// Construct an offs URL for a file from its OFD entry data.
  String _buildFileUrl(Uri baseUri, OfdEntry entry) {
    final fileHashB58 = base58Encode(entry.fileHash!.toList());
    final descHashB58 = base58Encode(entry.descriptorHash!.toList());
    final contentType = mimeFromExtension(entry.name);
    return '${baseUri.origin}/offsystem/v3/$contentType/${entry.finalByte}/$fileHashB58/$descHashB58/${Uri.encodeComponent(entry.name)}';
  }

  /// Recursively export OFD entries to the local filesystem.
  Future<void> _exportOfdEntries(
    Uri baseUri,
    List<OfdEntry> entries,
    Directory localDir,
  ) async {
    for (final entry in entries) {
      if (entry.isDirectory) {
        // Fetch the sub-OFD and recurse
        final dirHashB58 = base58Encode(entry.dirHash!.toList());
        final subOfdUrl = '${baseUri.origin}/offsystem/v3/offsystem/directory/0/$dirHashB58/$dirHashB58/${Uri.encodeComponent(entry.name)}.ofd?ofd=raw';
        final response = await http.get(Uri.parse(subOfdUrl));
        if (response.statusCode != 200) {
          throw Exception('Failed to fetch sub-OFD ${entry.name}: ${response.statusCode}');
        }

        final subOfd = parseOfdCbor(response.bodyBytes);
        final subDir = Directory('${localDir.path}${Platform.pathSeparator}${entry.name}');
        if (!await subDir.exists()) {
          await subDir.create(recursive: true);
        }
        await _exportOfdEntries(baseUri, subOfd, subDir);
      } else {
        final fileUrl = _buildFileUrl(baseUri, entry);
        final fileResponse = await http.get(Uri.parse(fileUrl));
        if (fileResponse.statusCode != 200) {
          throw Exception('Failed to download ${entry.name}: ${fileResponse.statusCode}');
        }

        final filePath = '${localDir.path}${Platform.pathSeparator}${entry.name}';
        final file = File(filePath);
        await file.writeAsBytes(fileResponse.bodyBytes);
      }
    }
  }

  Future<void> _exportFolder() async {
    final url = _urlController.text.trim();
    if (url.isEmpty) return;
    if (_saveDirectory == null) {
      setState(() => _error = 'Please select a save directory');
      return;
    }

    setState(() {
      _isDownloading = true;
      _error = null;
      _resultFile = null;
    });

    try {
      final baseUri = Uri.parse(url);
      final rawUrl = url.contains('?') ? '$url&ofd=raw' : '$url?ofd=raw';
      final response = await http.get(Uri.parse(rawUrl));
      if (response.statusCode != 200) {
        throw Exception('Failed to fetch OFD: ${response.statusCode}');
      }

      final ofdEntries = parseOfdCbor(response.bodyBytes);
      if (ofdEntries.isEmpty) {
        throw Exception('OFD is empty or could not be parsed');
      }

      // Determine local directory name from the OFD URL filename
      final pathSegments = baseUri.pathSegments;
      final rawFileName = pathSegments.isNotEmpty ? pathSegments.last : 'export';
      final dirName = rawFileName.endsWith('.ofd')
          ? rawFileName.substring(0, rawFileName.length - 4)
          : rawFileName;
      final localDir = Directory('${_saveDirectory!}${Platform.pathSeparator}$dirName');
      if (!await localDir.exists()) {
        await localDir.create(recursive: true);
      }

      await _exportOfdEntries(baseUri, ofdEntries, localDir);

      setState(() => _resultFile = localDir.path);
    } catch (e) {
      setState(() => _error = e.toString());
    } finally {
      setState(() => _isDownloading = false);
    }
  }

  String mimeFromExtension(String filename) {
    final dot = filename.lastIndexOf('.');
    if (dot < 0 || dot == filename.length - 1) return 'application/octet-stream';
    return filename.substring(dot + 1).toLowerCase();
  }

  @override
  void dispose() {
    _urlController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.all(24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          const Text('Export File', style: TextStyle(fontSize: 24, fontWeight: FontWeight.bold)),
          const SizedBox(height: 24),
          TextField(
            controller: _urlController,
            decoration: const InputDecoration(
              labelText: 'OFF URL',
              hintText: 'http://localhost:23402/offsystem/v3/...',
              border: OutlineInputBorder(),
            ),
          ),
          const SizedBox(height: 16),
          if (!kIsWeb)
            ElevatedButton.icon(
              onPressed: _pickDirectory,
              icon: const Icon(Icons.folder),
              label: Text(_saveDirectory ?? 'Choose Save Directory'),
            ),
          if (!kIsWeb) const SizedBox(height: 16),
          ElevatedButton(
            onPressed: _urlController.text.isNotEmpty && !_isDownloading ? _download : null,
            style: ElevatedButton.styleFrom(
              backgroundColor: const Color(0xFF313181),
              foregroundColor: Colors.white,
            ),
            child: _isDownloading
                ? const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2, color: Colors.white))
                : const Text('Download'),
          ),
          const SizedBox(height: 12),
          ElevatedButton(
            onPressed: _urlController.text.isNotEmpty && !_isDownloading ? _exportFolder : null,
            style: ElevatedButton.styleFrom(
              backgroundColor: const Color(0xFF313181),
              foregroundColor: Colors.white,
            ),
            child: _isDownloading
                ? const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2, color: Colors.white))
                : const Text('Export Folder'),
          ),
          if (_resultFile != null) ...[
            const SizedBox(height: 16),
            Container(
              padding: const EdgeInsets.all(12),
              decoration: BoxDecoration(
                color: Colors.green.shade900.withValues(alpha: 0.3),
                borderRadius: BorderRadius.circular(8),
                border: Border.all(color: Colors.green),
              ),
              child: Text('Saved to: $_resultFile'),
            ),
          ],
          if (_error != null) ...[
            const SizedBox(height: 16),
            Container(
              padding: const EdgeInsets.all(12),
              decoration: BoxDecoration(
                color: Colors.red.shade900.withValues(alpha: 0.3),
                borderRadius: BorderRadius.circular(8),
                border: Border.all(color: Colors.red),
              ),
              child: Text(_error!, style: const TextStyle(color: Colors.redAccent)),
            ),
          ],
        ],
      ),
    );
  }
}
