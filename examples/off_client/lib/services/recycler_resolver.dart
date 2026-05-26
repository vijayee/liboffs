import 'dart:typed_data';
import 'off_api.dart';
import 'ofd.dart';
import 'base58.dart';

/// Result from resolving an OFD recycler URL.
class ResolvedOfd {
  final String serverBase;
  final List<OfdEntry> entries;
  const ResolvedOfd({required this.serverBase, required this.entries});
}

/// Resolves OFD recycler URLs into per-file recycler lists.
class RecyclerResolver {
  final OffApi _api;

  RecyclerResolver(this._api);

  /// Fetch and parse an OFD, returning all file entries with the server base.
  /// Recursively resolves sub-directory OFDs.
  Future<ResolvedOfd> resolveOfd(String ofdUrl) async {
    final donorPool = <OfdEntry>[];
    final rawBytes = await _api.fetchRawOfd(ofdUrl);
    final entries = parseOfdCbor(rawBytes);

    final uri = Uri.parse(ofdUrl);
    final serverBase =
        '${uri.scheme}://${uri.host}${uri.hasPort ? ':${uri.port}' : ''}';

    await _collectDonors(entries, serverBase, donorPool);
    return ResolvedOfd(serverBase: serverBase, entries: donorPool);
  }

  /// Recursively collect file ORIs from OFD entries into the donor pool.
  Future<void> _collectDonors(
    List<OfdEntry> entries,
    String serverBase,
    List<OfdEntry> donorPool,
  ) async {
    for (final entry in entries) {
      if (entry.isDirectory) {
        if (entry.dirHash == null) continue;
        final dirHashB58 = base58Encode(entry.dirHash!);
        final subUrl = '$serverBase/offsystem/v3/offsystem/directory/0/'
            '$dirHashB58/$dirHashB58/${Uri.encodeComponent(entry.name)}.ofd';

        try {
          final subBytes = await _api.fetchRawOfd(subUrl);
          final subEntries = parseOfdCbor(subBytes);
          await _collectDonors(subEntries, serverBase, donorPool);
        } catch (_) {
          // Sub-OFD fetch failed — skip this subtree
        }
      } else {
        if (entry.fileHash != null && entry.descriptorHash != null) {
          donorPool.add(entry);
        }
      }
    }
  }

  /// Calculate total blocks (data + descriptor) for a file size.
  static int totalBlocks(int finalByte,
      {int blockType = 128000, int tupleSize = 3, int descriptorPad = 32}) {
    if (finalByte == 0) return 0;
    final dataBlocks = (finalByte + blockType - 1) ~/ blockType;
    final cutPoint = (blockType ~/ descriptorPad) * descriptorPad;
    final descDataPerBlock = cutPoint - descriptorPad;
    final descBytes = dataBlocks * descriptorPad * tupleSize;
    final descBlocks = (descBytes + descDataPerBlock - 1) ~/ descDataPerBlock;
    return dataBlocks + descBlocks;
  }

  /// Get the block count for an OfdEntry.
  static int entryBlocks(OfdEntry entry) {
    return totalBlocks(
      entry.finalByte ?? 0,
      blockType: entry.blockType ?? 128000,
      tupleSize: entry.tupleSize ?? 3,
    );
  }

  /// Build a per-file recycler URL list.
  ///
  /// [matched] is the OFD entry matching this file by name, or null.
  /// [fileSize] is the size of the local file being uploaded.
  /// [donorPool] contains all available donor entries from the OFD tree.
  /// [fallback] contains original non-OFD recycler URLs.
  /// [serverBase] is the base URL of the server, e.g. "http://host:23402".
  static List<String> buildRecyclerList({
    required OfdEntry? matched,
    required int fileSize,
    required List<OfdEntry> donorPool,
    required List<String> fallback,
    required String serverBase,
  }) {
    final result = <String>[];
    final blockType = matched?.blockType ?? 128000;
    final tupleSize = matched?.tupleSize ?? 3;
    final blocksNeeded =
        totalBlocks(fileSize, blockType: blockType, tupleSize: tupleSize);
    int blocksCovered = 0;

    // 1. Matched ORI
    if (matched != null) {
      result.add(_entryToUrl(matched, serverBase));
      blocksCovered += entryBlocks(matched);
    }

    // 2. Greedy donors sorted by block count descending
    if (blocksCovered < blocksNeeded && donorPool.isNotEmpty) {
      final sorted = List<OfdEntry>.from(donorPool)
        ..sort((a, b) => entryBlocks(b).compareTo(entryBlocks(a)));

      for (final donor in sorted) {
        if (blocksCovered >= blocksNeeded) break;
        if (matched != null &&
            _sameHash(donor.fileHash, matched.fileHash)) {
          continue;
        }
        result.add(_entryToUrl(donor, serverBase));
        blocksCovered += entryBlocks(donor);
      }
    }

    // 3. Fallback recyclers
    result.addAll(fallback);
    return result;
  }

  /// Convert an OfdEntry to a full ORI URL string.
  static String _entryToUrl(OfdEntry entry, String serverBase) {
    final fileHashB58 = base58Encode(entry.fileHash!);
    final descHashB58 = base58Encode(entry.descriptorHash!);
    final contentType = mimeFromExtension(entry.name);
    return '$serverBase/offsystem/v3/$contentType/${entry.finalByte}/'
        '$fileHashB58/$descHashB58/${Uri.encodeComponent(entry.name)}';
  }

  static bool _sameHash(Uint8List? a, Uint8List? b) {
    if (a == null || b == null) return false;
    if (a.length != b.length) return false;
    for (int i = 0; i < a.length; i++) {
      if (a[i] != b[i]) return false;
    }
    return true;
  }
}
