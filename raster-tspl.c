/*
 * raster-tspl: CUPS filter for COMER/Kuaimai TSPL label printers (CX418 and family).
 *
 * Clean-room replacement for the vendor's x86-only raster-tspl binary, so the
 * driver runs natively on Apple Silicon and Raspberry Pi. Output matches the
 * vendor filter byte for byte (test/compare.sh checks this).
 *
 * Usage (as a CUPS filter): raster-tspl job-id user title copies options [file]
 */

#include <cups/cups.h>
#include <cups/ppd.h>
#include <cups/raster.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Returns the marked PPD choice for an option, or NULL when unset or "None". */
static const char *ppd_choice(ppd_file_t *ppd, const char *keyword)
{
    ppd_choice_t *choice;

    if (!ppd || !(choice = ppdFindMarkedChoice(ppd, keyword)))
        return NULL;
    if (!strcmp(choice->choice, "None"))
        return NULL;
    return choice->choice;
}

static int ppd_true(ppd_file_t *ppd, const char *keyword)
{
    const char *v = ppd_choice(ppd, keyword);
    return v && (!strcasecmp(v, "True") || !strcasecmp(v, "Yes"));
}

static void put(const char *s)
{
    fputs(s, stdout);
    fputs("\r\n", stdout);
}

static void write_setup(ppd_file_t *ppd, const cups_page_header2_t *h)
{
    char buf[128];
    const char *v;
    ppd_size_t *size;
    double width = h->PageSize[0], length = h->PageSize[1];

    /* The vendor takes SIZE from the PPD's marked page size when there is one. */
    if (ppd && (size = ppdPageSize(ppd, NULL))) {
        width = size->width;
        length = size->length;
    }
    snprintf(buf, sizeof(buf), "SIZE %.1f mm, %.1f mm",
             width * 25.4 / 72.0, length * 25.4 / 72.0);
    put(buf);

    if (ppd_choice(ppd, "MediaMethod"))
        put("SET RIBBON OFF");

    if ((v = ppd_choice(ppd, "PaperType"))) {
        if (!strcmp(v, "1"))
            put("GAP 2 mm, 0 mm");
        else if (!strcmp(v, "2"))
            put("BLINE 2 mm, 0 mm");
        else if (!strcmp(v, "3"))
            put("GAP 0 mm, 0 mm");
    }

    /* "FowardOffset" is the vendor PPD's spelling. */
    if ((v = ppd_choice(ppd, "FowardOffset"))) {
        snprintf(buf, sizeof(buf), "OFFSET %d mm", atoi(v));
        put(buf);
    }

    /*
     * SHIFT takes dots. The vendor truncates mm * dpi / 25.4 toward zero and
     * clamps to +/-90, so 12mm gives 90 where the formula gives 95.
     */
    if ((v = ppd_choice(ppd, "ShiftMove"))) {
        int dots = (int)(atoi(v) * (double)h->HWResolution[1] / 25.4);
        if (dots > 90)
            dots = 90;
        if (dots < -90)
            dots = -90;
        snprintf(buf, sizeof(buf), "SHIFT %d", dots);
        put(buf);
    }

    if ((v = ppd_choice(ppd, "PrintSpeed"))) {
        snprintf(buf, sizeof(buf), "SPEED %d", atoi(v));
        put(buf);
    }

    if ((v = ppd_choice(ppd, "PrintDarkness"))) {
        snprintf(buf, sizeof(buf), "DENSITY %d", atoi(v));
        put(buf);
    }

    if ((v = ppd_choice(ppd, "PostAction")) && !strcmp(v, "1")) {
        put("SET TEAR ON");
        put("SET CUTTER OFF");
        put("SET PEEL OFF");
    } else {
        put("SET TEAR OFF");
        put("SET PEEL OFF");
        put("SET CUTTER OFF");
    }
}

/* True when a pixel value of 1 (or 255) means white in this color space. */
static int white_is_high(cups_cspace_t cs)
{
    return cs == CUPS_CSPACE_W || cs == CUPS_CSPACE_SW ||
           cs == CUPS_CSPACE_RGB || cs == CUPS_CSPACE_SRGB;
}

