import sys,pathlib,json
sys.path.insert(0,'.')
import run, drc_judge as J
R=pathlib.Path('results/m24-keep10m/boards')
for n in sys.argv[1:]:
    ref=run.FIX/n/'unrouted.kicad_pcb'
    src=ref if ref.exists() else R/f'{n}.kicad_pcb'
    h=run.drc(src); m=run.drc(R/f'{n}.placed.kicad_pcb')
    a=J.violations(m,predicate=J.placement_error,courtyard_pairs=True)-J.violations(h,predicate=J.placement_error,courtyard_pairs=True)
    print('==',n,src)
    for k,v in a.items(): print(v,k[0],[i[1][:70] for i in k[2]], k[2] and [i[2] for i in k[2]])
