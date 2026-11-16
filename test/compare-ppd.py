"""Compares our compiled PPDs with the vendor PPDs, model by model.

Usage: python3 -I test/compare-ppd.py <our-ppd-dir> <vendor-ppd-dir>

Checks every UI option (type, group, default, order, choices with labels and
code) and the attributes raster-tspl or CUPS read against the vendor PPDs.

Sizes differ from the vendor on purpose. Ours use true inch dimensions, keep
every vendor size and add the Windows driver's sizes. For sizes this checks
that PageSize, PageRegion, ImageableArea and PaperDimension agree and match
the inch label. Exits 1 on any difference.
"""
import gzip
import pathlib
import re
import sys

# Labels rounded to 2 places that stand for exact fractions of an inch.
EXACT = {"2.38": 2.375, "7.38": 7.375, "5.83": 35 / 6}
ADDED = {"w100h63", "w38h50", "w38h25", "w38h12", "w31h57"}
SIZE_OPTIONS = ("PageSize", "PageRegion")

ATTRS = ["cupsFilter", "ColorDevice", "DefaultColorSpace", "MaxMediaWidth", "MaxMediaHeight",
         "HWMargins", "CustomPageSize", "LandscapeOrientation", "cupsModelNumber",
         "cupsManualCopies", "LanguageLevel", "DefaultResolution"]


def read(path):
    data = path.read_bytes()
    if path.suffix == ".gz":
        data = gzip.decompress(data)
    return data.decode("latin-1")


def numeric(value):
    """Rounds every number in a string to 2 places, so 306.140014648438 == 306.14."""
    return value and re.sub(r"-?\d+\.\d+|-?\d+", lambda m: f"{float(m.group()):.2f}", value)


def norm(code):
    return re.sub(r"\s+", " ", code).strip()


def parse(text):
    ppd = {"options": {}, "sizes": {}, "attrs": {}, "custom": {}}
    groups = {}
    group = None
    for line in text.splitlines():
        g = re.match(r"^\*OpenGroup:\s*(\S+)", line)
        if g:
            group = g.group(1)
        elif line.startswith("*CloseGroup"):
            group = None
        u = re.match(r"^\*OpenUI \*(\w+)", line)
        if u:
            groups[u.group(1)] = group
    for m in re.finditer(r'^\*OpenUI \*(\w+)(?:/([^:]*))?:\s*(\w+)\s*$(.*?)^\*CloseUI', text, re.M | re.S):
        kw, label, kind, body = m.groups()
        default = re.search(r"^\*Default%s:\s*(\S+)" % kw, body, re.M)
        order = re.search(r"^\*OrderDependency:\s*(\S+)\s+(\S+)", body, re.M)
        choices = [(c, (t or "").strip(), norm(code))
                   for c, t, code in re.findall(r'^\*%s ([^/:\s]+)(?:/([^:]*))?:\s*"([^"]*)"' % kw, body, re.M)]
        ppd["options"][kw] = {"label": label, "type": kind, "group": groups.get(kw),
                              "default": default and default.group(1),
                              "order": order and order.groups(), "choices": choices}
    for kw in ("ImageableArea", "PaperDimension"):
        for name, val in re.findall(r'^\*%s ([^/:\s]+)[^:]*:\s*"([^"]*)"' % kw, text, re.M):
            ppd["sizes"].setdefault(name, {})[kw] = " ".join(val.split())
    for kw in ATTRS:
        m = re.search(r'^\*%s(?: [^:]*)?:\s*(.*)$' % kw, text, re.M)
        ppd["attrs"][kw] = numeric(m and m.group(1).strip().strip('"'))
    for name, val in re.findall(r'^\*ParamCustomPageSize (\w+):\s*(.*)$', text, re.M):
        ppd["custom"][name] = numeric(" ".join(val.split()))
    model = re.search(r'^\*ModelName:\s*"([^"]*)"', text, re.M)
    ppd["model"] = model and re.sub(r"^COMER ", "", model.group(1))
    return ppd


