"""
# France departements as polygons
"""

from pathlib import Path

import numpy as np
import numpy.random as nr
from datoviz import canvas, run, colormap

ROOT = Path(__file__).resolve().parent.parent.parent.parent

pos = np.fromfile(
    ROOT / "data/misc/departements.polypoints.bin", dtype=np.float64)
pos = pos.reshape((-1, 2))
pos = np.c_[pos[:, 1], pos[:, 0], np.zeros(pos.shape[0])]
# latitude, longitude, 0

# Web Mercator projection
lat, lon, _ = pos.T
lonrad = lon / 180.0 * np.pi
latrad = lat / 180.0 * np.pi
zoom = 1
c = 256 / 2 * np.pi * 2 ** zoom
x = c * (lonrad + np.pi)
y = -c * (np.pi - np.log(np.tan(np.pi / 4.0 + latrad / 2.0)))
pos = np.c_[x, y, _]

length = np.fromfile(
    ROOT / "data/misc/departements.polylengths.bin", dtype=np.uint32)
N = len(length)
color = colormap(nr.rand(N), vmin=0, vmax=1, cmap='viridis')

c = canvas(show_fps=False)
panel = c.panel(controller='axes')
visual = panel.visual('polygon')

visual.data('pos', pos)
visual.data('length', length)
visual.data('color', color)

run()
