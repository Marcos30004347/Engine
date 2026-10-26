"""Join the simulation to realised buildings by IDs; never guess a tenancy."""
from collections import Counter, defaultdict
from city_narratives import build_narratives


def attach_history(buildings, bindings, history):
    sid = lambda value: None if value is None else str(value)
    physical = {sid(b['id']): b for b in buildings}
    historical = {sid(b['id']): b for b in history.get('buildings', [])}
    realised = {sid(b['historicalBuilding']): sid(b.get('generatedBuilding')) for b in bindings
                if sid(b.get('generatedBuilding')) in physical}
    ancestry = {sid(p['id']): p for p in history.get('ancestry', [])}
    activities = {sid(a['person']): a for a in history.get('activities', [])}
    week = history.get('week', 0)
    offset = history.get('year', 0) - week / 52
    year = lambda w: round(offset + w / 52, 1)
    work_events, moves = defaultdict(list), defaultdict(list)
    for event in history.get('workEvents', []):
        work_events[sid(event['person'])].append({'year': year(event['week']), 'kind': event['kind'], 'detail':event.get('occupation') or ''})
    for event in history.get('migrationEvents', []):
        work_events[sid(event['person'])].append({'year': year(event['week']), 'kind': event['kind'],
            'detail': f"Settlement {event.get('before') or 'outside the region'} → {event.get('after') or 'outside the region'}"})
    for event in history.get('socialEvents', []):
        if event['kind'] in ('marriage', 'separation', 'role-change'):
            work_events[sid(event['subject'])].append({'year':year(event['week']), 'kind':event['kind'],
                'detail': ancestry.get(sid(event.get('after') or event.get('before')), {}).get('name','') if event['kind'] in ('marriage','separation') else ''})
    for event in history.get('residenceEvents', []):
        moves[sid(event['household'])].append({'year': year(event['week']), 'kind': event['kind'],
            'buildingId': realised.get(sid(event.get('building'))), 'detail': event.get('detail', '')})
    residents, workers = defaultdict(list), defaultdict(list)
    people = []
    for person in history.get('citizens', []):
        pid, hid = sid(person['id']), sid(person.get('household'))
        ancestor = ancestry.get(pid, {})
        born = person.get('bornWeek', ancestor.get('bornWeek'))
        activity = activities.get(pid, {})
        record = {'id': pid, 'name': person.get('name', f'Resident {pid}'), 'householdId': hid,
            'age': max(0, (week-born)//52) if born is not None else None,
            'occupation': person.get('occupation'), 'apprentice': bool(person.get('apprentice')),
            'duty': person.get('duty', ''), 'homeId': realised.get(sid(person.get('home'))),
            'workId': realised.get(sid(person.get('workBuilding'))),
            'homeHistoricalId': sid(person.get('home')), 'workHistoricalId': sid(person.get('workBuilding')),
            'parents': [sid(p) for p in ancestor.get('parents', [])], 'spouse': sid(ancestor.get('spouse')),
            'activity': activity.get('activity'),
            'careerStage': person.get('careerStage'), 'ambition': person.get('ambition'), 'mentorId':sid(person.get('mentor')),
            'parentNames': [ancestry.get(sid(p),{}).get('name',f'Person {p}') for p in ancestor.get('parents',[])],
            'spouseName': ancestry.get(sid(ancestor.get('spouse')),{}).get('name'),
            'events': sorted(work_events[pid], key=lambda e: e['year'])[-12:]}
        people.append(record)
        if person.get('home') is not None: residents[sid(person['home'])].append(pid)
        if person.get('workBuilding') is not None: workers[sid(person['workBuilding'])].append(pid)
    for historic_id, physical_id in realised.items():
        source = historical.get(historic_id, {})
        physical[physical_id]['history'] = {'id': historic_id, 'use': source.get('use', ''),
            'trade': source.get('trade', ''), 'builtYear': year(source['builtWeek']) if 'builtWeek' in source else None,
            'residents': residents[historic_id], 'workers': workers[historic_id],
            'events': moves[sid(source.get('household'))][-8:]}
    eras = Counter(int(year(b['builtWeek']) // 10) * 10 for b in historical.values() if 'builtWeek' in b)
    chronicle = [{'year': era, 'text': f'{count} surviving buildings date from years {era}–{era+9}.'}
                 for era, count in sorted(eras.items())]
    arrivals = sum(e.get('after') == history.get('settlement') for e in history.get('migrationEvents', []))
    return {'stories':build_narratives(history,people), 'chronicle': chronicle, 'recordedArrivals': arrivals, 'waitingHouseholds': history.get('waitingHouseholds', 0), 'name': history.get('name', f"Settlement {history.get('settlement', '')}"),
        'settlementId': sid(history.get('settlement')), 'role': history.get('role', 'settlement'),
        'year': history.get('year'), 'week': week, 'population': history.get('population', len(people)),
        'source': history.get('socialSource', ''), 'people': people,
        'occupations': dict(Counter(p['occupation'] or 'no assigned occupation' for p in people)),
        'linkedBuildings': len(realised), 'unrealisedBuildings': len(historical)-len(realised),
        'unplacedResidents': sum(p['homeId'] is None for p in people),
        'regionalLinks': [sid(p) for p in history.get('regionalLinks', [])]}
