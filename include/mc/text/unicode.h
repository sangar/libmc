#pragma once

#include "mc/text/str.h"

// These functions read their input as UTF-8 and take each malformed byte as
// U+FFFD, as utf8_decode does, so what they return is always valid UTF-8.

// unicode_nfc returns s in Normalization Form C, so a letter spelled as a
// base and a combining mark, "a\xCC\x8A" as in macOS file names, becomes the
// precomposed "å" that typed text uses.
String unicode_nfc(Arena *arena, String s);
// unicode_lower and unicode_upper map each code point by its simple case
// mapping, one code point to one, so upper case "ß" is "ẞ", not "SS".
String unicode_lower(Arena *arena, String s);
String unicode_upper(Arena *arena, String s);
// unicode_equal_ignore_case compares after simple case folding, so "ÆØÅ"
// equals "æøå". It does not normalize: pass text that may be decomposed
// through unicode_nfc first.
bool unicode_equal_ignore_case(String a, String b);

// unicode_compare_norwegian orders by code point, a shorter prefix first, as
// str_compare does, except that Æ, Ø and Å follow Z and æ, ø and å follow z,
// the Danish and Norwegian alphabet: "a" < "z" < "æ" < "ø" < "å". Like
// unicode_equal_ignore_case, it expects text in NFC.
int unicode_compare_norwegian(String a, String b);
// unicode_compare_norwegian_ignore_case orders as unicode_compare_norwegian
// would after simple case folding, so "Øl" sorts between "zebra" and "ål".
int unicode_compare_norwegian_ignore_case(String a, String b);
// unicode_sort_norwegian sorts by unicode_compare_norwegian_ignore_case and
// puts strings that differ only in case in unicode_compare_norwegian order.
void unicode_sort_norwegian(StringList *list);
