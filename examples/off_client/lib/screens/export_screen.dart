import 'package:flutter/material.dart';
import 'package:flutter/foundation.dart';
import 'package:file_picker/file_picker.dart';
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
