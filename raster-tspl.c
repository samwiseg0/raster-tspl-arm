/*
 * raster-tspl: CUPS filter for COMER/Kuaimai TSPL label printers (CX418 and family).
 *
 * Clean-room replacement for the vendor's x86-only raster-tspl binary, so the
 * driver runs natively on Apple Silicon and Raspberry Pi. Output matches the
 * vendor filter byte for byte, apart from a few vendor bugs it fixes (see the
 * README). test/compare.sh checks this.
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
#ifdef HAVE_LZO
#include <lzo/lzo1x.h>
#endif

static ppd_file_t *ppd;
static int num_options;
static cups_option_t *options;

/*
 * Returns a setting as the vendor reads it: the job option first, then the
 * PPD's marked choice. Values outside the PPD's choices pass through, so
 * "PrintSpeed=99" sends SPEED 99. Returns NULL when neither is set.
 */
static const char *setting(const char *keyword)
{
    const char *v = cupsGetOption(keyword, num_options, options);
    ppd_choice_t *choice;

    if (v)
        return v;
    if (ppd && (choice = ppdFindMarkedChoice(ppd, keyword)))
        return choice->choice;
    return NULL;
}

/* Like setting(), but "None" (use the printer's own setting) counts as unset. */
static const char *value(const char *keyword)
{
    const char *v = setting(keyword);
    return v && strcmp(v, "None") ? v : NULL;
}

/* The vendor accepts only "true". This also takes "yes", "on" and "1". */
static int is_true(const char *keyword)
{
    const char *v = setting(keyword);
    return v && (!strcasecmp(v, "true") || !strcasecmp(v, "yes") ||
                 !strcasecmp(v, "on") || !strcmp(v, "1"));
}

static void put(const char *s)
{
    fputs(s, stdout);
    fputs("\r\n", stdout);
}

/* A growable byte buffer. A page's bitmap is encoded once, then written per copy. */
typedef struct {
    unsigned char *data;
    size_t len, size;
} buffer_t;

