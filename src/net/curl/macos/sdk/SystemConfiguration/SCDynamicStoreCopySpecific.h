// The SystemConfiguration and CoreFoundation calls curl's lib/macos.c makes,
// for cross builds without the macOS SDK. The build searches this directory
// with -idirafter, so the SDK's own header wins wherever it is installed.
#pragma once

typedef const void *CFTypeRef;
typedef const struct __CFDictionary *CFDictionaryRef;
typedef const struct __SCDynamicStore *SCDynamicStoreRef;

CFDictionaryRef SCDynamicStoreCopyProxies(SCDynamicStoreRef store);
void CFRelease(CFTypeRef cf);
