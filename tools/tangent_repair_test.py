import copy,math
from product_geometry import repair_tangents
source=dict(positions=[[0,0,0],[1,0,0],[0,1,0]],normals=[[0,0,1]]*3,uvs=[[0,0],[1,0],[0,1]],indices=[0,1,2],tangents=[[None,-1,-1,-1],[1,0,0,1],[float('nan'),0,0,1]])
original=copy.deepcopy(source)
assert repair_tangents(source)==2
assert source['tangents']==[[1,0,0,1]]*3
for key in ['positions','normals','uvs','indices']:assert source[key]==original[key]
assert repair_tangents(source)==0
source['uvs']=[[0,0]]*3;source['tangents'][0]=[None,0,0,1]
assert repair_tangents(source)==1
assert abs(sum(x*x for x in source['tangents'][0][:3])-1)<1e-6
source['normals'][0]=[0,0,0];source['tangents'][0]=[None,0,0,1]
try:repair_tangents(source)
except ValueError:pass
else:raise AssertionError('Invalid normal was accepted')
print('PASS tangent reconstruction, retained geometry, degenerate UVs and invalid-normal rejection')
