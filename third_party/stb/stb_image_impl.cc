// Donner decodes only PNG, JPEG and GIF: ImageLoader rejects any input without one of their
// signatures before stb_image runs. Keep in sync with the `image` target in third_party/BUILD.stb.
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
