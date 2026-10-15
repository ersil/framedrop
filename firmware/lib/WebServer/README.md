# WebServer library (patched)

A copy of the WebServer library from the Arduino-ESP32 core used by this
build (framework-arduinoespressif32 4.20017.260907, Arduino core 2.0.17).
PlatformIO builds a library in the project's `lib/` folder in place of the
framework's copy of the same name.

All changes are in `src/Parsing.cpp`, marked `photoframe:`. Each closes a way
for any client on the network, before authentication, to hang or crash the
board:

- `_uploadReadByte()` gives up after `HTTP_MAX_POST_WAIT` (5 s) without a
  byte. The stock loop waited as long as the client stayed connected, so a
  client that went silent mid-upload hung loopTask until a power cycle.
- A multipart boundary longer than 70 characters (the RFC 2046 limit) is
  refused. The parser puts a buffer of that length on the stack.
- More than 32 query or form arguments are refused. The count sizes one
  `new[]`, and a failed allocation throws `bad_alloc`, which aborts.
- Request lines over 2048 bytes and non-multipart bodies over 8 KB are
  refused before they are buffered and copied.
