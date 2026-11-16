# raster-tspl-arm

A CUPS filter for COMER/Kuaimai TSPL label printers (CX418, CX418HD, CX418BT, CI250, CE410).
It replaces the vendor's `raster-tspl`, which ships only as x86 binaries.
This one builds natively on Apple Silicon and Raspberry Pi.

Its output matches the vendor filter byte for byte. `test/compare.sh` checks this.

The vendor filter is built on an HPRT library (it reports itself as `rastertohprtlblt`). HPRT-based TSPL printers may work with it too, but only the CX418 has been tested.

## Build

Needs the CUPS headers. They come with the Xcode Command Line Tools on macOS and in `libcups2-dev` on Debian.

```sh
make
```

## Install

Install the vendor driver package first. It provides the PPDs. Then replace its filter:

```sh
# macOS
sudo mv /usr/libexec/cups/filter/raster-tspl /usr/libexec/cups/filter/raster-tspl.x86_64-vendor
sudo install -o root -g wheel -m 755 raster-tspl /usr/libexec/cups/filter/raster-tspl

# Debian / Raspberry Pi OS
sudo mv /usr/lib/cups/filter/raster-tspl /usr/lib/cups/filter/raster-tspl.x86-vendor
sudo install -o root -g root -m 755 raster-tspl /usr/lib/cups/filter/raster-tspl
```

## Test

Run on a Mac with the vendor driver installed. The vendor filter runs under Rosetta.

```sh
make && test/compare.sh
```

It uses `raster-tspl.x86_64-vendor` when the native filter is installed. Pass another vendor filter path as the first argument.

## Output format

- 512 NUL bytes, then `\r\n`, once per job.
- Setup once per job: `SIZE`, then the optional `SET RIBBON OFF`, `GAP`/`BLINE`, `OFFSET`, `SHIFT`, `SPEED`, `DENSITY`, then the tear-off settings.
- Per page: `CLS`, `BITMAP 0,0,<bytes-per-row>,<rows>,1,<data>`, `PRINT 1,1`.
- Bitmap bits are MSB first, and 1 means white. Pixels at 50% grey or darker print black.

## License

MIT. See `LICENSE`.
