import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:file_picker/file_picker.dart';

import '../services/off_api.dart';

class ConnectScreen extends StatefulWidget {
  final OffApi api;
  const ConnectScreen({super.key, required this.api});

  @override
  State<ConnectScreen> createState() => _ConnectScreenState();
}

class _ConnectScreenState extends State<ConnectScreen> {
  final TextEditingController _locatorController = TextEditingController();
  String? _status;
  bool _isConnecting = false;
  String? _resultJson;

  Future<void> _connectViaBase58() async {
    final locator = _locatorController.text.trim();
    if (locator.isEmpty) return;
    setState(() {
      _isConnecting = true;
      _status = null;
      _resultJson = null;
    });
    try {
      final result = await widget.api.connectPeerBase58(locator);
      setState(() {
        _status = 'Connected';
        _resultJson = result;
      });
    } catch (error) {
      setState(() {
        _status = 'Connection failed: $error';
      });
    } finally {
      setState(() => _isConnecting = false);
    }
  }

  Future<void> _uploadAndConnect() async {
    final result =
        await FilePicker.platform.pickFiles(type: FileType.image, withData: true);
    if (result == null || result.files.isEmpty) return;
    setState(() {
      _isConnecting = true;
      _status = null;
      _resultJson = null;
    });
    try {
      final bytes = result.files.first.bytes!;
      final connectResult = await widget.api.connectPeer(bytes);
      setState(() {
        _status = 'Connected';
        _resultJson = utf8.decode(connectResult.codeUnits);
      });
    } catch (error) {
      setState(() {
        _status = 'Connection failed: $error';
      });
    } finally {
      setState(() => _isConnecting = false);
    }
  }

  @override
  void dispose() {
    _locatorController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.all(24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          const Text(
            'Connect to Peer',
            style: TextStyle(fontSize: 24, fontWeight: FontWeight.bold),
          ),
          const SizedBox(height: 24),
          TextField(
            controller: _locatorController,
            enabled: !_isConnecting,
            decoration: const InputDecoration(
              labelText: 'Locator',
              hintText: 'Enter peer locator (Base58)',
              prefixIcon: Icon(Icons.language),
              border: OutlineInputBorder(),
            ),
          ),
          const SizedBox(height: 12),
          ElevatedButton.icon(
            onPressed:
                _locatorController.text.isNotEmpty && !_isConnecting
                    ? _connectViaBase58
                    : null,
            icon: const Icon(Icons.link),
            label: const Text('Connect via Base58'),
            style: ElevatedButton.styleFrom(
              backgroundColor: const Color(0xFF313181),
              foregroundColor: Colors.white,
              minimumSize: const Size.fromHeight(48),
            ),
          ),
          const SizedBox(height: 8),
          ElevatedButton.icon(
            onPressed: _isConnecting ? null : _uploadAndConnect,
            icon: const Icon(Icons.qr_code_scanner),
            label: const Text('Upload QR Image'),
            style: ElevatedButton.styleFrom(
              backgroundColor: const Color(0xFF313181),
              foregroundColor: Colors.white,
              minimumSize: const Size.fromHeight(48),
            ),
          ),
          if (_isConnecting) ...[
            const SizedBox(height: 16),
            const LinearProgressIndicator(),
          ],
          if (_status != null) ...[
            const SizedBox(height: 16),
            Container(
              padding: const EdgeInsets.all(12),
              decoration: BoxDecoration(
                color: Colors.blue.shade900.withValues(alpha: 0.3),
                borderRadius: BorderRadius.circular(8),
                border: Border.all(color: Colors.blue),
              ),
              child: Text(_status!),
            ),
          ],
          if (_resultJson != null) ...[
            const SizedBox(height: 12),
            Container(
              width: double.infinity,
              padding: const EdgeInsets.all(12),
              decoration: BoxDecoration(
                color: Colors.grey.shade900.withValues(alpha: 0.3),
                borderRadius: BorderRadius.circular(8),
                border: Border.all(color: Colors.grey),
              ),
              child: SingleChildScrollView(
                scrollDirection: Axis.horizontal,
                child: SelectableText(
                  _resultJson!,
                  style: const TextStyle(
                    fontFamily: 'monospace',
                    fontSize: 12,
                  ),
                ),
              ),
            ),
          ],
        ],
      ),
    );
  }
}
