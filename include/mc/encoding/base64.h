#pragma once

#include "mc/text/str.h"

// Base64Alphabet picks one of RFC 4648's encodings: the standard alphabet
// with = padding, or the URL and file name safe alphabet without padding,
// which JOSE and ACME use.
typedef enum Base64Alphabet { BASE64_STANDARD, BASE64_URL } Base64Alphabet;

// base64_encode writes data in alphabet, in the arena.
String base64_encode(Arena *arena, String data, Base64Alphabet alphabet);

// base64_decode reads text written in alphabet into *data in the arena and
// reports whether it is the canonical encoding of some bytes: characters of
// that alphabet only, padded exactly when the alphabet pads, and with no
// bits set past the last byte. No whitespace is skipped.
bool base64_decode(Arena *arena, String text, Base64Alphabet alphabet, String *data);
