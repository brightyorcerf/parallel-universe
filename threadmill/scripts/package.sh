#!/usr/bin/env bash
# Builds the submission archive: scripts/package.sh <TEAMNUM> <ASSIGN_NUM>
#
# Ships sources, scripts, README, the generated gnuplot scripts and PNGs, and
# the Excel workbook. Deliberately excludes bin/ and any object files -- the
# brief says source files only.
set -euo pipefail

cd "$(dirname "$0")/.."

[[ $# -eq 2 ]] || { echo "usage: scripts/package.sh <TEAMNUM> <ASSIGN_NUM>" >&2; exit 1; }
NAME="$1_ASSIGN$2"
STAGE=".package/$NAME"

rm -rf .package
mkdir -p "$STAGE"

cp -R src scripts Makefile README.md "$STAGE/"
mkdir -p "$STAGE/plots"
cp plots/plot_all.sh "$STAGE/plots/"
# Generated but worth shipping: the .gp scripts are what "drawn using gnuplot"
# means, and the PNGs are the figures themselves.
for f in plots/*.gp plots/*.png; do [[ -e $f ]] && cp "$f" "$STAGE/plots/"; done
[[ -d results ]] && cp -R results "$STAGE/"

if [[ -f "$NAME.xlsx" ]]; then
    cp "$NAME.xlsx" "$STAGE/"
else
    echo "note: $NAME.xlsx not found -- run make_excel.py --out $NAME.xlsx first" >&2
fi

find "$STAGE" -name '*.o' -o -name '.DS_Store' -o -name '__pycache__' | xargs -r rm -rf

( cd .package && zip -qr "../$NAME.zip" "$NAME" )
rm -rf .package

echo "wrote $NAME.zip"
unzip -l "$NAME.zip" | tail -n +4 | head -40
