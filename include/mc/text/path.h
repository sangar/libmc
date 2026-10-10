#pragma once

#include "mc/text/str.h"

// Slash-separated paths, handled lexically: nothing here touches the file
// system. The functions without an Arena return views into their argument.

bool path_is_absolute(String path);
// path_base is the last element without trailing slashes: "." for an empty path, "/" for the root.
String path_base(String path);
// path_dir is everything before the last slash: "." without one, "/" for a top-level path.
String path_dir(String path);
// path_ext is the suffix from the last dot of the last element, or empty.
String path_ext(String path);
// path_relative views path below root; false when path is root itself or outside it.
bool path_relative(String root, String path, String *relative);

// path_join puts exactly one slash between dir and name.
String path_join(Arena *arena, String dir, String name);
// path_clean resolves . and .. and repeated slashes, as Go's path.Clean does.
String path_clean(Arena *arena, String path);
// path_expand_home replaces a leading "~" or "~/" with home.
String path_expand_home(Arena *arena, String path, String home);
