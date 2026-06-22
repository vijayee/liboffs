import 'package:flutter/material.dart';
import 'screens/import_screen.dart';
import 'screens/export_screen.dart';
import 'screens/connect_screen.dart';
import 'screens/configuration_screen.dart';
import 'services/off_api.dart';

class OffApp extends StatelessWidget {
  const OffApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'OFF System',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF313181),
          brightness: Brightness.dark,
        ),
        useMaterial3: true,
        fontFamily: 'Odin',
      ),
      home: const AppShell(),
    );
  }
}

class AppShell extends StatefulWidget {
  const AppShell({super.key});

  @override
  State<AppShell> createState() => _AppShellState();
}

class _AppShellState extends State<AppShell> {
  int _selectedIndex = 0;

  final OffApi _api = OffApi();

  List<Widget> get _screens => [
        ImportScreen(api: _api),
        const ExportScreen(),
        ConnectScreen(api: _api),
        ConfigurationScreen(api: _api),
      ];

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('OFF System'),
        backgroundColor: Theme.of(context).colorScheme.primary,
        foregroundColor: Colors.white,
      ),
      drawer: NavigationDrawer(
        selectedIndex: _selectedIndex,
        onDestinationSelected: (index) {
          setState(() => _selectedIndex = index);
          Navigator.pop(context);
        },
        children: const [
          Padding(
            padding: EdgeInsets.symmetric(vertical: 16),
            child: Image(
              image: AssetImage('assets/images/off-logo-lettered.svg'),
              height: 80,
            ),
          ),
          NavigationDrawerDestination(
            icon: Icon(Icons.upload),
            label: Text('Import'),
          ),
          NavigationDrawerDestination(
            icon: Icon(Icons.download),
            label: Text('Export'),
          ),
          NavigationDrawerDestination(
            icon: Icon(Icons.link),
            label: Text('Connect'),
          ),
          NavigationDrawerDestination(
            icon: Icon(Icons.settings),
            label: Text('Configuration'),
          ),
        ],
      ),
      body: _screens[_selectedIndex],
    );
  }
}