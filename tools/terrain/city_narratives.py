"""Evidence-backed story arcs over a dated snapshot; never invent an outcome."""
from collections import defaultdict


def build_narratives(history, people):
    people_by_id = {p['id']: p for p in people}
    epoch = history.get('week', 0) - history.get('year', 0) * 52
    year = lambda week: round((week-epoch)/52, 1)
    names={str(p['id']):p.get('name',f"Person {p['id']}") for p in history.get('ancestry', [])}
    names.update({p['id']:p['name'] for p in people})
    name = lambda pid: names.get(str(pid), f'Person {pid}')
    house_names={}
    for p in history.get('ancestry',[]):
        if p.get('dynasty') is not None and p.get('name'):house_names[p['dynasty']]=p['name'].split()[-1]
    house=lambda id:house_names.get(id,str(id))
    arcs = []
    sid = str(history.get('settlement', ''))

    def add(key, title, status, participants, episodes):
        episodes.sort(key=lambda e: e['year'])
        arcs.append({'id': f'{sid}:{key}', 'title': title, 'status': status,
                     'participants': sorted({str(p) for p in participants if str(p) in people_by_id}),
                     'episodes': episodes, 'lastYear': episodes[-1]['year']})

    # Annual world stories already carry explicit causal edges. Read those
    # facts directly; no economic snapshots or ledger reconstruction.
    graph_events = {e['id']: e for e in history.get('stories', [])}
    graph_epoch = history.get('week', 0) / 52 - history.get('year', 0)
    caused = {e.get('cause') for e in graph_events.values()}
    for event in graph_events.values():
        if event['kind'] not in ('legendary founding', 'land charter', 'poor harvest', 'recovery', 'succession'):
            continue
        if event['kind'] == 'poor harvest' and event['id'] in caused:
            continue
        causes = [graph_events[event['cause']]] if event.get('cause') in graph_events else []
        episodes = [{'year': e['year'] - graph_epoch, 'text': e['narrative'],
                     'evidence': ('assumed legendary premise' if e.get('assumed') else 'story graph') + ':' + str(e['id'])}
                    for e in causes + [event]]
        if episodes[-1]['year'] > history.get('year', 0):
            continue
        title = {'poor harvest': 'The lean harvests', 'recovery': 'Rebuilding after hardship',
                 'succession': 'The changing of leadership', 'legendary founding': 'The legendary founding',
                 'land charter': 'The inherited land charter'}[event['kind']]
        add('graph-' + event['kind'].replace(' ', '_') + '-' + str(event['id']), title,
            'legendary premise' if event.get('assumed') else 'recovered' if event['kind'] == 'recovery' else 'recorded',
            [event.get('subject')], episodes)

    economy = {e['year']: e for e in history.get('economicYears', [])}
    harvest = {e['year']: e['yieldPerMille'] for e in history.get('harvestYears', []) if e['year']<=history.get('year',float('inf'))}
    for y, value in sorted(harvest.items()):
        if value >= 1000 or harvest.get(y-1, 1000) < 1000:
            continue
        episodes=[]; end=y
        while end in harvest:
            value=harvest[end]
            change="fell to" if end==y else "remained below normal at" if value<1000 else "recovered to"
            text=f"Farm yields {change} {value/10:g}% of the ordinary harvest."
            hunger=economy.get(end, {}).get('hungerEpisodes', 0)
            if hunger: text+=f' Households experienced {hunger} recorded hunger episodes that year.'
            episodes.append({'year': end, 'text': text, 'evidence': 'harvest-policy + economic-ledger'})
            if value>=1000: break
            end+=1
        resolved=harvest.get(end,0)>=1000
        add(f'harvest-{y}', 'The lean harvests', 'recovered' if resolved else 'ongoing', [], episodes)

    careers=defaultdict(list)
    for e in history.get('workEvents', []):
        careers[str(e['person'])].append(e)
    phrases={'apprenticed':'began an apprenticeship', 'graduated':'completed their apprenticeship',
             'mastered':'was recognised as a master', 'ambition':'set out to open a business',
             'opened':'opened their own business', 'released':'lost a position', 'hired':'found paid work',
             'self-employed':'began working independently', 'retired':'retired from working life'}
    for pid, events in careers.items():
        events.sort(key=lambda e:e['week'])
        meaningful=[e for e in events if e['kind'] in phrases]
        milestones=[e for e in meaningful if e['kind'] in ('apprenticed','graduated','mastered','ambition','opened')]
        if not milestones: continue
        # Keep turning points, not a chapter for every hiring retry.
        def career_text(e):
            job=e.get('occupation')
            business={'baker':'bakery','brewer':'brewery','weaver':'weaving workshop','potter':'pottery',
                      'herbalist':'herbalist shop','scribe':'scriptorium','smith':'smithy','farmer':'farm',
                      'merchant':'shop','innkeeper':'inn'}.get(job,'workshop')
            phrase=phrases[e['kind']]
            if job:
                phrase={'apprenticed':f'began training as an apprentice {job}',
                        'graduated':f'completed their training as a {job}',
                        'mastered':f'earned recognition as a master {job}',
                        'ambition':f'sought a {business} of their own',
                        'opened':f'opened their own {business}'}.get(e['kind'],phrase)
            return f'{name(pid)} {phrase}.'
        episodes=[{'year':year(e['week']), 'text':career_text(e),
                   'evidence':f"economy:{e['kind']}:{e['week']}"} for e in milestones[-8:]]
        opened=max((e['week'] for e in milestones if e['kind']=='opened'),default=-1)>max((e['week'] for e in milestones if e['kind']=='ambition'),default=-1)
        ambition=any(e['kind']=='ambition' for e in milestones)
        status='business opened' if opened else 'seeking a business' if ambition else 'master' if milestones[-1]['kind']=='mastered' else 'career developing'
        death=next((e for e in history.get('socialEvents',[]) if e['kind']=='death' and str(e['subject'])==pid),None)
        if death:
            episodes.append({'year':year(death['week']),'text':f'{name(pid)} died.', 'evidence':f"social:death:{death['week']}"})
            status='unfinished at death' if ambition and not opened else 'remembered career'
        title=f"{name(pid)} — a business of their own" if opened else f"{name(pid)} and the unfinished ambition" if ambition else f"The making of {name(pid)}"
        add(f'career-{pid}', title, status, [pid], episodes)

    feuds={}
    for e in sorted(history.get('socialEvents', []),key=lambda e:e['week']):
        kind=e['kind'];a=e['subject'];other=e.get('after') if kind=='feud-started' else e.get('before')
        if kind in ('feud-started','feud-ended') and other is not None:
            pair=tuple(sorted((a,other)))
            if kind=='feud-started':feuds[pair]=e
            elif pair in feuds:
                start=feuds.pop(pair)
                add(f'feud-{a}-{other}-{start["week"]}',f'Houses {house(pair[0])} and {house(pair[1])}: a broken peace','ended',[],[
                    {'year':year(start['week']),'text':'The two houses entered a feud, raising tension in the realm.','evidence':f'social:feud-started:{start["week"]}'},
                    {'year':year(e['week']),'text':'Their feud ended. The record does not identify a mediator.','evidence':f'social:feud-ended:{e["week"]}'}])
        elif kind=='succession' and e.get('after') is not None:
            add(f'succession-{a}-{e["week"]}','The changing of the crown','succession recorded',[e.get('before'),e['after']],[
                {'year':year(e['week']),'text':(f"{name(e['after'])} succeeded {name(e['before'])} as ruler." if e.get('before') is not None else f"{name(e['after'])} became ruler after a vacancy."),'evidence':f'social:succession:{e["week"]}'}])
    for pair,e in feuds.items():
        add(f'feud-{pair[0]}-{pair[1]}-{e["week"]}',f'Houses {house(pair[0])} and {house(pair[1])}: an unsettled feud','unresolved',[],[
            {'year':year(e['week']),'text':'The houses entered a feud; no end has been recorded at this date.','evidence':f'social:feud-started:{e["week"]}'}])

    # Keep a varied anthology; a populous city's careers must not bury its crises.
    groups=defaultdict(list)
    for arc in sorted(arcs,key=lambda a:(-a['lastYear'],a['id'])):
        groups[arc['id'].split(':',1)[1].rsplit('-',1)[0] if ':graph-' in arc['id'] else arc['id'].split(':',1)[1].split('-',1)[0]].append(arc)
    selected=[]
    while len(selected)<16 and any(groups.values()):
        for group in sorted(groups):
            if groups[group] and len(selected)<16:selected.append(groups[group].pop(0))
    return selected
