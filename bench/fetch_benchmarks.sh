#!/usr/bin/env bash
# Download SAT Competition benchmarks from the Global Benchmark Database
# (https://benchmark-database.de) into bench/instances/<track>/.
#
#   ./fetch_benchmarks.sh                # SAT Competition 2023 main track (400)
#   ./fetch_benchmarks.sh main_2024      # another track
#
# The same 400 instances of 2023 are also archived on Zenodo
# (doi:10.5281/zenodo.11426992); if this script cannot reach the database,
# download them from there and unpack into bench/instances/main_2023/.
set -euo pipefail
track="${1:-main_2023}"
here="$(cd "$(dirname "$0")" && pwd)"
dest="$here/instances/$track"
mkdir -p "$dest"
list="$dest/urls.txt"
if [ ! -s "$list" ]; then
  echo "fetching the list of $track instances ..."
  curl -fsSL "https://benchmark-database.de/getinstances?query=track%3D${track}&context=cnf" -o "$list" ||
    curl -fsSL "https://benchmark-database.de/getinstances?track=${track}&context=cnf" -o "$list"
fi
n=$(grep -c . "$list" || true)
echo "$n instances listed; downloading into $dest (already present files are skipped)"
cd "$dest"
# --content-disposition keeps the original names (<hash>-<name>.cnf.xz)
wget -q --show-progress --content-disposition -nc -i "$list" || true
echo "done: $(ls -1 "$dest" | grep -c '\.cnf') files in $dest"