/*
 * Packs one raster line into TSPL bitmap bits (1 = white, MSB first).
 * Bits past the image width stay white.
 */
static void pack_line(const unsigned char *in, unsigned char *out, unsigned width,
                      unsigned bpp, int high_white, int mirror, int negative)
{
    unsigned x, src;
    int black;

    memset(out, 0xff, (width + 7) / 8);
    for (x = 0; x < width; x++) {
        src = mirror ? width - 1 - x : x;
        if (bpp == 1)
            black = (in[src / 8] >> (7 - src % 8)) & 1;
        else
            black = in[src] >= 128;
        if (high_white)
            black = !black;
        if (negative)
            black = !black;
        if (black)
            out[x / 8] &= ~(0x80 >> (x % 8));
    }
}

int main(int argc, char *argv[])
{
    int fd = 0, page = 0, mirror, negative, rotate;
    cups_raster_t *ras;
    cups_page_header2_t h;
    ppd_file_t *ppd = NULL;
    cups_option_t *options = NULL;
    int num_options;
    unsigned char *line = NULL, *bitmap = NULL;
    unsigned wbytes, y;
    const char *ppdfile, *v;
    static const char pad[512];

    if (argc < 6 || argc > 7) {
        fputs("ERROR: raster-tspl job-id user title copies options [file]\n", stderr);
        return 1;
    }
    if (argc == 7 && (fd = open(argv[6], O_RDONLY)) < 0) {
        perror("ERROR: unable to open raster file");
        return 1;
    }

    if ((ppdfile = getenv("PPD")) && (ppd = ppdOpenFile(ppdfile))) {
        ppdMarkDefaults(ppd);
        num_options = cupsParseOptions(argv[5], 0, &options);
        cupsMarkOptions(ppd, num_options, options);
        cupsFreeOptions(num_options, options);
    } else {
        fputs("DEBUG: no PPD, using printer settings\n", stderr);
    }

    ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    while (cupsRasterReadHeader2(ras, &h)) {
        if (h.cupsBitsPerPixel != 1 && h.cupsBitsPerPixel != 8) {
            fprintf(stderr, "ERROR: unsupported raster: %u bits per pixel\n",
                    h.cupsBitsPerPixel);
            return 1;
        }

        page++;
        fprintf(stderr, "PAGE: %d 1\n", page);

        wbytes = (h.cupsWidth + 7) / 8;
        line = realloc(line, h.cupsBytesPerLine);
        bitmap = realloc(bitmap, (size_t)wbytes * h.cupsHeight);
        if (!line || !bitmap) {
            fputs("ERROR: out of memory\n", stderr);
            return 1;
        }

        mirror = h.MirrorPrint || ppd_true(ppd, "MirrorImage");
        negative = h.NegativePrint || ppd_true(ppd, "NegativeImage");
        rotate = (v = ppd_choice(ppd, "Orientation")) && !strcmp(v, "2");

        /* 180 degree rotation is a vertical flip plus a mirror. */
        for (y = 0; y < h.cupsHeight; y++) {
            if (cupsRasterReadPixels(ras, line, h.cupsBytesPerLine) < h.cupsBytesPerLine)
                break;
            pack_line(line,
                      bitmap + (size_t)wbytes * (rotate ? h.cupsHeight - 1 - y : y),
                      h.cupsWidth, h.cupsBitsPerPixel, white_is_high(h.cupsColorSpace),
                      mirror ^ rotate, negative);
        }

        if (page == 1) {
            /* The vendor filter leads each job with 512 NULs, then a blank line. */
            fwrite(pad, 1, sizeof(pad), stdout);
            fputs("\r\n", stdout);
            write_setup(ppd, &h);
        }
        put("CLS");
        printf("BITMAP 0,0,%u,%u,1,", wbytes, h.cupsHeight);
        fwrite(bitmap, wbytes, h.cupsHeight, stdout);
        fputs("\r\n", stdout);
        put("PRINT 1,1");
        fflush(stdout);
    }

    cupsRasterClose(ras);
    if (fd)
        close(fd);
    if (ppd)
        ppdClose(ppd);
    free(line);
    free(bitmap);

    if (!page) {
        fputs("ERROR: no pages found\n", stderr);
        return 1;
    }
    return 0;
}
