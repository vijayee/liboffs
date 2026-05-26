/// Bitcoin-style Base58 encoding/decoding.
/// Matches the C implementation in liboffs/src/Util/base58.c.
library;

const _alphabet = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz';
final _indices = _buildIndices();

List<int> _buildIndices() {
  final indices = List<int>.filled(128, -1);
  for (int i = 0; i < _alphabet.length; i++) {
    indices[_alphabet.codeUnitAt(i)] = i;
  }
  return indices;
}

/// Decode a base58-encoded string into raw bytes.
/// Returns null if the input contains invalid characters.
List<int>? base58Decode(String input) {
  if (input.isEmpty) return null;

  // Count leading '1' characters (each represents a leading zero byte)
  int leadingZeros = 0;
  while (leadingZeros < input.length && input[leadingZeros] == '1') {
    leadingZeros++;
  }

  // Convert from base58 to big-endian bytes
  final bytes = <int>[];
  for (int i = leadingZeros; i < input.length; i++) {
    final codeUnit = input.codeUnitAt(i);
    if (codeUnit >= 128) return null;
    final digit = _indices[codeUnit];
    if (digit < 0) return null;

    int carry = digit;
    for (int j = 0; j < bytes.length; j++) {
      carry += bytes[j] * 58;
      bytes[j] = carry & 0xff;
      carry >>= 8;
    }
    while (carry > 0) {
      bytes.add(carry & 0xff);
      carry >>= 8;
    }
  }

  // Add leading zeros
  for (int i = 0; i < leadingZeros; i++) {
    bytes.add(0);
  }

  // Reverse (big-endian to regular order)
  return bytes.reversed.toList();
}

/// Encode raw bytes into a base58 string.
String base58Encode(List<int> input) {
  if (input.isEmpty) return '';

  // Count leading zeros
  int leadingZeros = 0;
  while (leadingZeros < input.length && input[leadingZeros] == 0) {
    leadingZeros++;
  }

  // Convert to base58 (big-endian arithmetic)
  final resultCodes = <int>[];
  for (int i = leadingZeros; i < input.length; i++) {
    int carry = input[i];
    for (int j = 0; j < resultCodes.length; j++) {
      carry += resultCodes[j] * 256;
      resultCodes[j] = carry % 58;
      carry ~/= 58;
    }
    while (carry > 0) {
      resultCodes.add(carry % 58);
      carry ~/= 58;
    }
  }

  // Add leading '1's
  final result = '1' * leadingZeros;

  // Build output string (reverse order)
  return result + resultCodes.reversed.map((code) => _alphabet[code]).join();
}
