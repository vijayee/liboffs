import 'dart:typed_data';
import 'package:cbor/cbor.dart';
import 'base58.dart';

/// Parsed information extracted from an offs:// URL.
class ParsedOffUrl {
  final String fileHashB58;
  final String descriptorHashB58;
  final int streamLength;
  final String fileName;

  const ParsedOffUrl({
    required this.fileHashB58,
    required this.descriptorHashB58,
    required this.streamLength,
    required this.fileName,
  });
}

/// An entry in an OFD (OFF File Descriptor).
class OfdEntry {
  final String name;
  final bool isDirectory;

  // File fields
  final Uint8List? fileHash;
  final Uint8List? descriptorHash;
  final int? finalByte;
  final int? blockType;
  final int? tupleSize;
  final int? fileOffset;

  // Directory fields
  final Uint8List? dirHash;

  const OfdEntry._({
    required this.name,
    required this.isDirectory,
    this.fileHash,
    this.descriptorHash,
    this.finalByte,
    this.blockType,
    this.tupleSize,
    this.fileOffset,
    this.dirHash,
  });

  factory OfdEntry.file({
    required String name,
    required Uint8List fileHash,
    required Uint8List descriptorHash,
    required int finalByte,
    int blockType = 128000,
    int tupleSize = 3,
    int fileOffset = 0,
  }) {
    return OfdEntry._(
      name: name,
      isDirectory: false,
      fileHash: fileHash,
      descriptorHash: descriptorHash,
      finalByte: finalByte,
      blockType: blockType,
      tupleSize: tupleSize,
      fileOffset: fileOffset,
    );
  }

  factory OfdEntry.directory({
    required String name,
    required Uint8List dirHash,
  }) {
    return OfdEntry._(
      name: name,
      isDirectory: true,
      dirHash: dirHash,
    );
  }
}

/// Parse an offs URL to extract file hash and metadata.
/// Format: http://host/offsystem/v3/<type>/<length>/<b58_hash1>/<b58_hash2>/<name>
ParsedOffUrl? parseOffUrl(String url) {
  final prefixIndex = url.indexOf('/offsystem/v3/');
  if (prefixIndex < 0) return null;

  final afterPrefix = url.substring(prefixIndex + '/offsystem/v3/'.length);
  final allParts = afterPrefix.split('/');
  if (allParts.length < 4) return null;

  // Parse from the end: last 4 segments are stream_length, hash1, hash2, filename
  final streamLengthStr = allParts[allParts.length - 4];
  final fileHashB58 = allParts[allParts.length - 3];
  final descriptorHashB58 = allParts[allParts.length - 2];
  final fileName = allParts.sublist(allParts.length - 1).join('/');

  final streamLength = int.tryParse(streamLengthStr);
  if (streamLength == null) return null;

  // Validate base58 hashes
  if (base58Decode(fileHashB58) == null) return null;
  if (base58Decode(descriptorHashB58) == null) return null;

  return ParsedOffUrl(
    fileHashB58: fileHashB58,
    descriptorHashB58: descriptorHashB58,
    streamLength: streamLength,
    fileName: Uri.decodeComponent(fileName),
  );
}

/// Build a CBOR-encoded OFD from a list of entries.
Uint8List buildOfdCbor(List<OfdEntry> entries) {
  final entryMaps = <CborMap>[];

  for (final entry in entries) {
    final map = <CborValue, CborValue>{};

    map[CborString('n')] = CborString(entry.name);
    map[CborString('t')] = CborSmallInt(entry.isDirectory ? 1 : 0);

    if (entry.isDirectory) {
      map[CborString('d')] = CborBytes(entry.dirHash!);
    } else {
      map[CborString('f')] = CborBytes(entry.fileHash!);
      if (entry.descriptorHash != null) {
        map[CborString('D')] = CborBytes(entry.descriptorHash!);
      }
      map[CborString('s')] = CborSmallInt(entry.finalByte!);
      map[CborString('B')] = CborSmallInt(entry.blockType!);
      map[CborString('T')] = CborSmallInt(entry.tupleSize!);
      map[CborString('o')] = CborSmallInt(entry.fileOffset!);
    }

    entryMaps.add(CborMap(map));
  }

  // CborString is abstract (cbor-6.5.1) and has no const constructor; the
  // entries list is therefore runtime-built.
  final root = CborMap({
    // ignore: prefer_const_constructors
    CborString('v'): CborSmallInt(1),
    CborString('entries'): CborList(entryMaps),
  });

  return Uint8List.fromList(cborEncode(root));
}

int _safeInt(dynamic value) {
  if (value is int) return value;
  if (value is BigInt) return value.toInt();
  return 0;
}

/// Parse CBOR-encoded OFD data into a list of entries.
List<OfdEntry> parseOfdCbor(Uint8List data) {
  final decoded = cborDecode(data);
  final object = decoded.toObject();
  if (object is! Map) return [];

  final entries = object['entries'];
  if (entries is! List) return [];

  final result = <OfdEntry>[];

  for (final entry in entries) {
    if (entry is! Map) continue;

    final name = entry['n'];
    if (name is! String) continue;

    final typeVal = entry['t'];
    final isDir = typeVal == 1;

    if (isDir) {
      final dirHash = entry['d'];
      if (dirHash is List<int>) {
        result.add(OfdEntry.directory(
          name: name,
          dirHash: Uint8List.fromList(dirHash),
        ));
      }
    } else {
      final fileHash = entry['f'];
      if (fileHash is! List<int>) continue;

      final descHash = entry['D'];
      if (descHash is! List<int>) continue; // file entries require descriptor_hash
      final sizeVal = entry['s'];
      final btypeVal = entry['B'];
      final tsizeVal = entry['T'];
      final offsetVal = entry['o'];

      result.add(OfdEntry.file(
        name: name,
        fileHash: Uint8List.fromList(fileHash),
        descriptorHash: Uint8List.fromList(descHash),
        finalByte: _safeInt(sizeVal),
        blockType: _safeInt(btypeVal),
        tupleSize: _safeInt(tsizeVal),
        fileOffset: _safeInt(offsetVal),
      ));
    }
  }

  return result;
}
