"""Deterministic offline composition queue. Production C++ promotion is pending.

Jobs read declared immutable predecessor products and return proposals. A barrier
commits results in stable ID order. Halos are read/proposal domains, never ownership.
"""

from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from hashlib import sha256
from types import MappingProxyType
import time


def purpose_seed(world_seed, region_id, strategy, version=1):
    value = f"{world_seed}:{region_id}:{strategy}:{version}".encode()
    return int.from_bytes(sha256(value).digest()[:8], "little")


@dataclass(frozen=True)
class SolverJob:
    id: str
    dependencies: tuple
    solve: object


class SolverQueue:
    def __init__(self, workers=1):
        if not 1 <= workers <= 8:
            raise ValueError("worker budget must be 1..8")
        self.workers = workers
        self.trace = []

    def run(self, jobs, initial=None):
        pending = {j.id: j for j in jobs}
        if len(pending) != len(jobs):
            raise ValueError("duplicate job ID")
        result = dict(initial or {})
        if set(result) & set(pending):
            raise ValueError("job shadows frozen input")
        known = set(result) | set(pending)
        if any(set(j.dependencies) - known for j in jobs):
            raise ValueError("unknown dependency")
        # Validate the entire DAG before invoking any producer.
        available = set(result)
        while len(available) < len(known):
            ready = [
                j.id
                for j in jobs
                if j.id not in available and set(j.dependencies) <= available
            ]
            if not ready:
                raise ValueError("cyclic dependencies")
            available.update(ready)
        with ThreadPoolExecutor(max_workers=self.workers) as pool:
            while pending:
                ready = sorted(
                    k
                    for k, j in pending.items()
                    if set(j.dependencies) <= result.keys()
                )

                def invoke(key):
                    j = pending[key]
                    view = MappingProxyType({d: result[d] for d in j.dependencies})
                    return j.solve(view)

                started = time.perf_counter()
                proposals = list(pool.map(invoke, ready))
                # Failed producers never commit any result from this barrier.
                for key, proposal in zip(ready, proposals):
                    if proposal is None:
                        raise ValueError(f"{key}: missing product")
                for key, proposal in zip(ready, proposals):
                    result[key] = proposal
                    del pending[key]
                self.trace.append(
                    {"jobs": ready, "seconds": time.perf_counter() - started}
                )
        return result
