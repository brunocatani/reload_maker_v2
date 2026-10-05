"""A magazine's cooked models follow its presentation, not the reload capture
each receiver binds it to (the HK USP's receivers share one magazine)."""
import copy
from pathlib import Path
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from product_authoring import presentation_identity

def main():
    setup={'version':1,'recipeId':'a'*64,'emptyTrack':6,'motion':'preset','sampledPayload':False,
           'pieces':[{'path':'Mag/WeaponMagazine/Child2/Body','role':'body'},{'path':'Mag/WeaponMagazine/Child3/Body','role':'spare'}]}
    other_receiver=copy.deepcopy(setup);other_receiver['recipeId']='b'*64
    assert presentation_identity(setup)==presentation_identity(other_receiver),'Another receiver\'s capture changed the presentation'
    roles=copy.deepcopy(other_receiver);roles['pieces'][1]['role']='body'
    assert presentation_identity(setup)!=presentation_identity(roles),'A changed piece role kept the cooked models'
    sampled=copy.deepcopy(setup);sampled['sampledPayload']=True
    resampled=copy.deepcopy(sampled);resampled['recipeId']='b'*64
    assert presentation_identity(sampled)!=presentation_identity(resampled),'A payload sampled from another capture kept the cooked models'
    assert presentation_identity(None) is None and presentation_identity({'rounds':{'a':True}})=={'rounds':{'a':True}}
    print('PASS presentation identity ignores the per-receiver capture binding unless the payload is sampled from it',flush=True)

if __name__=='__main__':main()
