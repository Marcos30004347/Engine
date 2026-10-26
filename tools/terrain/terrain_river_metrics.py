"""Measurements on actual river routing points, not image smoothing."""

import numpy as np


def straight_runs(product):
    runs = []
    for river in product["rivers"]:
        v = np.diff(np.array(river["routingPoints"]), axis=0)
        lengths = np.linalg.norm(v, axis=1)
        valid = lengths > 1e-8
        v = v[valid]
        lengths = lengths[valid]
        unit = v / lengths[:, None]
        previous = None
        run = 0.0
        for direction, length in zip(unit, lengths):
            if previous is not None and np.linalg.norm(direction - previous) > 1e-5:
                runs.append(run)
                run = 0.0
            run += float(length)
            previous = direction
        if run:
            runs.append(run)
    return dict(
        maximumStraightRunMetres=max(runs, default=0.0),
        p95StraightRunMetres=float(np.percentile(runs, 95)) if runs else 0.0,
        runsOver1000Metres=sum(v > 1000 for v in runs),
    )
