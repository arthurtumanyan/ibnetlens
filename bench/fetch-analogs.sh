#!/bin/sh
# Fetches third-party tools used as comparison baselines into bench/.analogs (not committed).
# ibtopology is a Python 2 script; it is ported with the minimal edits listed below.
set -eu
DIR=$(dirname "$0")/.analogs
mkdir -p "$DIR/pylib"

curl -fsS --max-time 60 -o "$DIR/ibtopology.orig" https://raw.githubusercontent.com/kostty/ibtopo/master/ibtopology
cp "$DIR/ibtopology.orig" "$DIR/ibtopology3.py"
# print statement -> function, iteritems/itervalues -> items/values
sed -i 's|^#!/usr/bin/python$|#!/usr/bin/python3|; s|^\( *\)print line$|\1print(line)|; s|iteritems()|items()|g; s|itervalues()|values()|g' "$DIR/ibtopology3.py"
pip install --quiet --target "$DIR/pylib" python-hostlist xmltodict
echo "ready: $DIR/ibtopology3.py"
