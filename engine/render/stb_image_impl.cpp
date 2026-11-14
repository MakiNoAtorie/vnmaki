// The stb_image implementation, built with only the decoders the engine uses (docs/SPEC.md
// section 4: PNG and JPEG). Third-party code: built without the project's warnings and lint
// (engine/render/CMakeLists.txt).

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
// Images come from memory (through the VFS), never from stdio files.
#define STBI_NO_STDIO
// Errors are reported as ImageError by image.cpp; skip stb's message strings.
#define STBI_NO_FAILURE_STRINGS
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
