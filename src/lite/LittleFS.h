#pragma once
// ---------------------------------------------------------------------------
// LittleFS no-op shim for the OPENEVSE_LITE build.
//
// Persistence is deferred this slice: the LibreTiny EFM32 fork ships no
// LittleFS (it has Preferences instead). The real EVSE core (event_log.cpp,
// energy_meter.cpp) #include <LittleFS.h> and read/write JSON to flash. Rather
// than modify those library/core sources, this header satisfies their
// LittleFS/File API surface with empty stubs so they compile and link. Every
// operation no-ops: opens return an invalid File (operator bool == false), so
// the core's "if(file)" guards skip all I/O. Nothing is persisted; on-flash
// state is simply never present, which the core already tolerates (treats a
// missing file as "fresh"/defaults).
//
// Slice-2 will replace this with a real backing store (Preferences or a small
// flash region). Until then, gate this header to the lite build only.
// ---------------------------------------------------------------------------
#ifdef OPENEVSE_LITE

#include <Arduino.h>

// File open-mode tokens used by the core (event_log.cpp passes FILE_APPEND).
#ifndef FILE_READ
#define FILE_READ   "r"
#endif
#ifndef FILE_WRITE
#define FILE_WRITE  "w"
#endif
#ifndef FILE_APPEND
#define FILE_APPEND "a"
#endif

// Minimal File stub. Derives from Print so it satisfies ArduinoJson's
// serializeJson(doc, file) writer and the core's file.println() calls. Always
// invalid (operator bool == false) so callers skip all real I/O.
class File : public Print {
public:
  File() {}

  // Print interface (writes are discarded).
  virtual size_t write(uint8_t) override { return 0; }
  virtual size_t write(const uint8_t *, size_t) override { return 0; }

  // Validity: an unmounted FS never yields a usable file.
  explicit operator bool() const { return false; }

  // Read side — all empty.
  int available() { return 0; }
  int read() { return -1; }
  String readString() { return String(); }
  String readStringUntil(char) { return String(); }

  // Metadata / traversal — inert.
  size_t size() { return 0; }
  bool isDirectory() { return false; }
  const char *name() { return ""; }
  File openNextFile() { return File(); }

  void close() {}
};

// Minimal LittleFS object. All operations no-op; opens return invalid Files.
class LittleFSClass {
public:
  bool begin(bool = false) { return false; }
  File open(const char *, const char * = FILE_READ) { return File(); }
  File open(const String &, const char * = FILE_READ) { return File(); }
  bool exists(const char *) { return false; }
  bool exists(const String &) { return false; }
  bool remove(const char *) { return false; }
  bool remove(const String &) { return false; }
  bool mkdir(const char *) { return false; }
  bool mkdir(const String &) { return false; }
  size_t totalBytes() { return 0; }
  size_t usedBytes() { return 0; }
};

extern LittleFSClass LittleFS;

#endif // OPENEVSE_LITE
