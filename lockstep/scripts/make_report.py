"""Builds report/report.html from report/body.html + results/averages.csv + plots, then prints it to PDF with Chrome."""
import csv, os, subprocess
from collections import defaultdict

here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
avg = defaultdict(dict)
for r in csv.DictReader(open(os.path.join(here, "results", "averages.csv"))):
    avg[(r["impl"], r["N"], r["workload"])][int(r["threads"])] = (float(r["avg_mops"]), float(r["speedup"]))

T = [1, 2, 4, 6, 8, 10, 12, 14, 16]
IMPLS = ["LazyDList", "LockFreeDList", "LockStack", "TreiberStack", "EliminationStack", "LockQueue", "MSQueue"]
WLS = ["0C-50I-50D", "50C-25I-25D", "100C-0I-0D"]

def table(n):
    h = [f"<h3>N = {n}: average throughput in MOPS (speedup in parentheses)</h3><table><tr><th>Impl</th><th>Workload</th>"]
    h += [f"<th>{t}</th>" for t in T] + ["</tr>"]
    for impl in IMPLS:
        for wl in WLS:
            d = avg.get((impl, str(n), wl), {})
            h.append(f"<tr><td>{impl}</td><td>{wl}</td>" + "".join(
                f"<td>{d[t][0]:.2f}<br><small>({d[t][1]:.2f})</small></td>" if t in d else "<td>-</td>" for t in T) + "</tr>")
    return "".join(h) + "</table>"

def fig(name, cap):
    return f'<figure><img src="../plots/{name}.png"><figcaption>{cap}</figcaption></figure>'

body = open(os.path.join(here, "report", "body.html")).read()
body = body.replace("{{DISCUSSION}}", open(os.path.join(here, "report", "discussion.html")).read())
body = body.replace("{{TABLES}}", table(1000) + table(10000))
for fam, cap in [("dlist", "Doubly linked list: Lazy vs Non-Blocking"), ("stack", "Stack: Lazy lock-based vs Treiber"),
                 ("queue", "Queue: two-lock vs Michael-Scott"), ("treiber_vs_elimination", "Treiber vs Elimination Backoff stack")]:
    body = body.replace("{{FIG_%s}}" % fam, fig(f"{fam}_throughput", cap + " - throughput") + fig(f"{fam}_speedup", cap + " - speedup"))

html = os.path.join(here, "report", "report.html")
open(html, "w").write(body)
subprocess.run(["/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", "--headless", "--disable-gpu",
                "--no-pdf-header-footer", "--print-to-pdf=" + os.path.join(here, "report", "report.pdf"), "file://" + html],
               check=True, capture_output=True)
print("report/report.pdf written")
