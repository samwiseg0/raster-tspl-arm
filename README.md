# raster-tspl-arm

A CUPS driver for COMER/Kuaimai TSPL label printers (CX418, CX418BT, CX418HD, CE410, CI250, CI250E).
It replaces the vendor's `raster-tspl` filter, which ships only as x86 binaries, and its PPDs.
It builds natively on Apple Silicon and Raspberry Pi. The vendor package is not needed.

The filter's output matches the vendor filter byte for byte, apart from the vendor bugs listed below. The PPDs match the vendor PPDs option for option, with corrected and added label sizes. `test/compare.sh` and `test/compare-ppd.py` check this.

The vendor filter is built on an HPRT library (it reports itself as `rastertohprtlblt`). HPRT-based TSPL printers may work with it too, but only the CX418 has been tested.

## Build

Needs the CUPS headers and `ppdc`. They come with the Xcode Command Line Tools on macOS. On Debian, install `libcups2-dev` and `cups-ppdc`.

LZO is optional. It enables compressed bitmaps (`DataCompress`), which the CX418HD PPD turns on by default. Install it with `brew install lzo` or `apt install liblzo2-dev`. Without it, the filter sends uncompressed bitmaps. On macOS the filter links LZO statically.

```sh
make            # builds raster-tspl and build/*.ppd
make LZO=0      # builds without LZO
```

## Install

If the vendor driver is installed, first move its filter aside. The install below overwrites it, and the test needs it:

```sh
sudo mv /usr/libexec/cups/filter/raster-tspl /usr/libexec/cups/filter/raster-tspl.x86_64-vendor
```

Then install:

```sh
# macOS
sudo install -o root -g wheel -m 755 raster-tspl /usr/libexec/cups/filter/raster-tspl
sudo install -o root -g wheel -m 644 build/*.ppd /Library/Printers/PPDs/Contents/Resources/

# Debian / Raspberry Pi OS
sudo install -o root -g root -m 755 raster-tspl /usr/lib/cups/filter/raster-tspl
sudo install -d /usr/share/ppd/comer
sudo install -o root -g root -m 644 build/*.ppd /usr/share/ppd/comer/
```

Then add the printer, for example:

```sh
lpadmin -p CX418 -E -v usb://... -P build/cx418.ppd
```

## Test

Run on a Mac with the vendor driver package installed. The vendor filter runs under Rosetta.

```sh
make && test/compare.sh && python3 -I test/compare-ppd.py build /Library/Printers/PPDs/Contents/Resources
```

`compare.sh` runs both filters with the vendor PPDs for every model and compares bytes and exit status. It covers every option, values outside the PPD choices, rotation, mirror, negative, compression, multi-page jobs and copy counts. For each vendor bug we fix, it checks our output against the vendor's output for the corrected input. It also rasterizes every size of our PPDs and checks `SIZE` and the bitmap width. It uses `raster-tspl.x86_64-vendor` when the native filter is installed. Pass another vendor filter path as the first argument.

## Output format

- 512 NUL bytes, then `\r\n`, once per job.
- Once per job: `SIZE`, then the optional `SET RIBBON`, `GAP`/`BLINE`, `OFFSET`, `SHIFT`, `SPEED` and `DENSITY`.
- With the first label: the post-print mode (`SET TEAR`, `SET PEEL`, `SET CUTTER`).
- Per label: `CLS`, the bitmap, `PRINT 1,1`. Each page prints once per copy in the raster header.
- Uncompressed bitmap: `BITMAP 0,0,<bytes-per-row>,<rows>,1,<data>\r\n`. Bits are MSB first and 1 means white. Pixels at 50% grey or darker print black.
- Compressed bitmap: one `BITMAP 0,<y>,<bytes-per-row>,<rows>,16,` band per 80 mm of feed, each followed by a 4-byte little-endian length and LZO1X-1 data. Bands after the first are preceded by `CLS`.
- Settings come from the job options first, then the PPD. Values outside the PPD's choices pass through.

## Differences from the vendor driver

Fixed vendor bugs:

- Label sizes use true inch dimensions, as the vendor's Windows driver does. The vendor's Mac PPDs used 100 mm for 4" labels, and the 2.75"x1.25", 2.5"x2" and 2.5"x1 sizes had a PageSize wider than the label.
- The PPDs add the Windows driver's 4"x2.5", 1.5"x2", 1.5"x1", 1.5"x0.5" and 1.25"x2.25" sizes.
- The PPDs name the manufacturer, so printers show as "COMER CX418".
- A raster page with NumCopies 0 prints once. The vendor printed nothing and then fed a blank label.
- Orientation values outside 0-3 print unrotated with a warning. The vendor failed the job after the setup commands.
- MirrorImage, NegativeImage and DataCompress accept `yes`, `on` and `1` as well as `true`.

Vendor behaviour kept, because it is untested on hardware:

- Compressed output sends `CLS` between bands. The CX418 cannot test this: tested on 2026-11-16, it skips mode 16 bitmaps and prints uncompressed ones. Its PPD does not offer DataCompress. Only the CX418HD PPD turns it on.
- Orientation 1 and 3 (90 and 270 degrees, not offered in the PPD) rotate the bitmap but keep the unrotated `SIZE`.

## License

MIT. See `LICENSE`. LZO is GPL-2.0. A filter binary built with LZO is covered by the GPL.
