import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';
import 'package:flutter/material.dart';
import 'package:file_picker/file_picker.dart';
import 'package:flutter/services.dart';
import '../services/off_api.dart';
import '../services/ofd.dart';
import '../services/base58.dart';
import '../services/recycler_resolver.dart';

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

  /// Check if a URL is an OFD (off file descriptor) by URL pattern.
  static bool _isOfdUrl(String url) {
    return url.contains('offsystem/directory') || url.endsWith('.ofd');
  }

  /// Check if an OFD URL's filename matches a directory name.
  static bool _ofdMatchesDir(String url, String dirName) {
    final uri = Uri.parse(url);
    final segments = uri.pathSegments;
    if (segments.isEmpty) return false;
    final filename = segments.last;
    return filename == '$dirName.ofd' || filename == dirName;
  }

  /// Filter recycler URLs to those that are NOT OFDs.
  List<String> _nonOfdRecyclers() {
    if (_recyclerUrls == null) return [];
    return _recyclerUrls!.where((u) => !_isOfdUrl(u)).toList();
  }

  /// Upload a directory recursively and return the URL of its top-level OFD.
  Future<String> _uploadDirectory(Directory dir,
      {List<OfdEntry>? parentDonorPool}) async {
    final entries = <OfdEntry>[];
    final dirEntries = dir.listSync();

    final files = dirEntries.whereType<File>().toList();
    final subdirs = dirEntries.whereType<Directory>().toList();
    final dirName = dir.path.split(Platform.pathSeparator).last;

    // Build donor pool: start from parent, add from matching OFD recyclers
    final donorPool = <OfdEntry>[...?parentDonorPool];
    final ofdEntryMap = <String, OfdEntry>{};
    String? recyclerServerBase;

    if (_recyclerUrls != null) {
      final resolver = RecyclerResolver(_api);
      for (final url in _recyclerUrls!) {
        if (_isOfdUrl(url) && _ofdMatchesDir(url, dirName)) {
          try {
            final resolved = await resolver.resolveOfd(url);
            recyclerServerBase = resolved.serverBase;
            for (final entry in resolved.entries) {
              donorPool.add(entry);
              ofdEntryMap[entry.name] = entry;
            }
          } catch (_) {
            // OFD resolution failed -- continue without it
          }
        }
      }
    }

    final fallbackRecyclers = _nonOfdRecyclers();

    // Process subdirectories first: each gets its own OFD uploaded
    for (final subdir in subdirs) {
      final subDirName = subdir.path.split(Platform.pathSeparator).last;
      final subUrl =
          await _uploadDirectory(subdir, parentDonorPool: donorPool);
      final parsed = parseOffUrl(subUrl);
      if (parsed == null) {
        throw Exception('Failed to parse sub-OFD URL: $subUrl');
      }
      final dirHash = Uint8List.fromList(base58Decode(parsed.fileHashB58)!);
      entries.add(OfdEntry.directory(name: subDirName, dirHash: dirHash));
    }

    // Upload files in this directory with per-file recycler lists
    for (final file in files) {
      final fileName = file.path.split(Platform.pathSeparator).last;
      final length = await file.length();

      // Build per-file recycler list from OFD entries
      final matched = ofdEntryMap[fileName];
      final fileRecyclers = RecyclerResolver.buildRecyclerList(
        matched: matched,
        fileSize: length,
        donorPool: donorPool,
        fallback: fallbackRecyclers,
        serverBase: recyclerServerBase ?? '',
      );

      final url = await _api.uploadFile(
        fileName: fileName,
        streamLength: length,
        filePath: file.path,
        recyclerUrls: fileRecyclers.isNotEmpty ? fileRecyclers : null,
      );

      final parsed = parseOffUrl(url);
      if (parsed == null) {
        throw Exception('Failed to parse upload URL: $url');
      }

      final fileHash = Uint8List.fromList(base58Decode(parsed.fileHashB58)!);
      final descHash =
          Uint8List.fromList(base58Decode(parsed.descriptorHashB58)!);

      entries.add(OfdEntry.file(
        name: fileName,
        fileHash: fileHash,
        descriptorHash: descHash,
        finalByte: parsed.streamLength,
      ));
    }

    if (entries.isEmpty) {
      throw Exception('Empty directory: ${dir.path}');
    }

    // Build and upload this directory's OFD
    final ofdBytes = buildOfdCbor(entries);
    return _api.uploadFileBuffered(
      fileName: '$dirName.ofd',
      streamLength: ofdBytes.length,
      contentType: 'offsystem/directory',
      bodyBytes: ofdBytes,
      recyclerUrls: _recyclerUrls,
    );
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
      final rootDir = Directory(dirResult);
      _progress = 0.5; // indeterminate during uploads
      final finalUrl = await _uploadDirectory(rootDir);

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
          const SizedBox(height: 16),
          TextField(
            controller: _recyclerController,
            decoration: const InputDecoration(
              labelText: 'Recycler URLs (comma-separated)',
              hintText: 'http://host/offsystem/v3/...',
              border: OutlineInputBorder(),
            ),
            onChanged: (value) {
              if (value.trim().isEmpty) {
                _recyclerUrls = null;
              } else {
                _recyclerUrls = value
                    .split(',')
                    .map((url) => url.trim())
                    .where((url) => url.isNotEmpty)
                    .toList();
              }
            },
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