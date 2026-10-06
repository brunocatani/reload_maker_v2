"""Multiple native material slots must remain one unambiguous runtime catalog."""
import copy
import sys
from pathlib import Path

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from product_authoring import merge_configuration_slots


def run():
    # The 1911's slide is its structural receiver; its two finish attach points
    # have the same display label but different records.
    captured=[dict(name='Receiver',records=['Eb_1911.esp:003da9']),
              dict(name='Magazine',records=['Eb_1911.esp:00081f']),
              dict(name='Material',records=['Eb_1911.esp:000819']),
              dict(name='Material',records=['Eb_1911.esp:00081b'])]
    original=copy.deepcopy(captured)
    merged=merge_configuration_slots(captured,captured)
    assert captured==original
    assert len(merged)==3
    assert merged[-1]['records']==['Eb_1911.esp:000819','Eb_1911.esp:00081b']
    next_capture=[dict(name='Receiver',records=['Eb_1911.esp:003daa']),
                  dict(name='Material',records=['eb_1911.ESP:00081B'])]
    second=merge_configuration_slots(merged,next_capture)
    assert second[0]['records']==['Eb_1911.esp:003da9','Eb_1911.esp:003daa']
    assert second[1:]==merged[1:]
    assert merge_configuration_slots(second,next_capture)==second
    # Different selector names are not aliases: preserve conflicting roles so
    # the native validator can reject them, instead of silently dropping one.
    conflict=merge_configuration_slots(merged,[dict(name='Other',records=['Eb_1911.esp:003da9'])])
    assert conflict[-1]['records']==conflict[0]['records']
    print('PASS repeated labels, incremental selectors, immutable capture, conflicting roles')


if __name__=='__main__':run()
