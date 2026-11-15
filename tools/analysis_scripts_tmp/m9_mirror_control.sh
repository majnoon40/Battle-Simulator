#!/bin/bash
# M9 pre-step 3: demo-river mirrored horizontally (west<->east), factions
# symmetric (same armies), run 100 seeds. Report-only control.
set -e
mkdir -p /tmp/m9flip/maps /tmp/m9flip/scenarios
python3 - <<'PY'
from PIL import Image
im = Image.open('data/maps/demo-river.png')
im.transpose(Image.FLIP_LEFT_RIGHT).save('/tmp/m9flip/maps/demo-river-flip.png',
                                         optimize=False)
print('flipped', im.size)
PY
# Same scenario, map file swapped (bands are symmetric: Red west / Blue east).
python3 - <<'PY'
import json
s = json.load(open('data/scenarios/demo-river.json'))
s['map']['file'] = 'demo-river-flip.png'
json.dump(s, open('/tmp/m9flip/scenarios/demo-river-flip.json', 'w'), indent=2)
PY
for s in $(seq 1 100); do
  ./build/battle_cli --mod /tmp/m9flip --scenario demo-river-flip --seed $s 2>/dev/null |
    grep -E "Ticks elapsed|Termination reason|Winner|Total deaths" |
    sed 's/  */ /g' | tr '\n' ';' | sed "s/^/seed $s /"
  echo
done
