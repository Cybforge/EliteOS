#!/bin/sh
# Build the time zone database in root/usr/share/zoneinfo from the IANA data
# (tzdata, public domain). Needs zic and /usr/share/zoneinfo/tzdata.zi on the
# build host; the result is committed, so ordinary builds do not need either.
set -e
out=${1:-root/usr/share/zoneinfo}
src=${2:-/usr/share/zoneinfo}
rm -rf "$out"
mkdir -p "$out"
# "slim" files: the transitions up to the last rule change, then the POSIX rule
zic -b slim -d "$out" "$src/tzdata.zi"
cp "$src/zone1970.tab" "$src/iso3166.tab" "$out/"
# not needed: the "posixrules" link and the Factory zone
rm -f "$out/posixrules" "$out/Factory" "$out/leapseconds" "$out/leap-seconds.list"
# keep only the zones in zone1970.tab (one per region) plus UTC
python3 - "$out" <<PY
import os, sys
out = sys.argv[1]
keep = {"UTC", "Etc/UTC", "Etc/GMT", "GMT"}
for line in open(out + "/zone1970.tab"):
    if line.strip() and not line.startswith("#"):
        keep.add(line.split("\t")[2])
for root, dirs, files in os.walk(out):
    for f in files:
        rel = os.path.relpath(os.path.join(root, f), out)
        if rel not in keep and rel not in ("zone1970.tab", "iso3166.tab"):
            os.remove(os.path.join(root, f))
for root, dirs, files in os.walk(out, topdown=False):
    if root != out and not os.listdir(root):
        os.rmdir(root)
PY
echo "$(find "$out" -type f | wc -l) files, $(du -sk "$out" | cut -f1) KiB"
