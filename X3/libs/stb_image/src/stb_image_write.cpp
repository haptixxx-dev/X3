// The write half of stb, vendored alongside the read half. Needed by the render
// test harness (tools/render-test) to emit PNGs, and by nothing else yet.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image/stb_image_write.h>
