"""Building-use demand for city block allocation; no shells or interiors yet."""

# Prototype/game parameters in metres, not historical or engineering standards.
BUILDING_TYPES = {
    'cottage': dict(floors=[1,2], frontageMetres=[6,9], depthMetres=[8,12], householdUnits=1, lotAreaMetres2=240),
    'shop-house': dict(floors=[2,3], frontageMetres=[5,8], depthMetres=[12,18], householdUnits=1, lotAreaMetres2=160),
    'courtyard-house': dict(floors=[1,2], frontageMetres=[12,20], depthMetres=[16,26], householdUnits=2, lotAreaMetres2=520),
    'workshop-yard': dict(floors=[1,2], frontageMetres=[8,14], depthMetres=[12,22], householdUnits=0, lotAreaMetres2=420),
    'warehouse': dict(floors=[1,2], frontageMetres=[10,18], depthMetres=[16,28], householdUnits=0, lotAreaMetres2=540),
    'inn': dict(floors=[2,3], frontageMetres=[12,20], depthMetres=[18,28], householdUnits=0, lotAreaMetres2=650),
    'hall-chapel': dict(floors=[1], frontageMetres=[10,16], depthMetres=[18,30], householdUnits=0, lotAreaMetres2=700),
    'keep': dict(floors=[2,4], frontageMetres=[14,24], depthMetres=[16,26], householdUnits=0, lotAreaMetres2=1100),
    'barn': dict(floors=[1], frontageMetres=[8,14], depthMetres=[12,22], householdUnits=0, lotAreaMetres2=450),
}

DISTRICT_TYPES = {
    'merchant': ['shop-house','inn','warehouse','courtyard-house'],
    'residential': ['cottage','courtyard-house'],
    'craftsmen': ['workshop-yard','cottage','warehouse'],
    'civic': ['hall-chapel'],
    'castle': ['keep'],
    'homesteads': ['cottage','barn'],
    'growth-reserve': [],
}


def demand(morphology, households):
    if not isinstance(households,int) or households < 0:
        raise ValueError('Household demand must be a nonnegative integer')
    if morphology == 'lane-and-homesteads':
        return {'cottage':households, 'barn':(households+2)//3}
    if morphology not in ('market-frontage','terrace-and-bailey'):
        raise ValueError('Unsupported settlement morphology')
    shops = households//3 if morphology=='market-frontage' else households//5
    courts = households//10
    return {'cottage':households-shops-2*courts, 'shop-house':shops,
            'courtyard-house':courts, 'workshop-yard':max(1,households//15),
            'warehouse':max(1,households//40),'inn':1,'hall-chapel':1,
            **({'keep':1} if morphology=='terrace-and-bailey' else {})}
