#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#define PNGHEADER 8

long *load_png(const char *fname, int *ret_size)
{
    static FILE *fp = NULL;
    unsigned char header[PNGHEADER];
    int width, height, depth, color_type;
    int i;
    int data_size;
    static uint32_t *data = NULL;
    unsigned char **row_pointers = NULL;
    png_structp png_ptr = NULL;
    png_infop info_ptr = NULL;

    fp = fopen(fname, "rb");
    if (!fp) {
        fprintf(stderr, "Error loading %s: %s\n", fname,
            strerror(errno));
        return 0;
    }
    if (fread(header, 1, PNGHEADER, fp) < PNGHEADER) {
        fprintf(stderr, "File to short (%s)\n", fname);
        goto error;
    }
    if (png_sig_cmp(header, 0, PNGHEADER)) {
        fprintf(stderr, "File is not in PNG format (%s)\n", fname);
        goto error;
    }

    png_ptr =
        png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL,
                   NULL);
    if (!png_ptr) {
        fprintf(stderr,
            "Error while initializing libPNG (read)\n");
        goto error;
    }

    info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        fprintf(stderr,
            "Error while initializing libPNG (info)\n");
        goto error;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        fprintf(stderr, "Error while reading PNG file (%s)\n",
            fname);
        goto error;
    }

    png_init_io(png_ptr, fp);
    png_set_sig_bytes(png_ptr, PNGHEADER);

    png_read_png(png_ptr, info_ptr,
            PNG_TRANSFORM_STRIP_16 | PNG_TRANSFORM_BGR |
            PNG_TRANSFORM_EXPAND | PNG_TRANSFORM_PACKING, NULL);

    width = png_get_image_width(png_ptr, info_ptr);
    height = png_get_image_height(png_ptr, info_ptr);
    depth = png_get_bit_depth(png_ptr, info_ptr);
    color_type = png_get_color_type(png_ptr, info_ptr);

    if (width > 128 || height > 128) {
        fprintf(stderr, "Image too large (%dx%d > 128x128)\n",
            width, height);
        goto error;
    }

    if (depth != 8) {
        fprintf(stderr, "Depth(%d) != 8 (%s)!\n", depth, fname);
        goto error;
    }

    if (color_type != PNG_COLOR_TYPE_RGB_ALPHA) {
        fprintf(stderr,
            "Not supported color type (0x%x != 0x%x) (%s)!\n",
            color_type, PNG_COLOR_TYPE_RGB_ALPHA, fname);
        goto error;
    }

    data_size = 2 + width * height;
    data = malloc(data_size * sizeof(long));
    if (!data) {
        fprintf(stderr, "Cannot allocate memory (%ld bytes)!\n",
            data_size * sizeof(long));
        goto error;
    }

    row_pointers = png_get_rows(png_ptr, info_ptr);
    for (i = 0; i < height; i++) {
        memcpy(&data[i * width + 2], row_pointers[i],
               width * sizeof(uint32_t));
    }

    data[0] = width;
    data[1] = height;
    if (sizeof(long) > sizeof(uint32_t)) {
        /* convert to array of longs */
        i = data_size;
        while (--i >= 0)
            ((unsigned long *) data)[i] = data[i];
    }

    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    fclose(fp);
    if (ret_size)
        *ret_size = data_size;

    return (long *) data;

error:
    if (data)
        free(data);

    if (png_ptr)
        png_destroy_read_struct(&png_ptr, info_ptr ? &info_ptr : NULL, NULL);
    fclose(fp);
    return NULL;

}

// DEBUG
#ifdef MAIN
int main(int argc, char **argv)
{
    if (argc < 2)
        return 1;
    return load_png(argv[1], NULL) != NULL;
}
#endif
