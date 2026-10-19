/* stb_image for album art. Covers come from i.scdn.co as JPEG; limiting the
 * decoder to JPEG keeps every other format parser away from network data. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG   /* a local profile.png only, never network data */
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 4096
#include "stb_image.h"