def size_of(code):
    m = re.search(r"/PageSize(\[[^\]]*\])", code)
    return m and m.group(1)


def check_sizes(ours, vendor):
    """Our sizes: all vendor names kept, ADDED present, entries agree, inch labels match."""
    problems = []
    choices = {c: (t, size_of(code)) for c, t, code in ours["options"]["PageSize"]["choices"]}
    region = {c: size_of(code) for c, _, code in ours["options"]["PageRegion"]["choices"]}
    vendor_names = {c for c, _, _ in vendor["options"]["PageSize"]["choices"]}
    missing = vendor_names - set(choices)
    if missing:
        problems.append(f"vendor sizes missing: {sorted(missing)}")
    extra = set(choices) - vendor_names
    if extra != ADDED:
        problems.append(f"added sizes {sorted(extra)}, expected {sorted(ADDED)}")
    for name, (label, ps) in choices.items():
        m = re.match(r'([\d.]+)"x([\d.]+)"', label)
        want = [round(EXACT.get(v, float(v)) * 72) for v in m.groups()]
        dims = ours["sizes"].get(name, {})
        got = {
            "PageSize": ps.strip("[]").split(),
            "PageRegion": region.get(name, "").strip("[]").split(),
            "PaperDimension": dims.get("PaperDimension", "").split(),
            "ImageableArea": dims.get("ImageableArea", "0 0 0 0").split()[2:],
        }
        for kw, val in got.items():
            if [float(v) for v in val] != [float(w) for w in want]:
                problems.append(f"{name} {kw} {val} != {want} from label {label!r}")
    return problems


def compare(ours, vendor):
    diffs = []

    def report(what, a, b):
        diffs.append(f"  DIFF {what}: vendor={b!r} ours={a!r}")

    for kw in sorted(set(ours["options"]) | set(vendor["options"])):
        o, v = ours["options"].get(kw), vendor["options"].get(kw)
        if not o or not v:
            report(f"option {kw}", bool(o), bool(v))
            continue
        for field in ("label", "type", "group", "default", "order"):
            if o[field] != v[field]:
                report(f"{kw} {field}", o[field], v[field])
        if kw not in SIZE_OPTIONS and o["choices"] != v["choices"]:
            report(f"{kw} choices", o["choices"], v["choices"])
    for kw in ATTRS:
        if ours["attrs"][kw] != vendor["attrs"][kw]:
            report(f"*{kw}", ours["attrs"][kw], vendor["attrs"][kw])
    if ours["custom"] != vendor["custom"]:
        report("ParamCustomPageSize", ours["custom"], vendor["custom"])
    diffs += [f"  SIZE {p}" for p in check_sizes(ours, vendor)]
    return diffs, len(diffs)


def main():
    ours = {}
    for path in sorted(pathlib.Path(sys.argv[1]).glob("*.ppd*")):
        ppd = parse(read(path))
        ours[ppd["model"]] = ppd
    total = 0
    # Only the vendor's TSPL PPDs. The system PPD folder holds every other driver's too.
    vendor_files = sorted(pathlib.Path(sys.argv[2]).glob("*.TSPL.ppd*"))
    if not vendor_files:
        sys.exit("no vendor PPDs found")
    for path in vendor_files:
        vendor = parse(read(path))
        model = vendor["model"]
        if model not in ours:
            print(f"{model}: MISSING from our PPDs")
            total += 1
            continue
        diffs, unexpected = compare(ours[model], vendor)
        print(f"{model}: {len(vendor['options'])} options, {len(ours[model]['sizes'])} sizes, "
              f"{unexpected} differences")
        for d in diffs:
            print(d)
        total += unexpected
    print("PASS" if total == 0 else f"FAIL: {total} unexpected differences")
    sys.exit(1 if total else 0)


if __name__ == "__main__":
    main()
