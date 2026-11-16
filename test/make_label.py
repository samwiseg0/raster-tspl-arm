"""Writes a 4x6 test label PDF: text, border, bars and a 64-step gray ramp."""
import sys

bars = "".join(f"{40 + i * 8} 30 4 120 re f\n" for i in range(31))
ramp = "".join(f"{i / 63:.4f} g {12 + i * 4} 200 4 60 re f\n" for i in range(64))
content = f"""BT /F1 36 Tf 20 360 Td (SHIP TO:) Tj ET
BT /F2 22 Tf 20 320 Td (Test Label 123) Tj ET
4 w 10 10 263 405 re S
{bars}{ramp}""".encode()

objs = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 283 425] /Contents 4 0 R"
    b" /Resources << /Font << /F1 5 0 R /F2 6 0 R >> >> >>",
    b"<< /Length %d >>\nstream\n" % len(content) + content + b"\nendstream",
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>",
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
]
out = bytearray(b"%PDF-1.4\n")
offsets = []
for i, o in enumerate(objs, 1):
    offsets.append(len(out))
    out += b"%d 0 obj\n" % i + o + b"\nendobj\n"
xref = len(out)
out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
out += b"".join(b"%010d 00000 n \n" % o for o in offsets)
out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, xref)
open(sys.argv[1], "wb").write(out)
