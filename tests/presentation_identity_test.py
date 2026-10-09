"""A magazine's cooked models follow its presentation, never the reload
capture id it was saved from (the HK USP's receivers share one magazine; no
hash gating, owner 2026-10-05)."""
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
    movement=copy.deepcopy(setup);movement.update(motion='linear',entrySeconds=1.5,loadedFrame=23,emptyFrame=42)
    assert presentation_identity(setup)==presentation_identity(movement),'A movement-only edit required recooking unchanged geometry'
    movement.update(motion='manual',movementId='authored-movement')
    assert presentation_identity(setup)==presentation_identity(movement),'A manual movement invalidated cooked geometry'
    roles=copy.deepcopy(other_receiver);roles['pieces'][1]['role']='body'
    assert presentation_identity(setup)!=presentation_identity(roles),'A changed piece role kept the cooked models'
    sampled=copy.deepcopy(setup);sampled['sampledPayload']=True
    sampled['loadedFrame']=3;changed_sample=copy.deepcopy(sampled);changed_sample['loadedFrame']=8
    assert presentation_identity(sampled)!=presentation_identity(changed_sample),'A genuinely different sampled payload lost its geometry check'
    resampled=copy.deepcopy(sampled);resampled['recipeId']='b'*64
    assert presentation_identity(sampled)==presentation_identity(resampled),'A capture id gated a sampled payload'
    assert presentation_identity(None) is None and presentation_identity({'rounds':{'a':True}})=={'rounds':{'a':True}}
    print('PASS presentation identity follows the pieces and their roles, never the capture id',flush=True)

if __name__=='__main__':main()
