#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"
#include <string.h>
/* Referenz fuer tools/fmttest.py: dieselbe stb-Version nativ auf dem Mac.
 * stbref dec in.jpg out.png | stbref rt in.png out.png (JPEG q90 wie AmiPixelShop und zurueck) */
int main(int c, char **v) {
    int w, h, n;
    unsigned char *p = stbi_load(v[2], &w, &h, &n, 3);
    if (!p) return 1;
    if (!strcmp(v[1], "rt")) {
        stbi_write_jpg("stbref_tmp.jpg", w, h, 3, p, 90);
        stbi_image_free(p);
        p = stbi_load("stbref_tmp.jpg", &w, &h, &n, 3);
    }
    stbi_write_png(v[3], w, h, 3, p, w * 3);
    return 0;
}
