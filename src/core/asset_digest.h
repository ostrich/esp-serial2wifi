#ifndef ASSET_DIGEST_H
#define ASSET_DIGEST_H
#include <stdint.h>
// Private, streaming SHA-256 state. No SDK crypto ABI dependency.
typedef struct {
  uint32_t h[8], bytes;
  uint8_t block[64];
  unsigned used;
} AssetDigest;
void assetDigestInit(AssetDigest *s);
void assetDigestFeed(AssetDigest *s, const void *data, unsigned length);
void assetDigestFinish(AssetDigest *s, uint8_t out[32]);
#endif
