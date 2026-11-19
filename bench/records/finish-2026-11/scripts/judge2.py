import sys,json,pathlib
ROOT='ROOT/'
sys.path.insert(0,ROOT+'bench')
import run as bench
PICK={'ErgodoneWithHole_ErgoDone':'topfit','Hardware_Playground_sd_wav_pcm5100':'topfit','fifogfx_c64cart':'topfit','kitspace_test-jig':'topfit','LeeChee_1800':'toprv8',
'memsarray_mems_modules':'topmask','P8000_WDC_Emulator_P8000_WDC_Emulator':'toplay2','MicroMaple_MicroMaple':'toplay4','Aleste-520EX_aleste':'toplay4','kitspace_ardfpga':'c60fit',
'Hardware_Playground_serial_gw_ATMEGA328P':'ctl60','kitspace_teensy-fx':'ctl60'}
out={}
for b,arm in PICK.items():
    p=pathlib.Path(ROOT+f'bench/results/ontop-2026-11-19/{b}/{arm}/out.kicad_pcb')
    j=bench.drc(p); u=bench.drc(bench.FIX/b/'unrouted.kicad_pcb')
    if j is None or u is None: out[b]={'arm':arm,'drc':'failed'}; print(b,arm,'drc failed',flush=True); continue
    added={k:v-u['routed_errors'].get(k,0) for k,v in j['routed_errors'].items() if v-u['routed_errors'].get(k,0)>0}
    out[b]=dict(arm=arm,unconnected=j['unconnected'],added=added); print(b,json.dumps(out[b]),flush=True)
json.dump(out,open(ROOT+'bench/results/ontop-2026-11-19/kicad-judge.json','w'),indent=1)
