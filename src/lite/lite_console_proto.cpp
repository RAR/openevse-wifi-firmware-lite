// Pure WebSocket protocol helpers — see lite_console_proto.h. No Arduino deps so
// the native doctest env can verify the handshake key + frame framing byte-exact.
#include "lite_console_proto.h"
#include <string.h>

// ---- SHA-1 (RFC 3174) -----------------------------------------------------
// Self-contained so we depend on no platform crypto (LibreTiny ships no
// guaranteed mbedTLS for the silabs core). Only used on the short handshake
// string, so speed is irrelevant.
namespace {

struct Sha1 {
  uint32_t h[5];
  uint64_t len;          // total message length in bytes
  uint8_t  block[64];
  size_t   blockLen;
};

inline uint32_t rotl(uint32_t v, int c) { return (v << c) | (v >> (32 - c)); }

void sha1_init(Sha1 &s) {
  s.h[0] = 0x67452301; s.h[1] = 0xEFCDAB89; s.h[2] = 0x98BADCFE;
  s.h[3] = 0x10325476; s.h[4] = 0xC3D2E1F0;
  s.len = 0; s.blockLen = 0;
}

void sha1_process(Sha1 &s, const uint8_t *p) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 |
           (uint32_t)p[i*4+2] << 8 | (uint32_t)p[i*4+3];
  for (int i = 16; i < 80; i++)
    w[i] = rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

  uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3], e = s.h[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if      (i < 20) { f = (b & c) | (~b & d);            k = 0x5A827999; }
    else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDC; }
    else             { f = b ^ c ^ d;                     k = 0xCA62C1D6; }
    uint32_t tmp = rotl(a, 5) + f + e + k + w[i];
    e = d; d = c; c = rotl(b, 30); b = a; a = tmp;
  }
  s.h[0] += a; s.h[1] += b; s.h[2] += c; s.h[3] += d; s.h[4] += e;
}

void sha1_update(Sha1 &s, const uint8_t *data, size_t n) {
  s.len += n;
  while (n) {
    size_t take = 64 - s.blockLen;
    if (take > n) take = n;
    memcpy(s.block + s.blockLen, data, take);
    s.blockLen += take; data += take; n -= take;
    if (s.blockLen == 64) { sha1_process(s, s.block); s.blockLen = 0; }
  }
}

void sha1_final(Sha1 &s, uint8_t out[20]) {
  uint64_t bits = s.len * 8;
  uint8_t pad = 0x80;
  sha1_update(s, &pad, 1);
  uint8_t zero = 0;
  while (s.blockLen != 56) sha1_update(s, &zero, 1);
  uint8_t lenbe[8];
  for (int i = 0; i < 8; i++) lenbe[i] = (uint8_t)(bits >> (56 - i*8));
  sha1_update(s, lenbe, 8);
  for (int i = 0; i < 5; i++) {
    out[i*4]   = (uint8_t)(s.h[i] >> 24);
    out[i*4+1] = (uint8_t)(s.h[i] >> 16);
    out[i*4+2] = (uint8_t)(s.h[i] >> 8);
    out[i*4+3] = (uint8_t)(s.h[i]);
  }
}

// ---- base64 encode --------------------------------------------------------
const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64_encode(const uint8_t *in, size_t n, char *out, size_t out_cap) {
  size_t need = ((n + 2) / 3) * 4;
  if (out_cap < need + 1) return 0;
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t b = (uint32_t)in[i] << 16;
    if (i + 1 < n) b |= (uint32_t)in[i+1] << 8;
    if (i + 2 < n) b |= in[i+2];
    out[o++] = B64[(b >> 18) & 0x3F];
    out[o++] = B64[(b >> 12) & 0x3F];
    out[o++] = (i + 1 < n) ? B64[(b >> 6) & 0x3F] : '=';
    out[o++] = (i + 2 < n) ? B64[b & 0x3F]        : '=';
  }
  out[o] = '\0';
  return o;
}

} // namespace

size_t lite_ws_accept_key(const char *client_key, char *out, size_t out_cap) {
  if (!client_key || out_cap < 29) return 0;
  static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  Sha1 s; sha1_init(s);
  sha1_update(s, (const uint8_t *)client_key, strlen(client_key));
  sha1_update(s, (const uint8_t *)GUID, sizeof(GUID) - 1);
  uint8_t digest[20];
  sha1_final(s, digest);
  return base64_encode(digest, 20, out, out_cap);  // 20 bytes -> 28 chars
}

size_t lite_ws_header_len(size_t len) {
  if (len < 126)     return 2;
  if (len <= 0xFFFF) return 4;
  return 10;
}

size_t lite_ws_encode_text(const uint8_t *payload, size_t len, uint8_t *out, size_t out_cap) {
  size_t hdr = lite_ws_header_len(len);
  if (out_cap < hdr + len) return 0;
  out[0] = 0x81;                       // FIN + opcode 0x1 (text)
  if (len < 126) {
    out[1] = (uint8_t)len;             // bit7 (mask) = 0
  } else if (len <= 0xFFFF) {
    out[1] = 126;
    out[2] = (uint8_t)(len >> 8);
    out[3] = (uint8_t)(len);
  } else {
    out[1] = 127;
    for (int i = 0; i < 8; i++) out[2 + i] = (uint8_t)((uint64_t)len >> (56 - i*8));
  }
  if (len && payload) memcpy(out + hdr, payload, len);
  return hdr + len;
}

size_t lite_ws_encode_header(size_t len, uint8_t out[10]) {
  size_t hdr = lite_ws_header_len(len);
  out[0] = 0x81;                       // FIN + opcode 0x1 (text)
  if (len < 126) {
    out[1] = (uint8_t)len;
  } else if (len <= 0xFFFF) {
    out[1] = 126;
    out[2] = (uint8_t)(len >> 8);
    out[3] = (uint8_t)(len);
  } else {
    out[1] = 127;
    for (int i = 0; i < 8; i++) out[2 + i] = (uint8_t)((uint64_t)len >> (56 - i * 8));
  }
  return hdr;
}

size_t lite_ws_decode_frame(uint8_t *in, size_t len, LiteWsFrame *out) {
  if (!in || !out || len < 2) return 0;
  bool    masked = (in[1] & 0x80) != 0;
  uint64_t plen  = in[1] & 0x7F;
  size_t  pos    = 2;
  if (plen == 126) {
    if (len < 4) return 0;
    plen = ((uint64_t)in[2] << 8) | in[3];
    pos  = 4;
  } else if (plen == 127) {
    if (len < 10) return 0;
    plen = 0;
    for (int i = 0; i < 8; i++) plen = (plen << 8) | in[2 + i];
    pos = 10;
  }
  const uint8_t *mask = nullptr;
  if (masked) {
    if (len < pos + 4) return 0;
    mask = in + pos;
    pos += 4;
  }
  if (len < pos + plen) return 0;       // full payload not here yet
  uint8_t *payload = in + pos;
  if (masked) for (uint64_t i = 0; i < plen; i++) payload[i] ^= mask[i & 3];
  out->opcode      = in[0] & 0x0F;
  out->fin         = (in[0] & 0x80) != 0;
  out->payload     = payload;
  out->payload_len = (size_t)plen;
  return pos + (size_t)plen;
}
