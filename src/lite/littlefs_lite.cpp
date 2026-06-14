#ifdef OPENEVSE_LITE
// Storage for the no-op LittleFS shim object (see src/lite/LittleFS.h).
// Persistence is deferred this slice; every method no-ops.
#include "LittleFS.h"

LittleFSClass LittleFS;

#endif // OPENEVSE_LITE
