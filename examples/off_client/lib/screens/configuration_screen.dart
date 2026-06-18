import 'package:flutter/material.dart';

import '../services/off_api.dart';

class ConfigurationScreen extends StatefulWidget {
  final OffApi api;
  const ConfigurationScreen({super.key, required this.api});

  @override
  State<ConfigurationScreen> createState() => _ConfigurationScreenState();
}

class _ConfigurationScreenState extends State<ConfigurationScreen> {
  final _baseUrlController = TextEditingController();
  final _apiKeyController = TextEditingController();
  final _startPortController = TextEditingController(text: '8200');
  final _portRetriesController = TextEditingController(text: '2');
  final _httpPortController = TextEditingController(text: '23402');
  final _socketTimeoutController = TextEditingController(text: '120000');
  final _cacheLocationController = TextEditingController(text: '~/.offs/OFFSYSTEM');
  final _blockCacheSizeController = TextEditingController(text: '10000');
  final _miniCacheSizeController = TextEditingController(text: '10000');
  final _nanoCacheSizeController = TextEditingController(text: '10000');
  final List<_PeerEntry> _peers = [];

  @override
  void initState() {
    super.initState();
    _baseUrlController.text = widget.api.baseUrl;
  }

  @override
  void dispose() {
    _baseUrlController.dispose();
    _apiKeyController.dispose();
    _startPortController.dispose();
    _portRetriesController.dispose();
    _httpPortController.dispose();
    _socketTimeoutController.dispose();
    _cacheLocationController.dispose();
    _blockCacheSizeController.dispose();
    _miniCacheSizeController.dispose();
    _nanoCacheSizeController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.all(24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          const Text('Configuration', style: TextStyle(fontSize: 24, fontWeight: FontWeight.bold)),
          const SizedBox(height: 24),
          Expanded(
            child: DefaultTabController(
              length: 3,
              child: Column(
                children: [
                  const TabBar(
                    tabs: [
                      Tab(text: 'Bootstrap'),
                      Tab(text: 'Network'),
                      Tab(text: 'Storage'),
                    ],
                  ),
                  const SizedBox(height: 16),
                  Expanded(
                    child: TabBarView(
                      children: [
                        _buildBootstrapTab(),
                        _buildNetworkTab(),
                        _buildStorageTab(),
                      ],
                    ),
                  ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildBootstrapTab() {
    return Column(
      children: [
        Expanded(
          child: _peers.isEmpty
              ? const Center(child: Text('No bootstrap peers configured', style: TextStyle(color: Colors.grey)))
              : ListView.builder(
                  itemCount: _peers.length,
                  itemBuilder: (context, index) {
                    final peer = _peers[index];
                    return ListTile(
                      title: Text(peer.nodeId),
                      subtitle: Text('${peer.ipAddress}:${peer.port}'),
                      trailing: IconButton(
                        icon: const Icon(Icons.close, size: 18),
                        onPressed: () => setState(() => _peers.removeAt(index)),
                      ),
                    );
                  },
                ),
        ),
        const Divider(),
        Row(
          children: [
            Expanded(
              child: TextField(
                decoration: const InputDecoration(
                  labelText: 'Locator',
                  hintText: 'Enter peer locator (Base58)',
                  border: OutlineInputBorder(),
                ),
                onSubmitted: (value) {
                  if (value.isNotEmpty) {
                    setState(() {
                      _peers.add(_PeerEntry(nodeId: value.substring(0, value.length > 8 ? 8 : value.length), ipAddress: '0.0.0.0', port: 8200));
                    });
                  }
                },
              ),
            ),
            const SizedBox(width: 8),
            ElevatedButton(onPressed: () {}, child: const Text('Add')),
          ],
        ),
      ],
    );
  }

  Widget _buildNetworkTab() {
    return ListView(
      children: [
        TextFormField(controller: _baseUrlController, decoration: const InputDecoration(labelText: 'Base URL', border: OutlineInputBorder())),
        const SizedBox(height: 16),
        TextFormField(controller: _apiKeyController, decoration: const InputDecoration(labelText: 'API Key', border: OutlineInputBorder()), obscureText: true),
        const SizedBox(height: 16),
        TextFormField(controller: _startPortController, decoration: const InputDecoration(labelText: 'Start Port (1024-65535)'), keyboardType: TextInputType.number),
        const SizedBox(height: 16),
        TextFormField(controller: _portRetriesController, decoration: const InputDecoration(labelText: 'Port Retries'), keyboardType: TextInputType.number),
        const SizedBox(height: 16),
        TextFormField(controller: _httpPortController, decoration: const InputDecoration(labelText: 'HTTP Port (1-65535)'), keyboardType: TextInputType.number),
        const SizedBox(height: 16),
        TextFormField(controller: _socketTimeoutController, decoration: const InputDecoration(labelText: 'Socket Timeout (ms, min 30000)'), keyboardType: TextInputType.number),
        const SizedBox(height: 24),
        ElevatedButton(
          onPressed: () {
            final baseUrl = _baseUrlController.text.trim();
            final apiKey = _apiKeyController.text.trim();
            if (baseUrl.isEmpty) {
              ScaffoldMessenger.of(context).showSnackBar(
                const SnackBar(content: Text('Base URL cannot be empty')),
              );
              return;
            }
            widget.api.setBaseUrl(baseUrl);
            widget.api.setApiKey(apiKey.isEmpty ? null : apiKey);
            ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Network settings saved')));
          },
          style: ElevatedButton.styleFrom(backgroundColor: const Color(0xFF313181), foregroundColor: Colors.white),
          child: const Text('Save'),
        ),
      ],
    );
  }

  Widget _buildStorageTab() {
    return ListView(
      children: [
        TextField(controller: _cacheLocationController, decoration: const InputDecoration(labelText: 'Cache Location', border: OutlineInputBorder())),
        const SizedBox(height: 16),
        TextFormField(controller: _blockCacheSizeController, decoration: const InputDecoration(labelText: 'Block Cache Storage Size (MB)'), keyboardType: TextInputType.number),
        const SizedBox(height: 16),
        TextFormField(controller: _miniCacheSizeController, decoration: const InputDecoration(labelText: 'Mini Block Cache Storage Size (MB)'), keyboardType: TextInputType.number),
        const SizedBox(height: 16),
        TextFormField(controller: _nanoCacheSizeController, decoration: const InputDecoration(labelText: 'Nano Block Cache Storage Size (MB)'), keyboardType: TextInputType.number),
        const SizedBox(height: 24),
        ElevatedButton(
          onPressed: () {
            ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Storage settings saved')));
          },
          style: ElevatedButton.styleFrom(backgroundColor: const Color(0xFF313181), foregroundColor: Colors.white),
          child: const Text('Save'),
        ),
      ],
    );
  }
}

class _PeerEntry {
  final String nodeId;
  final String ipAddress;
  final int port;

  _PeerEntry({required this.nodeId, required this.ipAddress, required this.port});
}