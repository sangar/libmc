#pragma once

#include "mc/text/str.h"

// roots_file is the bundle of trusted roots a client uses: ca_file when given,
// else the system's, http_default_ca_file; empty means Mozilla's roots built
// into libmc, for systems that have none.
String roots_file(String ca_file);

// roots_set gives a curl easy handle the bundle at ca_file, or Mozilla's
// roots when it is null. It takes void *, as curl.h defines CURL, so this
// header stays free of curl's.
void roots_set(void *curl, const char *ca_file);
