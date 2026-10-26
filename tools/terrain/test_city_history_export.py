"""History joins must retain precise IDs and distinguish missing geometry."""
import unittest
from city_history_export import attach_history

class CityHistoryTests(unittest.TestCase):
    def test_exact_links_and_unrealised_people(self):
        h = 2**63 + 21
        history = {'settlement':100, 'year':60, 'week':3120,
            'buildings':[{'id':h,'builtWeek':520,'use':'home'},{'id':h+1,'trade':'farmer'}],
            'citizens':[{'id':h+3,'household':h+9,'home':h,'workBuilding':h+1,'bornWeek':2080,'occupation':'farmer'},
                        {'id':h+4,'home':h+1}],
            'workEvents':[{'person':h+3,'week':3000,'kind':'hired'}]}
        buildings=[{'id':h+7}]
        result=attach_history(buildings,[{'historicalBuilding':h,'generatedBuilding':h+7},
            {'historicalBuilding':h+1,'generatedBuilding':None}],history)
        person=result['people'][0]
        self.assertEqual(person['id'],str(h+3))
        self.assertEqual(person['homeId'],str(h+7))
        self.assertIsNone(person['workId'])
        self.assertEqual(person['workHistoricalId'],str(h+1))
        self.assertEqual(person['age'],20)
        self.assertEqual(buildings[0]['history']['residents'],[str(h+3)])
        self.assertEqual(buildings[0]['history']['builtYear'],10)
        self.assertEqual(result['unplacedResidents'],1)
        self.assertEqual(result['unrealisedBuildings'],1)
    def test_never_infers_binding_from_equal_ids(self):
        buildings=[{'id':7}]
        result=attach_history(buildings,[],{'buildings':[{'id':7}],'citizens':[{'id':8,'home':7}]})
        self.assertNotIn('history',buildings[0])
        self.assertIsNone(result['people'][0]['homeId'])

    def test_household_past_is_not_a_new_members_biography(self):
        result=attach_history([],[],{'year':60,'week':3120,
            'citizens':[{'id':7,'household':9,'bornWeek':2600}],
            'residenceEvents':[{'household':9,'week':520,'kind':'tenancy-assigned'}]})
        self.assertEqual(result['people'][0]['events'],[])

if __name__=='__main__': unittest.main()
