import 'dart:convert';
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:file_picker/file_picker.dart';
import 'package:flutter/services.dart';
import '../services/off_api.dart';

class ImportScreen extends StatefulWidget {
  const ImportScreen({super.key});

  @override
  State<ImportScreen> createState() => _ImportScreenState();
}

class _ImportScreenState extends State<ImportScreen> {
  final OffApi _api = OffApi();
  String? _selectedFilePath;
  String? _selectedFileName;
  bool _isUploading = false;
  String? _resultUrl;
  String? _error;
  double _progress = 0;
  List<String>? _recyclerUrls;
  final TextEditingController _recyclerController = TextEditingController();

  Future<void> _pickFile() async {
    final result = await FilePicker.platform.pickFiles();
    if (result != null && result.files.single.path != null) {
      setState(() {
        _selectedFilePath = result.files.single.path;
        _selectedFileName = result.files.single.name;
        _resultUrl = null;
        _error = null;
      });
    }
  }

  Future<void> _upload() async {
    if (_selectedFilePath == null) return;

    setState(() {
      _isUploading = true;
      _progress = 0;
      _error = null;
      _resultUrl = null;
    });

    try {
      final file = File(_selectedFilePath!);
      final length = await file.length();

      final url = await _api.uploadFile(
        fileName: _selectedFileName!,
        streamLength: length,
        filePath: _selectedFilePath!,
        onProgress: (progress) {
          setState(() => _progress = progress);
        },
      );

      setState(() {
        _progress = 1.0;
        _resultUrl = url;
      });
    } catch (e) {
      setState(() => _error = e.toString());
    } finally {
      setState(() => _isUploading = false);
    }
  }

  Future<void> _importFolder() async {
    final dirResult = await FilePicker.platform.getDirectoryPath();
    if (dirResult == null) return;

    setState(() {
      _isUploading = true;
      _progress = 0;
      _error = null;
      _resultUrl = null;
    });

    try {
      final folder = Directory(dirResult);
      final folderName = folder.path.split(Platform.pathSeparator).last;
      final ofd = <String, String>{};

      final entries = folder.listSync(recursive: true);
      final files = entries.whereType<File>().toList();

      for (int i = 0; i < files.length; i++) {
        final file = files[i];
        final relativePath = file.path.substring(folder.path.length + 1);
        final filePath = relativePath.replaceAll(Platform.pathSeparator, '/');

        final length = await file.length();
        final url = await _api.uploadFile(
          fileName: filePath.split('/').last,
          streamLength: length,
          filePath: file.path,
          recyclerUrls: _recyclerUrls,
        );

        ofd[filePath] = url;
        setState(() => _progress = (i + 1) / (files.length + 1));
      }

      final ofdJson = jsonEncode(ofd);
      final ofdBytes = utf8.encode(ofdJson);
      final finalUrl = await _api.uploadFileBuffered(
        fileName: '$folderName.ofd',
        streamLength: ofdBytes.length,
        contentType: 'offsystem/directory',
        bodyBytes: ofdBytes,
        recyclerUrls: _recyclerUrls,
      );

      setState(() {
        _progress = 1.0;
        _resultUrl = finalUrl;
      });
    } catch (e) {
      setState(() => _error = e.toString());
    } finally {
      setState(() => _isUploading = false);
    }
  }

  @override
  void dispose() {
    _recyclerController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.all(24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          const Text('Import File', style: TextStyle(fontSize: 24, fontWeight: FontWeight.bold)),
          const SizedBox(height: 24),
          ElevatedButton.icon(
            onPressed: _isUploading ? null : _importFolder,
            icon: const Icon(Icons.create_new_folder),
            label: const Text('Import Folder'),
          ),
          ElevatedButton.icon(
            onPressed: _isUploading ? null : _pickFile,
            icon: const Icon(Icons.folder_open),
            label: const Text('Select File'),
          ),
          if (_selectedFileName != null) ...[
            const SizedBox(height: 16),
            Text('Selected: $_selectedFileName', style: const TextStyle(fontSize: 14)),
          ],
          const SizedBox(height: 24),
          ElevatedButton(
            onPressed: _selectedFilePath != null && !_isUploading ? _upload : null,
            style: ElevatedButton.styleFrom(
              backgroundColor: const Color(0xFF313181),
              foregroundColor: Colors.white,
            ),
            child: _isUploading
                ? const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2, color: Colors.white))
                : const Text('Upload'),
          ),
          if (_isUploading) ...[
            const SizedBox(height: 16),
            LinearProgressIndicator(value: _progress),
          ],
          if (_resultUrl != null) ...[
            const SizedBox(height: 24),
            Container(
              padding: const EdgeInsets.all(12),
              decoration: BoxDecoration(
                color: Colors.green.shade900.withOpacity(0.3),
                borderRadius: BorderRadius.circular(8),
                border: Border.all(color: Colors.green),
              ),
              child: Row(
                children: [
                  Expanded(
                    child: Text(_resultUrl!, style: const TextStyle(fontSize: 12, fontFamily: 'monospace')),
                  ),
                  IconButton(
                    icon: const Icon(Icons.copy, size: 18),
                    onPressed: () {
                      Clipboard.setData(ClipboardData(text: _resultUrl!));
                      ScaffoldMessenger.of(context).showSnackBar(
                        const SnackBar(content: Text('URL copied to clipboard')),
                      );
                    },
                  ),
                ],
              ),
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