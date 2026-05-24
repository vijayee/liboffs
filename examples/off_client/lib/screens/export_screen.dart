import 'dart:convert';
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter/foundation.dart';
import 'package:file_picker/file_picker.dart';
import 'package:http/http.dart' as http;
import '../services/file_download.dart';

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

      if (!kIsWeb) {
        if (_saveDirectory == null) {
          setState(() => _error = 'Please select a save directory');
          return;
        }
      }

      final path = await downloadToDisk(url, _saveDirectory, fileName);
      setState(() => _resultFile = path);
    } catch (e) {
      setState(() => _error = e.toString());
    } finally {
      setState(() => _isDownloading = false);
    }
  }

  Future<void> _exportFolder() async {
    final url = _urlController.text.trim();
    if (url.isEmpty || _saveDirectory == null) return;

    setState(() {
      _isDownloading = true;
      _error = null;
      _resultFile = null;
    });

    try {
      final rawUrl = url.contains('?') ? '$url&ofd=raw' : '$url?ofd=raw';
      final response = await http.get(Uri.parse(rawUrl));
      if (response.statusCode != 200) {
        throw Exception('Failed to fetch OFD: ${response.statusCode}');
      }

      final ofd = jsonDecode(response.body) as Map<String, dynamic>;
      final urlParsed = Uri.parse(url);
      final pathSegments = urlParsed.pathSegments;
      final rawFileName = pathSegments.isNotEmpty ? pathSegments.last : 'export';
      final dirName = rawFileName.endsWith('.ofd')
          ? rawFileName.substring(0, rawFileName.length - 4)
          : rawFileName;
      final localDir = Directory('${_saveDirectory!}${Platform.pathSeparator}$dirName');
      if (!await localDir.exists()) {
        await localDir.create(recursive: true);
      }

      final entries = ofd.entries.toList();
      for (int i = 0; i < entries.length; i++) {
        final path = entries[i].key;
        final fileUrl = entries[i].value as String;

        final fileResponse = await http.get(Uri.parse(fileUrl));
        if (fileResponse.statusCode != 200) {
          throw Exception('Failed to download $path: ${fileResponse.statusCode}');
        }

        final filePath = '${localDir.path}${Platform.pathSeparator}$path';
        final file = File(filePath);
        final parentDir = file.parent;
        if (!await parentDir.exists()) {
          await parentDir.create(recursive: true);
        }
        await file.writeAsBytes(fileResponse.bodyBytes);
      }

      setState(() => _resultFile = localDir.path);
    } catch (e) {
      setState(() => _error = e.toString());
    } finally {
      setState(() => _isDownloading = false);
    }
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
                color: Colors.green.shade900.withOpacity(0.3),
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
                color: Colors.red.shade900.withOpacity(0.3),
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
