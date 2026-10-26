import unittest
from city_narratives import build_narratives

class NarrativeTests(unittest.TestCase):
    def test_world_graph_uses_causal_events_and_labels_legends(self):
        history={'settlement':10000,'year':200,'week':1200*52,'stories':[
            {'id':1,'year':600,'kind':'legendary founding','subject':10000,'cause':0,'assumed':True,'narrative':'An inherited founding tale.'},
            {'id':2,'year':1080,'kind':'poor harvest','subject':10000,'cause':0,'narrative':'A poor harvest.'},
            {'id':3,'year':1085,'kind':'recovery','subject':44,'cause':2,'narrative':'The market was rebuilt.'}]}
        arcs=build_narratives(history,[])
        self.assertEqual(len(arcs),2)
        legend=next(a for a in arcs if a['status']=='legendary premise')
        self.assertEqual(legend['episodes'][0]['year'],-400)
        recovered=next(a for a in arcs if a['status']=='recovered')
        self.assertEqual([e['year'] for e in recovered['episodes']],[80,85])
        self.assertTrue(all('graph' in e['evidence'] for e in recovered['episodes']))

    def test_crisis_does_not_invent_future_recovery(self):
        h={'settlement':100,'week':52*9,'year':9,'harvestYears':[{'year':8,'yieldPerMille':550},{'year':9,'yieldPerMille':750}]}
        arc=build_narratives(h,[])[0]
        self.assertEqual(arc['status'],'ongoing')
        self.assertEqual(len(arc['episodes']),2)
        h['harvestYears'].append({'year':10,'yieldPerMille':1200})
        self.assertEqual(build_narratives(h,[])[0]['status'],'ongoing')
        h['year']=10;h['week']=520
        self.assertEqual(build_narratives(h,[])[0]['status'],'recovered')
    def test_new_ambition_is_not_resolved_by_old_business(self):
        h={'workEvents':[{'person':7,'week':10,'kind':'opened'},{'person':7,'week':20,'kind':'ambition'}]}
        a=build_narratives(h,[{'id':'7','name':'Mira Reed'}])[0]
        self.assertEqual(a['status'],'seeking a business')
        self.assertEqual(a['participants'],['7'])
    def test_feud_pair_closes_once_and_reopens_independently(self):
        h={'socialEvents':[{'kind':'feud-started','week':1,'subject':10,'after':20},
          {'kind':'feud-ended','week':53,'subject':20,'before':10},
          {'kind':'feud-started','week':90,'subject':10,'after':20}]}
        arcs=build_narratives(h,[])
        self.assertEqual(sorted(a['status'] for a in arcs),['ended','unresolved'])
        self.assertEqual(arcs,build_narratives(h,[]))
    def test_anthology_stays_bounded_and_keeps_crises(self):
        h={'workEvents':[{'person':i,'week':i+10,'kind':'ambition'} for i in range(100)],
           'harvestYears':[{'year':0,'yieldPerMille':550}]}
        arcs=build_narratives(h,[])
        self.assertEqual(len(arcs),16)
        self.assertTrue(any(a['title']=='The lean harvests' for a in arcs))

    def test_death_leaves_an_unfulfilled_ambition(self):
        h={'workEvents':[{'person':7,'week':10,'kind':'ambition'}],
           'socialEvents':[{'subject':7,'week':20,'kind':'death'}]}
        arc=build_narratives(h,[])[0]
        self.assertEqual(arc['status'],'unfinished at death')
        self.assertIn('died',arc['episodes'][-1]['text'])

if __name__=='__main__':unittest.main()
