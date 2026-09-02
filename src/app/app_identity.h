// The app's Marketplace identity, baked at build time (plan Â§3.6: bake it in
// so a stale local config cannot change the published app's identity).
//
// This is the PUBLIC client id of a PKCE app -- a public identifier by
// design, safe to embed in a shipped binary; there is no secret anywhere in
// this product. Currently the CoreVideo app's id (owner-authorized interim,
// plan Â§3.6); replaced with ZComms' own id when its Marketplace review
// lands, by changing exactly this constant or defining ZCOMMS_APP_KEY at
// configure time.
#pragma once

namespace zc {

#ifndef ZCOMMS_APP_KEY
#define ZCOMMS_APP_KEY "y6sIWSwiTZe1JygMx4C9EQ"
#endif

inline const char* kDefaultPublicAppKey = ZCOMMS_APP_KEY;
inline const char* kAppVersion = "0.1.13";

}  // namespace zc
