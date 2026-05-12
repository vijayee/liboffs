// Conditional file download implementation.
// Desktop streams directly to disk; web triggers a browser-native download.
//
export 'file_download_stub.dart'
    if (dart.library.io) 'file_download_io.dart'
    if (dart.library.html) 'file_download_web.dart';
