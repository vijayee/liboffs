import 'dart:typed_data';
import 'package:flutter_test/flutter_test.dart';
import 'package:off_client/services/recycler_resolver.dart';
import 'package:off_client/services/ofd.dart';

const serverBase = 'http://example.com:23402';

void main() {
  group('totalBlocks', () {
    test('small file (2KB, standard)', () {
      final blocks = RecyclerResolver.totalBlocks(2048);
      // data=1 + desc=1 = 2
      expect(blocks, 2);
    });

    test('zero byte file', () {
      final blocks = RecyclerResolver.totalBlocks(0);
      expect(blocks, 0);
    });

    test('large file (10MB, standard)', () {
      // data=ceil(10M/128K)=79, desc=ceil(79*96/127968)=1 = 80
      final blocks = RecyclerResolver.totalBlocks(10000000);
      expect(blocks, 80);
    });
  });

  group('buildRecyclerList', () {
    final hash1 = Uint8List(32)..[0] = 1;
    final hash2 = Uint8List(32)..[0] = 2;
    final hash3 = Uint8List(32)..[0] = 3;
    final descHash = Uint8List(32)..[0] = 0xFF;

    test('exact match -- no donors consumed', () {
      final matched = OfdEntry.file(
        name: 'test.js',
        finalByte: 2048,
        fileHash: hash1,
        descriptorHash: descHash,
      );
      final result = RecyclerResolver.buildRecyclerList(
        matched: matched,
        fileSize: 2048,
        donorPool: [],
        fallback: [],
        serverBase: serverBase,
      );
      expect(result.length, 1);
      expect(result[0], contains(serverBase));
      expect(result[0], contains('/2048/'));
    });

    test('file grew -- donors consumed', () {
      final matched = OfdEntry.file(
        name: 'main.js',
        finalByte: 2048,
        fileHash: hash1,
        descriptorHash: descHash,
      );
      final donors = [
        OfdEntry.file(
          name: 'old.js',
          finalByte: 128000,
          fileHash: hash2,
          descriptorHash: descHash,
        ),
        OfdEntry.file(
          name: 'lib.js',
          finalByte: 256000,
          fileHash: hash3,
          descriptorHash: descHash,
        ),
      ];
      // 256KB file: blocks_needed = 2+1=3, matched covers 2, need 1 more
      final result = RecyclerResolver.buildRecyclerList(
        matched: matched,
        fileSize: 256000,
        donorPool: donors,
        fallback: [],
        serverBase: serverBase,
      );
      expect(result.length, 2); // matched + 1 donor
      expect(result[0], contains('/2048/')); // matched is first
    });

    test('no match -- uses donor pool', () {
      final donors = [
        OfdEntry.file(
          name: 'other.js',
          finalByte: 128000,
          fileHash: hash2,
          descriptorHash: descHash,
        ),
      ];
      final fallback = ['http://fallback/ofd'];
      final result = RecyclerResolver.buildRecyclerList(
        matched: null,
        fileSize: 50000,
        donorPool: donors,
        fallback: fallback,
        serverBase: serverBase,
      );
      expect(result.length, 2); // donor + fallback
      expect(result.last, 'http://fallback/ofd');
    });

    test('no donors -- fallback only', () {
      final fallback = ['http://fb/ofd'];
      final result = RecyclerResolver.buildRecyclerList(
        matched: null,
        fileSize: 50000,
        donorPool: [],
        fallback: fallback,
        serverBase: serverBase,
      );
      expect(result.length, 1);
      expect(result[0], 'http://fb/ofd');
    });

    test('self-skip -- matched donor not duplicated', () {
      final matched = OfdEntry.file(
        name: 'app.js',
        finalByte: 2048,
        fileHash: hash1,
        descriptorHash: descHash,
      );
      // Donor pool contains the same entry as the matched
      final donors = [
        OfdEntry.file(
          name: 'app.js',
          finalByte: 2048,
          fileHash: hash1,
          descriptorHash: descHash,
        ),
      ];
      final result = RecyclerResolver.buildRecyclerList(
        matched: matched,
        fileSize: 256000,
        donorPool: donors,
        fallback: [],
        serverBase: serverBase,
      );
      // matched + 0 donors (the one donor is self, skipped)
      // matched covers 2 blocks, needs 3 -- shortfall, but no usable donors
      expect(result.length, 1);
    });
  });
}
