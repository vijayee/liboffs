import 'package:flutter/material.dart';

class ConnectScreen extends StatefulWidget {
  const ConnectScreen({super.key});

  @override
  State<ConnectScreen> createState() => _ConnectScreenState();
}

class _ConnectScreenState extends State<ConnectScreen> {
  final TextEditingController _locatorController = TextEditingController();
  String? _status;
  bool _isConnecting = false;

  Future<void> _connect() async {
    final locator = _locatorController.text.trim();
    if (locator.isEmpty) return;

    setState(() {
      _isConnecting = true;
      _status = null;
    });

    // TODO: Implement P2P connection via the OFF network RPC protocol
    await Future.delayed(const Duration(seconds: 1));
    setState(() {
      _isConnecting = false;
      _status = 'Connected to peer: $locator';
    });
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
          const Text('Connect to Peer', style: TextStyle(fontSize: 24, fontWeight: FontWeight.bold)),
          const SizedBox(height: 24),
          TextField(
            controller: _locatorController,
            decoration: const InputDecoration(
              labelText: 'Locator',
              hintText: 'Enter peer locator (Base58)',
              prefixIcon: Icon(Icons.language),
              border: OutlineInputBorder(),
            ),
          ),
          const SizedBox(height: 16),
          ElevatedButton(
            onPressed: _locatorController.text.isNotEmpty && !_isConnecting ? _connect : null,
            style: ElevatedButton.styleFrom(
              backgroundColor: const Color(0xFF313181),
              foregroundColor: Colors.white,
            ),
            child: _isConnecting
                ? const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2, color: Colors.white))
                : const Text('Connect'),
          ),
          if (_status != null) ...[
            const SizedBox(height: 16),
            Container(
              padding: const EdgeInsets.all(12),
              decoration: BoxDecoration(
                color: Colors.blue.shade900.withOpacity(0.3),
                borderRadius: BorderRadius.circular(8),
                border: Border.all(color: Colors.blue),
              ),
              child: Text(_status!),
            ),
          ],
        ],
      ),
    );
  }
}