#!/usr/bin/env python3
"""Collect results/*.csv into a single .xlsx workbook.

Deliberately dependency-free: an .xlsx is a zip of XML parts, and `zipfile`
plus a few format strings is all it takes.  openpyxl would be the obvious
choice, but this script has to run on a lab machine and on a grader's machine
without anyone being asked to pip install anything first.

  python3 scripts/make_excel.py [--results results] [--out TEAM_ASSIGN1.xlsx]

Produces one sheet per experiment (the five raw runs, their average, and the
speedup) plus a Summary sheet holding every speedup curve side by side.
"""
import argparse
import csv
import glob
import os
import re
import zipfile

NS = "http://schemas.openxmlformats.org/spreadsheetml/2006/main"
RNS = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
PNS = "http://schemas.openxmlformats.org/package/2006/relationships"

NUMERIC = re.compile(r"^-?\d+(\.\d+)?([eE][-+]?\d+)?$")


def col_name(i):
    """0 -> A, 25 -> Z, 26 -> AA."""
    s = ""
    i += 1
    while i:
        i, r = divmod(i - 1, 26)
        s = chr(ord("A") + r) + s
    return s


def esc(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def sheet_xml(rows):
    out = [f'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
           f'<worksheet xmlns="{NS}"><sheetData>']
    for r, row in enumerate(rows, start=1):
        out.append(f'<row r="{r}">')
        for c, val in enumerate(row):
            ref = f"{col_name(c)}{r}"
            if val is None or val == "":
                continue
            if NUMERIC.match(str(val)):
                out.append(f'<c r="{ref}"><v>{val}</v></c>')
            else:
                out.append(f'<c r="{ref}" t="inlineStr"><is><t>{esc(val)}</t></is></c>')
        out.append("</row>")
    out.append("</sheetData></worksheet>")
    return "".join(out)


def safe_sheet_name(name, taken):
    """Excel: <=31 chars, none of []:*?/\\ , and unique."""
    n = re.sub(r"[\[\]:*?/\\]", "_", name)[:31]
    base, i = n, 1
    while n.lower() in taken:
        suffix = f"~{i}"
        n = base[: 31 - len(suffix)] + suffix
        i += 1
    taken.add(n.lower())
    return n


def write_xlsx(path, sheets):
    """sheets: list of (name, rows)."""
    ct = [f'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
          f'<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
          f'<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
          f'<Default Extension="xml" ContentType="application/xml"/>'
          f'<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>']
    wb = [f'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
          f'<workbook xmlns="{NS}" xmlns:r="{RNS}"><sheets>']
    rels = [f'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            f'<Relationships xmlns="{PNS}">']

    for i, (name, _) in enumerate(sheets, start=1):
        ct.append(f'<Override PartName="/xl/worksheets/sheet{i}.xml" '
                  f'ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>')
        wb.append(f'<sheet name="{esc(name)}" sheetId="{i}" r:id="rId{i}"/>')
        rels.append(f'<Relationship Id="rId{i}" '
                    f'Type="{RNS}/worksheet" Target="worksheets/sheet{i}.xml"/>')
    ct.append("</Types>")
    wb.append("</sheets></workbook>")
    rels.append("</Relationships>")

    root_rels = (f'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                 f'<Relationships xmlns="{PNS}"><Relationship Id="rId1" '
                 f'Type="{RNS}/officeDocument" Target="xl/workbook.xml"/></Relationships>')

    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", "".join(ct))
        z.writestr("_rels/.rels", root_rels)
        z.writestr("xl/workbook.xml", "".join(wb))
        z.writestr("xl/_rels/workbook.xml.rels", "".join(rels))
        for i, (_, rows) in enumerate(sheets, start=1):
            z.writestr(f"xl/worksheets/sheet{i}.xml", sheet_xml(rows))


def read_meta(path):
    meta = {}
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                if "=" in line:
                    k, v = line.rstrip("\n").split("=", 1)
                    meta[k] = v
    return meta


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default="results")
    ap.add_argument("--out", default="TEAMNUM_ASSIGN1.xlsx")
    args = ap.parse_args()

    paths = sorted(glob.glob(os.path.join(args.results, "*.csv")))
    if not paths:
        raise SystemExit(f"no CSV files in {args.results}/ -- run scripts/run_experiments.sh first")

    sheets, taken = [], set()
    curves = {}          # experiment -> {threads: speedup}
    times = {}           # experiment -> {threads: avg}
    all_threads = set()

    for p in paths:
        name = os.path.splitext(os.path.basename(p))[0]
        meta = read_meta(os.path.join(args.results, name + ".meta"))
        with open(p) as f:
            rows = [r for r in csv.reader(f) if r]
        header, body = rows[0], rows[1:]

        sheet = [[f"Experiment: {name}"],
                 [f"Command: {meta.get('program','')} {meta.get('args','')}"],
                 [f"Host: {meta.get('host','')}", f"Run (UTC): {meta.get('date','')}"],
                 [f"Sequential binary (no OpenMP): {meta.get('seq_time','')} s",
                  f"Result checksum: {meta.get('seq_sum','')}"],
                 ["All runtimes in seconds. Speedup = avg(1 thread) / avg(t threads)."],
                 []]
        sheet.append(header)
        sheet.extend(body)
        sheets.append((safe_sheet_name(name, taken), sheet))

        curves[name], times[name] = {}, {}
        for r in body:
            t = int(r[0])
            all_threads.add(t)
            curves[name][t] = r[-1]
            times[name][t] = r[-2]

    names = sorted(curves)
    threads = sorted(all_threads)

    summary = [["Summary -- speedup vs thread count"], []]
    summary.append(["threads"] + names)
    for t in threads:
        summary.append([t] + [curves[n].get(t, "") for n in names])
    summary += [[], ["Summary -- average runtime (seconds)"], []]
    summary.append(["threads"] + names)
    for t in threads:
        summary.append([t] + [times[n].get(t, "") for n in names])

    sheets.insert(0, (safe_sheet_name("Summary", taken), summary))
    write_xlsx(args.out, sheets)
    print(f"wrote {args.out} ({len(sheets)} sheets, {len(names)} experiments)")


if __name__ == "__main__":
    main()