static int buf_add(buffer_t *b, const void *data, size_t len)
{
    unsigned char *p;
    size_t size;

    if (b->len + len > b->size) {
        size = (b->len + len) * 2;
        if (!(p = realloc(b->data, size)))
            return 0;
        b->data = p;
        b->size = size;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    return 1;
}

static int buf_str(buffer_t *b, const char *s)
{
    return buf_add(b, s, strlen(s));
}

static void write_size(const cups_page_header2_t *h)
{
    char buf[64];
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
}

/* Settings the vendor sends once at the start of the job, right after SIZE. */
static void write_setup(const cups_page_header2_t *h)
{
    char buf[64];
    const char *v;

    /* 2 is thermal transfer. The vendor PPD only offers 1. */
    if ((v = value("MediaMethod"))) {
        if (atoi(v) == 1)
            put("SET RIBBON OFF");
        else if (atoi(v) == 2)
            put("SET RIBBON ON");
    }

    /* Anything but gaps (1) or black mark (2) means continuous media. */
    if ((v = value("PaperType"))) {
        if (atoi(v) == 1)
            put("GAP 2 mm, 0 mm");
        else if (atoi(v) == 2)
            put("BLINE 2 mm, 0 mm");
        else
            put("GAP 0 mm, 0 mm");
    }

    /* "FowardOffset" is the vendor PPD's spelling. */
    if ((v = value("FowardOffset"))) {
        snprintf(buf, sizeof(buf), "OFFSET %d mm", atoi(v));
        put(buf);
    }

    /*
     * SHIFT takes dots. The vendor truncates mm * dpi / 25.4 toward zero and
     * clamps to +/-90, so 12mm gives 90 where the formula gives 95.
     */
    if ((v = value("ShiftMove"))) {
        int dots = (int)(atoi(v) * (double)h->HWResolution[1] / 25.4);
        if (dots > 90)
            dots = 90;
        if (dots < -90)
            dots = -90;
        snprintf(buf, sizeof(buf), "SHIFT %d", dots);
        put(buf);
    }

    if ((v = value("PrintSpeed"))) {
        snprintf(buf, sizeof(buf), "SPEED %d", atoi(v));
        put(buf);
    }

    if ((v = value("PrintDarkness"))) {
        snprintf(buf, sizeof(buf), "DENSITY %d", atoi(v));
        put(buf);
    }
}

/* The vendor sends the post-print mode with the first label it prints. */
static void write_post_action(void)
{
    const char *v;

    /*
     * "None" turns every mode off. 1 is tear-off, 2 peel and 3 cutter; the PPD
     * offers only None and 1. Any other value sends nothing.
     */
    if ((v = setting("PostAction")) && !strcmp(v, "None")) {
        put("SET TEAR OFF");
        put("SET PEEL OFF");
        put("SET CUTTER OFF");
    } else if (v && atoi(v) == 1) {
        put("SET TEAR ON");
        put("SET CUTTER OFF");
        put("SET PEEL OFF");
    } else if (v && atoi(v) == 2) {
        put("SET TEAR OFF");
        put("SET CUTTER OFF");
        put("SET PEEL ON");
    } else if (v && atoi(v) == 3) {
        put("SET TEAR OFF");
        put("SET CUTTER 1");
        put("SET PEEL OFF");
    }
}

/*
 * Encodes the page as BITMAP mode 16 bands: LZO1X-1 data behind a 4-byte
 * little-endian length. Each band covers 80mm of feed. The vendor sends CLS
 * before every band and no line break before PRINT.
 */
static int encode_compressed(buffer_t *b, const unsigned char *bitmap, unsigned wbytes,
                             unsigned height, unsigned dpi)
{
#ifdef HAVE_LZO
    static unsigned char *out, *wrk;
    static size_t out_size;
    unsigned band = (unsigned)(80.0 * dpi / 25.4), y, rows;
    size_t in_len, need;
    lzo_uint out_len;
    unsigned char len[4], *p;
    char cmd[64];

    /* A bad resolution in the raster header would give empty bands forever. */
    if (band < 1)
        band = 1;
    in_len = (size_t)wbytes * band;
    if (!wrk && (lzo_init() != LZO_E_OK || !(wrk = malloc(LZO1X_1_MEM_COMPRESS))))
        return 0;
    /* Worst case LZO1X expansion, from the LZO documentation. */
    need = in_len + in_len / 16 + 64 + 3;
    if (out_size < need) {
        if (!(p = realloc(out, need)))
            return 0;
        out = p;
        out_size = need;
    }

    for (y = 0; y < height; y += rows) {
        rows = height - y < band ? height - y : band;
        if (lzo1x_1_compress(bitmap + (size_t)wbytes * y, (lzo_uint)wbytes * rows,
                             out, &out_len, wrk) != LZO_E_OK)
            return 0;
        snprintf(cmd, sizeof(cmd), "%sBITMAP 0,%u,%u,%u,16,", y ? "CLS\r\n" : "", y, wbytes, rows);
        len[0] = out_len & 0xff;
        len[1] = (out_len >> 8) & 0xff;
        len[2] = (out_len >> 16) & 0xff;
        len[3] = (out_len >> 24) & 0xff;
        if (!buf_str(b, cmd) || !buf_add(b, len, 4) || !buf_add(b, out, out_len))
            return 0;
    }
    return 1;
#else
    (void)b; (void)bitmap; (void)wbytes; (void)height; (void)dpi;
    return 0;
#endif
}

/* Encodes the page's BITMAP command. Returns 0 when out of memory. */
static int encode_bitmap(buffer_t *b, const unsigned char *bitmap, unsigned wbytes,
                         unsigned height, unsigned dpi, int compress)
{
    char cmd[64];

    b->len = 0;
    if (compress && encode_compressed(b, bitmap, wbytes, height, dpi))
        return 1;
    if (compress)
        fputs("ERROR: LZO compression failed, sending uncompressed\n", stderr);
    b->len = 0;
    snprintf(cmd, sizeof(cmd), "BITMAP 0,0,%u,%u,1,", wbytes, height);
    return buf_str(b, cmd) && buf_add(b, bitmap, (size_t)wbytes * height) && buf_str(b, "\r\n");
}

/* True when a pixel value of 1 (or 255) means white in this color space. */
static int white_is_high(cups_cspace_t cs)
{
    return cs == CUPS_CSPACE_W || cs == CUPS_CSPACE_SW ||
           cs == CUPS_CSPACE_RGB || cs == CUPS_CSPACE_SRGB;
}

/* Reads one raster line into pix as 1 = black, 0 = white. */
static void unpack_line(const unsigned char *in, unsigned char *pix, unsigned width,
                        unsigned bpp, int high_white, int negative)
{
    unsigned x;
    int black;

    for (x = 0; x < width; x++) {
        if (bpp == 1)
            black = (in[x / 8] >> (7 - x % 8)) & 1;
        else
            black = in[x] >= 128;
        pix[x] = (unsigned char)(black ^ high_white ^ negative);
    }
}

/*
 * Packs pix (w x h, 1 = black) into TSPL bitmap rows: 1 = white, MSB first,
 * padding bits white. Orientation 1 turns the image 90 degrees clockwise, 2 by
 * 180 and 3 counter-clockwise. Mirror flips each output row afterwards.
 * Returns the output width and height.
 */
static void transform(const unsigned char *pix, unsigned w, unsigned h, int orientation,
                      int mirror, unsigned char *out, unsigned *ow, unsigned *oh)
{
    unsigned x, y, sx, sy, xx, wbytes;

    *ow = (orientation == 1 || orientation == 3) ? h : w;
    *oh = (orientation == 1 || orientation == 3) ? w : h;
    wbytes = (*ow + 7) / 8;
    memset(out, 0xff, (size_t)wbytes * *oh);
    for (y = 0; y < *oh; y++) {
        for (x = 0; x < *ow; x++) {
            xx = mirror ? *ow - 1 - x : x;
            switch (orientation) {
            case 1: sy = h - 1 - xx; sx = y; break;
            case 2: sy = h - 1 - y; sx = w - 1 - xx; break;
            case 3: sy = xx; sx = w - 1 - y; break;
            default: sy = y; sx = xx; break;
            }
            if (pix[(size_t)sy * w + sx])
                out[(size_t)y * wbytes + x / 8] &= ~(0x80 >> (x % 8));
        }
    }
}

int main(int argc, char *argv[])
{
    int fd = 0, page = 0, printed = 0, compress, mirror, negative, orientation;
    unsigned y, c, ow, oh, copies;
    buffer_t body = { NULL, 0, 0 };
    cups_raster_t *ras;
    cups_page_header2_t h;
    unsigned char *line = NULL, *pix = NULL, *bitmap = NULL;
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

    num_options = cupsParseOptions(argv[5], 0, &options);
    if ((ppdfile = getenv("PPD")) && (ppd = ppdOpenFile(ppdfile))) {
        ppdMarkDefaults(ppd);
        cupsMarkOptions(ppd, num_options, options);
    } else {
        fputs("DEBUG: no PPD, using printer settings\n", stderr);
    }

    /* The vendor ignores MirrorPrint and NegativePrint in the raster header. */
    mirror = is_true("MirrorImage");
    negative = is_true("NegativeImage");
    orientation = (v = setting("Orientation")) ? atoi(v) : 0;
    /* The vendor fails the job here. Printing unrotated is more useful. */
    if (orientation < 0 || orientation > 3) {
        fprintf(stderr, "WARNING: unsupported Orientation %d, printing unrotated\n", orientation);
        orientation = 0;
    }
    compress = is_true("DataCompress");
#ifndef HAVE_LZO
    if (compress) {
        fputs("DEBUG: built without LZO, sending uncompressed bitmaps\n", stderr);
        compress = 0;
    }
#endif

    ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    while (cupsRasterReadHeader2(ras, &h)) {
        if (h.cupsBitsPerPixel != 1 && h.cupsBitsPerPixel != 8) {
            fprintf(stderr, "ERROR: unsupported raster: %u bits per pixel\n",
                    h.cupsBitsPerPixel);
            return 1;
        }

        /*
         * The largest PPD size is 306 x 6480 points, about 1300 x 27000 dots at
         * 300 dpi. These limits keep the buffer sizes below from overflowing on
         * 32-bit systems.
         */
        if (h.cupsWidth == 0 || h.cupsWidth > 8192 || h.cupsHeight == 0 || h.cupsHeight > 65536 ||
            h.cupsBytesPerLine < (h.cupsWidth * h.cupsBitsPerPixel + 7) / 8) {
            fprintf(stderr, "ERROR: unsupported raster size %ux%u\n", h.cupsWidth, h.cupsHeight);
            return 1;
        }

        page++;
        /* The vendor printed nothing for NumCopies 0. CUPS treats 0 as 1. */
        copies = h.NumCopies ? h.NumCopies : 1;
        fprintf(stderr, "PAGE: %d %u\n", page, copies);

        line = realloc(line, h.cupsBytesPerLine);
        pix = realloc(pix, (size_t)h.cupsWidth * h.cupsHeight);
        /* Big enough for either orientation, with padding bits. */
        bitmap = realloc(bitmap, (size_t)(h.cupsWidth + 8) * (h.cupsHeight + 8) / 8 + 8);
        if (!line || !pix || !bitmap) {
            fputs("ERROR: out of memory\n", stderr);
            return 1;
        }

        for (y = 0; y < h.cupsHeight; y++) {
            if (cupsRasterReadPixels(ras, line, h.cupsBytesPerLine) < h.cupsBytesPerLine) {
                /* Rows missing from a truncated job print white. */
                fprintf(stderr, "ERROR: raster ended at line %u of %u\n", y, h.cupsHeight);
                memset(pix + (size_t)h.cupsWidth * y, 0, (size_t)h.cupsWidth * (h.cupsHeight - y));
                break;
            }
            unpack_line(line, pix + (size_t)h.cupsWidth * y, h.cupsWidth, h.cupsBitsPerPixel,
                        white_is_high(h.cupsColorSpace), negative);
        }

        if (page == 1) {
            /* The vendor leads each job with 512 NULs, then a blank line. */
            fwrite(pad, 1, sizeof(pad), stdout);
            fputs("\r\n", stdout);
            write_size(&h);
            write_setup(&h);
        }
        transform(pix, h.cupsWidth, h.cupsHeight, orientation, mirror, bitmap, &ow, &oh);
        if (!encode_bitmap(&body, bitmap, (ow + 7) / 8, oh, h.HWResolution[1], compress)) {
            fputs("ERROR: out of memory\n", stderr);
            return 1;
        }

        /* The vendor prints each page once per copy in the raster header. */
        for (c = 0; c < copies; c++) {
            if (!printed++)
                write_post_action();
            put("CLS");
            fwrite(body.data, 1, body.len, stdout);
            put("PRINT 1,1");
        }
        fflush(stdout);
    }

    cupsRasterClose(ras);
    if (fd)
        close(fd);
    if (ppd)
        ppdClose(ppd);
    cupsFreeOptions(num_options, options);
    free(line);
    free(pix);
    free(bitmap);
    free(body.data);

    if (!page) {
        fputs("ERROR: no pages found\n", stderr);
        return 1;
    }
    return 0;
}
