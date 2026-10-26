#!/usr/bin/env python3
"""Migrate old preview exports: archive diagnostics, retain real residence changes.

Requires ijson for bounded-memory parsing of legacy gigabyte-sized snapshots.
New CityEvolutionPreview exports already select only these residence events.
"""
import argparse
from collections import Counter
import gzip
import json
from pathlib import Path
import shutil
import ijson


def compact(path, recover=False):
    archive = path.with_name('history-full.json.gz')
    if archive.exists():
        return
    builder = ijson.common.ObjectBuilder()
    residence = []
    counts = Counter()
    item = None
    truncated = []
    def events(source):
        try:
            yield from ijson.parse(source, use_float=True)
        except ijson.common.IncompleteJSONError:
            if not recover:
                raise
            truncated.append(True)
    with path.open('rb') as source:
        for prefix, event, value in events(source):
            if prefix == 'residenceEvents':
                if event == 'start_array':
                    builder.event('start_array', None)
                    builder.event('end_array', None)
                continue
            if prefix.startswith('residenceEvents.'):
                if prefix == 'residenceEvents.item' and event == 'start_map':
                    item = ijson.common.ObjectBuilder()
                if item is not None:
                    item.event(event, value)
                if prefix == 'residenceEvents.item' and event == 'end_map':
                    entry = item.value
                    counts[entry['kind']] += 1
                    if entry['kind'] in ('tenancy-assigned', 'tenancy-released', 'relocated'):
                        residence.append(entry)
                    item = None
                continue
            builder.event(event, value)
    result = builder.value
    if truncated:
        # Only the residence-event tail may be recovered. Current-state arrays
        # must already be complete and agree with the completed run's manifest.
        for key in ('ancestry', 'citizens', 'buildings', 'householdRecords', 'migrationEvents', 'population'):
            if key not in result:
                raise ValueError('truncation affects the current snapshot: ' + key)
        year = int(path.parent.parent.name.split('-')[1])
        settlement = int(path.parent.name.split('-')[1])
        report = json.loads((path.parents[2] / 'report.json').read_text())
        milestone = next(m for m in report['milestones'] if m['year'] == year)
        summary = next(s for s in milestone['settlements'] if s['id'] == settlement)
        assert len(result['citizens']) == summary['population'] == result['population']
        assert len(result['buildings']) == summary['buildings']
        result.update(year=year, week=milestone['week'], settlement=settlement,
                      schemaVersion=1, waitingHouseholds=summary['waitingHouseholds'],
                      workers=sum(h['workers'] for h in result['householdRecords']),
                      socialSource='CitySocietyDriver scale fixture; not graph matcher',
                      residenceHistoryComplete=False,
                      exportWarnings=['Legacy export ran out of disk space in the residence-event tail. Current-state arrays are complete; some past residence events are missing.'])
    result['residenceEvents'] = residence
    result['diagnosticArchive'] = archive.name
    result['residenceExportCounts'] = dict(counts)
    # Preserve the exact legacy source before atomically replacing its projection.
    with path.open('rb') as source, gzip.open(archive, 'wb', compresslevel=1) as target:
        shutil.copyfileobj(source, target)
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(result, separators=(',', ':')))
    temporary.replace(path)
    print(path, 'events:', sum(counts.values()), '->', len(residence), flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('root', type=Path)
    parser.add_argument('--recover-truncated', action='store_true')
    args = parser.parse_args()
    for path in sorted(args.root.glob('year-*/settlement-*/history.json')):
        compact(path, args.recover_truncated)


if __name__ == '__main__':
    main()
