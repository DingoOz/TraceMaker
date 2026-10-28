#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the D80 corpus into a caller-owned build directory; no KiCad needed."""
import json
import math
import uuid
from pathlib import Path


def generate(root: Path) -> dict:
    ROOT = root
    CASES = {}
    IDS = {}
    ITEMS = {}
    FRAGMENTS = {}
    def uid(label):
        value = str(uuid.uuid5(uuid.NAMESPACE_URL, 'tmk-C/' + label))
        IDS[value] = label
        return value

    def capture(fragment, labels):
        # Labels recur across fixtures; save positions with each immutable board fragment.
        FRAGMENTS[fragment] = {label:dict(ITEMS[label]) for label in labels}
        return fragment

    def track(label, x1, y1, x2, y2, width=0.25, layer='F.Cu', net=1):
        ITEMS[label] = {'kind':'Track','pos':[x1,y1],'layer':layer}
        return capture(f'(segment (start {x1} {y1}) (end {x2} {y2}) (width {width}) (layer "{layer}") (net {net}) (uuid "{uid(label)}"))', [label])
    def arc(label, start, mid, end, width=0.25, layer='F.Cu'):
        ax,ay=start; bx,by=mid; cx,cy=end
        denominator=2*(ax*(by-cy)+bx*(cy-ay)+cx*(ay-by))
        aa=ax*ax+ay*ay; bb=bx*bx+by*by; cc=cx*cx+cy*cy
        center=[(aa*(by-cy)+bb*(cy-ay)+cc*(ay-by))/denominator,
                (aa*(cx-bx)+bb*(ax-cx)+cc*(bx-ax))/denominator]
        ITEMS[label] = {'kind':'Arc','pos':center}
        return capture(f'(arc (start {start[0]} {start[1]}) (mid {mid[0]} {mid[1]}) (end {end[0]} {end[1]}) (width {width}) (layer "{layer}") (net 1) (uuid "{uid(label)}"))', [label])
    def via(label, x, y, kind='', layers=('F.Cu','B.Cu'), net=1, extra=''):
        ITEMS[label] = {'kind':'Via','pos':[x,y]}
        return capture(f'(via {kind} (at {x} {y}) (size 0.8) (drill 0.4) (layers "{layers[0]}" "{layers[1]}") {extra} (net {net}) (uuid "{uid(label)}"))', [label])
    def footprint(ref,x,y,padtype='smd',size=(1,1),back=False,courtyard=False,angle=0,pad_at=(0,0),graphics='',pad_extra=''):
        layer = 'B.Cu' if back else 'F.Cu'
        radians = math.radians(angle)
        px = x + pad_at[0]*math.cos(radians) + pad_at[1]*math.sin(radians)
        py = y - pad_at[0]*math.sin(radians) + pad_at[1]*math.cos(radians)
        ITEMS[ref+':pad1'] = {'kind':'Pad','pos':[px,py]}
        layers = '"*.Cu" "*.Mask"' if padtype != 'smd' else f'"{layer}"'
        drill = '(drill 0.4)' if padtype != 'smd' else ''
        if courtyard:
            for side,coords in [('F',( -2,-2,2,2)),('B',(3,-2,7,2))]:
                a,b,c,d=coords
                graphics += f'(fp_rect (start {a} {b}) (end {c} {d}) (stroke (width 0.05) (type solid)) (fill none) (layer "{side}.CrtYd") (uuid "{uid(ref+side+'court')}"))'
        return capture(f'(footprint "Test:R" (layer "{layer}") (at {x} {y} {angle}) (uuid "{uid(ref)}") (property "Reference" "{ref}" (at 0 -3) (layer "F.SilkS")) {graphics} (pad "1" {padtype} rect (at {pad_at[0]} {pad_at[1]}) (size {size[0]} {size[1]}) {drill} (layers {layers}) {pad_extra} (net 1 "X") (uuid "{uid(ref+":pad1")}")))', [ref+':pad1'])
    def board(items):
        return '(kicad_pcb (version 20240108) (generator "pcbnew") (general (thickness 1.6)) (paper "A4") (layers (0 "F.Cu" signal) (1 "In1.Cu" signal) (2 "In2.Cu" signal) (31 "B.Cu" signal) (36 "B.SilkS" user "b.silkscreen") (37 "F.SilkS" user "f.silkscreen") (44 "Edge.Cuts" user) (46 "B.CrtYd" user "b.courtyard") (47 "F.CrtYd" user "f.courtyard")) (setup (pad_to_mask_clearance 0)) (net 0 "") (net 1 "X") (net 2 "Y") (net 3 "D_P") (net 4 "D_N") (net 5 "L_P1") (net 6 "L_N1") (gr_rect (start 0 0) (end 50 40) (stroke (width 0.05) (type solid)) (fill none) (layer "Edge.Cuts") (uuid "'+uid('edge')+'"))\n'+'\n'.join(items)+'\n)\n'
    def rule(condition='',word='track',layer='',severity='',constraint=None,name='C'):
        return f'(rule "{name}" '+ (f'(condition {json.dumps(condition)}) ' if condition else '') + (f'(layer {layer}) ' if layer else '') + (f'(severity {severity}) ' if severity else '') + (constraint or f'(constraint disallow {word})') + ')\n'
    def case(name,items,rules,note='',project=None,types=None):
        p=ROOT/name; p.mkdir(parents=True,exist_ok=True)
        (p/'b.kicad_pcb').write_text(board(items))
        (p/'b.kicad_dru').write_text('(version 1)\n'+rules)
        present = {label:spec for fragment in items for label,spec in FRAGMENTS.get(fragment,{}).items()}
        metadata = {'note':note,'types':types or ['items_not_allowed'],'ids':dict(IDS),'items':present}
        CASES[name] = metadata
        (p/'case.json').write_text(json.dumps(metadata, indent=2) + '\n')
        if project is not None: (p/'b.kicad_pro').write_text(json.dumps(project,indent=2))
        else: (p/'b.kicad_pro').unlink(missing_ok=True)

    tracks=[track('X-front',5,5,8,5),track('Y-front',5,9,8,9,net=2),track('X-inner',5,13,8,13,layer='In1.Cu'),track('X-back',5,17,8,17,layer='B.Cu')]
    vias=[via('X-through',15,5),via('Y-through',15,9,net=2),via('X-blind',15,13,'blind',('F.Cu','In1.Cu')),via('X-buried',15,17,'blind',('In1.Cu','In2.Cu')),via('X-micro',15,21,'micro',('F.Cu','In1.Cu'))]
    static=tracks+vias
    case('static_net_track',static,rule("A.NetName == 'X'"))
    case('static_net_via',static,rule("A.NetName == 'X'",'via'))
    project={'meta':{'filename':'b.kicad_pro','version':1},'net_settings':{'classes':[{'name':'Default','clearance':0.2,'track_width':0.25,'via_diameter':0.8,'via_drill':0.4},{'name':'Special','clearance':0.2,'track_width':0.25,'via_diameter':0.8,'via_drill':0.4}], 'netclass_assignments':{'X':'Special'}, 'netclass_patterns':[{'netclass':'Special','pattern':'X'}]},'board':{'design_settings':{'rules':{'allow_blind_buried_vias':True,'allow_microvias':True}}}}
    case('static_class_track',static,rule("A.NetClass == 'Special'"),project=project)
    case('static_class_via',static,rule("A.NetClass == 'Special'",'via'),project=project)
    # hasExactNetclass (evaluated since D145) and KiCad 10's tuning profiles, which compile into exact-width rules with it (doc 05 §58).
    case('static_exact_class_track',static,rule("A.hasExactNetclass('Special')"),project=project)
    case('static_exact_class_case',static,rule("A.hasExactNetclass('special')"),project=project,
         note='Exact: a different case selects nothing.')
    # hasNetclass (evaluated since D147): the item's effective class contains the name, exactly (doc 05 §59).
    case('static_has_class_track',static,rule("A.hasNetclass('Special')"),project=project)
    case('static_has_class_case',static,rule("A.hasNetclass('special')"),project=project,
         note='Exact: a different case selects nothing.')
    case('static_has_class_default',static,rule("A.hasNetclass('Default')"),project=project,
         note='Special leaves microvia, pair and schematic sizes unset: KiCad 10.0.6 fills them from Default, which then counts as one of its classes, so X is selected too.')
    full=json.loads(json.dumps(project))
    for c in full['net_settings']['classes']:
        c.update({'microvia_diameter':0.3,'microvia_drill':0.1,'diff_pair_width':0.2,'diff_pair_gap':0.25,'wire_width':6,'bus_width':12})
    case('static_has_class_default_full',static,rule("A.hasNetclass('Default')"),project=full,
         note='Special sets every size: its nets are not in Default.')
    def tuned(severity):
        q=json.loads(json.dumps(project))
        q['net_settings']['classes'][1]['tuning_profile']='MS'
        q['board']['design_settings']['rule_severities']={'tuning_profile_track_geometries':severity}
        entry=lambda layer,w: {'signal_layer':layer,'top_reference_layer':'UNDEFINED','bottom_reference_layer':'UNDEFINED','width':w,'diff_pair_gap':0,'delay':0}
        q['tuning_profiles']={'meta':{'version':0},'tuning_profiles_impedance_geometric':[{'profile_name':'MS','type':0,'target_impedance':50.0,
            'enable_time_domain_tuning':False,'via_prop_delay':0,'via_overrides':[],
            'layer_entries':[entry('F.Cu',150000),entry('In1.Cu',100000),entry('In2.Cu',100000),entry('B.Cu',150000)]}]}
        return q
    profiled=[track('tp-front-wide',5,25,8,25),track('tp-front-ok',5,29,8,29,width=0.15),track('tp-inner-wide',12,25,15,25,width=0.15,layer='In1.Cu'),
              track('tp-inner-ok',12,29,15,29,width=0.1,layer='In1.Cu'),track('tp-other-net',20,25,23,25,net=2),track('tp-back-narrow',20,29,23,29,width=0.1,layer='B.Cu')]
    case('tuning_profile_width',profiled,'',project=tuned('error'),types=['track_width'],
         note='Profile MS (0.15 mm outside, 0.1 mm inside) on class Special: its tracks of another width on a layer are track_width errors.')
    case('tuning_profile_ignored',profiled,'',project=tuned('ignore'),types=['track_width'],
         note="With tuning_profile_track_geometries at KiCad's default (ignore) there is no such rule.")
    inherited=tuned('error')
    inherited['net_settings']['classes'][0]['tuning_profile'],inherited['net_settings']['classes'][1]['tuning_profile']='MS',''
    case('tuning_profile_inherited',profiled,'',project=inherited,types=['track_width'],
         note="Profile MS on Default only: Special, which names none, takes Default's (D147), so both nets' tracks are held to it.")
    old_settings=tuned('error')
    old_settings['net_settings']['meta']={'version':4}
    case('tuning_profile_schema4',profiled,'',project=old_settings,types=['track_width'],
         note='Net settings of schema 4: KiCad migrates them and every class loses its tuning profile (D147).')
    # Diff-pair coupling (D147): P and N tracks on parallel lines whose projections overlap are coupled, unless other copper lies
    # between them; each coupling's gap must be within the rule (doc 05 §59). 0.2 mm tracks, so the gaps are 0.4, 0.2, 0.1 and 0.8 mm.
    dp=[track('dp-wide-P',5,30,15,30,width=0.2,net=3),track('dp-wide-N',7,30.6,17,30.6,width=0.2,net=4),
        track('dp-ok-P',20,30,30,30,width=0.2,net=3),track('dp-ok-N',20,30.4,30,30.4,width=0.2,net=4),
        track('dp-close-P',5,35,15,35,width=0.2,net=3),track('dp-close-N',5,35.3,15,35.3,width=0.2,net=4),
        track('dp-shield-P',20,34,30,34,width=0.2,net=3),track('dp-shield-N',20,35,30,35,width=0.2,net=4),track('dp-shield-Y',20,34.5,30,34.5,width=0.2,net=2),
        track('dp-apart-P',35,30,40,30,width=0.2,net=3),track('dp-apart-N',41,30.6,46,30.6,width=0.2,net=4),
        track('dp-bend-P',35,35,40,35,width=0.2,net=3),track('dp-bend-N',35,35.6,40,37,width=0.2,net=4)]
    gap_rule=rule("A.inDiffPair('*')",constraint='(constraint diff_pair_gap (min 0.15mm) (max 0.25mm))',name='dp_gap')
    case('diff_pair_gap',dp,gap_rule,types=['diff_pair_gap_out_of_range'],
         note='0.4 mm too wide, 0.1 mm too narrow; 0.2 mm fine; a Y track between two halves hides them; no overlap, or not parallel: no coupling.')
    case('diff_pair_gap_ignored',dp,rule("A.inDiffPair('*')",severity='ignore',constraint='(constraint diff_pair_gap (min 0.15mm) (max 0.25mm))'),
         types=['diff_pair_gap_out_of_range'],note='A rule at severity ignore checks nothing.')
    case('diff_pair_uncoupled',dp,gap_rule+rule("A.inDiffPair('*')",constraint='(constraint diff_pair_uncoupled (max 5mm))',name='dp_unc'),
         types=['diff_pair_gap_out_of_range','diff_pair_uncoupled_length_too_long'],
         note='With a maximum uncoupled length too: KiCad keys the items twice, so the gap errors stay, and the 10 mm coupled at 0.2 mm leave most of the length uncoupled.')
    # Pair names as MatchDpSuffix reads them: digits and underscores after the P or N are skipped, so L_P1 pairs with L_N1 on base
    # "L_", and inDiffPair('L') matches a base ending in an underscore without it. Both pairs are 0.4 mm apart.
    suffixed=[track('dps-L-P',5,10,15,10,width=0.2,net=5),track('dps-L-N',5,10.6,15,10.6,width=0.2,net=6),
              track('dps-D-P',20,10,30,10,width=0.2,net=3),track('dps-D-N',20,10.6,30,10.6,width=0.2,net=4)]
    case('diff_pair_suffix',suffixed,rule("A.inDiffPair('L')",constraint='(constraint diff_pair_gap (max 0.25mm))',name='dp_gap'),
         types=['diff_pair_gap_out_of_range'],note='L_P1/L_N1 are a pair whose base L_ matches L; D_P/D_N (base D_) does not.')
    case('diff_pair_suffix_base',suffixed,rule("A.inDiffPair('D_')",constraint='(constraint diff_pair_gap (max 0.25mm))',name='dp_gap'),
         types=['diff_pair_gap_out_of_range'],note="The base itself matches: D_ for D_P/D_N; L_ is not D_.")
    # Tracks of a tuning pattern (a generator group) are left out of the coupling test and of the uncoupled lengths (KiCad's
    # isInTuningPattern). Both pairs are 0.4 mm apart; the L pair's tracks are members of a tuning pattern.
    tuned=[track('dpt-D-P',5,10,15,10,width=0.2,net=3),track('dpt-D-N',5,10.6,15,10.6,width=0.2,net=4),
           track('dpt-L-P',20,10,30,10,width=0.2,net=5),track('dpt-L-N',20,10.6,30,10.6,width=0.2,net=6),
           f'(generated (uuid "{uid("tuning")}") (type tuning_pattern) (name "Tuning Pattern") (layer "F.Cu") (members "{uid("dpt-L-P")}" "{uid("dpt-L-N")}"))']
    case('diff_pair_tuning_pattern',tuned,rule("A.inDiffPair('*')",constraint='(constraint diff_pair_gap (max 0.25mm))',name='dp_gap')+
         rule("A.inDiffPair('*')",constraint='(constraint diff_pair_uncoupled (max 5mm))',name='dp_unc'),
         types=['diff_pair_gap_out_of_range','diff_pair_uncoupled_length_too_long'],note='Only the D pair, whose tracks are not in the tuning pattern, is judged.')
    # Nearly parallel tracks are coupled when their lines cross beyond 32-bit coordinates (SEG::Intersect returns nothing there):
    # 0.4 mm apart, N 10 nm off parallel over 10 mm (crossing about 400 m away), and N 0.1 mm off (crossing within the board).
    skewed=[track('dpn-D-P',5,10,15,10,width=0.2,net=3),track('dpn-D-N',5,10.6,15,10.60001,width=0.2,net=4),
            track('dpn-L-P',20,10,30,10,width=0.2,net=5),track('dpn-L-N',20,10.6,30,10.7,width=0.2,net=6)]
    case('diff_pair_near_parallel',skewed,rule("A.inDiffPair('*')",constraint='(constraint diff_pair_gap (max 0.25mm))',name='dp_gap'),
         types=['diff_pair_gap_out_of_range'],note='Only the D pair, 10 nm off parallel, is coupled.')
    # The order KiCad resolves a clearance in (DRC_ENGINE::EvalRules): a pad's local clearance first, floored at the board minimum;
    # then the custom rules, whose value is final even below the board minimum; only then the net class, floored. Holes alike: a
    # custom hole_clearance or hole_to_hole replaces the board's.
    def minimums(**rules):
        p = json.loads(json.dumps(project))
        p['board']['design_settings']['rules'].update(rules)
        return p
    clr_box='(zone (net 0) (net_name "") (layer "F.Cu") (uuid "'+uid('clr-box')+'") (name "box") (hatch edge 0.5) (connect_pads (clearance 0)) (min_thickness 0.25) (keepout (tracks allowed) (vias allowed) (pads allowed) (copperpour allowed) (footprints allowed)) (fill (thermal_gap 0.5) (thermal_bridge_width 0.5)) (polygon (pts (xy 10 5) (xy 20 5) (xy 20 15) (xy 10 15))))'
    below=[clr_box,track('cb-in-X',12,7,18,7,width=0.2,net=1),track('cb-in-Y',12,7.3,18,7.3,width=0.2,net=2),
           track('cb-out-X',25,7,30,7,width=0.2,net=1),track('cb-out-Y',25,7.3,30,7.3,width=0.2,net=2)]
    case('clearance_custom_below_minimum',below,rule("A.intersectsArea('box')",constraint='(constraint clearance (min 0.1mm))',name='clr'),
         project=minimums(min_clearance=0.125),types=['clearance'],note='0.1 mm gaps under a 0.125 mm board minimum: the rule in the box allows them.')
    local=[footprint('LC',10,20,pad_extra='(clearance 0.3)'),track('lc-Y',8,20.8,12,20.8,width=0.2,net=2)]
    case('clearance_local_over_custom',local,rule(constraint='(constraint clearance (min 0.1mm))',name='clr'),
         project=project,types=['clearance'],note="A 0.2 mm gap to a pad with a 0.3 mm local clearance; the custom 0.1 mm rule does not override the pad's.")
    holes=[footprint('H1',10,25,padtype='np_thru_hole',size=(0.4,0.4)),track('h1-Y',8,25.45,12,25.45,width=0.2,net=2),
           footprint('H2',20,25,padtype='np_thru_hole',size=(0.4,0.4)),footprint('H3',20.7,25,padtype='np_thru_hole',size=(0.4,0.4))]
    case('hole_rules_custom_below_minimum',holes,rule(constraint='(constraint hole_clearance (min 0.1mm)) (constraint hole_to_hole (min 0.2mm))',name='holes'),
         project=minimums(min_hole_clearance=0.3,min_hole_to_hole=0.5),types=['hole_clearance','hole_to_hole'],
         note='A track 0.15 mm from a hole, and holes 0.3 mm apart, under 0.3/0.5 mm board minimums and 0.1/0.2 mm rules.')
    case('hole_rules_minimum',holes,'',project=minimums(min_hole_clearance=0.3,min_hole_to_hole=0.5),types=['hole_clearance','hole_to_hole'],
         note='The same without the rule: the board minimums apply.')
    # Copper graphics: two touching shapes of one footprint are never tested (KiCad treats them as one compound shape); two touching
    # net-less board graphics are.
    fp_copper=''.join(f'(fp_line (start {a} {b}) (end {c} {d}) (stroke (width 0.1) (type solid)) (layer "F.Cu") (uuid "{uid("gfp"+str(k))}"))'
                      for k,(a,b,c,d) in enumerate([(-2,-2,2,-2),(2,-2,2,2)]))
    def gr_line(label,x1,y1,x2,y2):
        ITEMS[label]={'kind':'Copper','pos':[x1,y1]}
        return capture(f'(gr_line (start {x1} {y1}) (end {x2} {y2}) (stroke (width 0.1) (type solid)) (layer "F.Cu") (uuid "{uid(label)}"))',[label])
    fp_copper2=f'(fp_line (start -2 -2) (end -2 2) (stroke (width 0.1) (type solid)) (layer "F.Cu") (uuid "{uid("gfp2")}"))'
    graphics=[footprint('G1',10,30,graphics=fp_copper),footprint('G2',14,30,graphics=fp_copper2),
              gr_line('gr-a',30,30,34,30),gr_line('gr-b',34,30,34,34)]
    case('copper_graphics_compound',graphics,'',types=['shorting_items','clearance'],
         note="G1's two copper lines meet at a corner, and G2's line lies on G1's right-hand one; two net-less board lines meet.")
    # Vias that remove unconnected copper (D150): a Y track 0.7 mm from an X via's centre is inside the ring's clearance but clear of
    # the hole. KiCad keeps the ring on F.Cu (keep_end_layers), on In1.Cu where an X track reaches the hole, and on In2.Cu where the
    # file says a zone joins it; elsewhere only the hole is there. keep_end_layers no removes the end layers too.
    keep='(remove_unused_layers yes) (keep_end_layers yes)'
    unused=[via('vu-a',10,30,extra=keep),track('vu-a-inner',8,30.7,12,30.7,layer='In1.Cu',net=2),track('vu-a-front',8,29.3,12,29.3,net=2),
            via('vu-b',20,30,extra=keep),track('vu-b-feed',20,30,23,30,layer='In1.Cu'),track('vu-b-inner',18,30.7,22,30.7,layer='In1.Cu',net=2),
            via('vu-c',30,30,extra=keep+' (zone_layer_connections "In2.Cu")'),track('vu-c-inner',28,30.7,32,30.7,layer='In2.Cu',net=2),
            track('vu-c-other',28,29.3,32,29.3,layer='In1.Cu',net=2),
            via('vu-d',40,30,extra='(remove_unused_layers yes) (keep_end_layers no)'),track('vu-d-front',38,30.7,42,30.7,net=2)]
    case('via_unused_layers',unused,'',types=['clearance','shorting_items','hole_clearance'],
         note='Only vu-a-front, vu-b-inner and vu-c-inner are near copper: the other tracks pass rings KiCad removed.')
    case('static_layer_track',static,rule(layer='"F.Cu"'))
    case('static_inner_track',static,rule(layer='inner'))
    case('static_inner_via',static,rule(word='via',layer='inner'))
    case('static_type_track',static,rule("A.Type == 'Track'",'track via'))
    case('static_type_via',static,rule("A.Type == 'Via'",'track via'))
    for kind in ['through_via','blind_via','buried_via','micro_via']:
        case('subtype_'+kind,static,rule(word=kind),project=project)
    area='(zone (net 0) (net_name "") (layer "F.Cu") (uuid "'+uid('area')+'") (name "box") (hatch edge 0.5) (connect_pads (clearance 0)) (min_thickness 0.25) (keepout (tracks allowed) (vias allowed) (pads allowed) (copperpour not_allowed) (footprints allowed)) (fill (thermal_gap 0.5) (thermal_bridge_width 0.5)) (polygon (pts (xy 10 5) (xy 20 5) (xy 20 15) (xy 10 15))))'
    area_items=[area,track('inside',12,7,18,7),track('crossing',7,10,15,10),track('outside',25,10,28,10),track('back-inside',12,13,18,13,layer='B.Cu')]
    for func in ['insideArea','intersectsArea','enclosedByArea']:
        case('area_'+func,area_items,rule(f"A.{func}('box')"),note='Named F.Cu rule area: inside, boundary-crossing, outside, and B.Cu geometric-inside tracks.')
    court_items=[footprint('R1',20,10,courtyard=True),track('front-court',18,11,21,11),track('back-court',23,11,26,11,layer='B.Cu'),track('front-in-back-court',23,9,26,9),track('outside-court',35,10,38,10)]
    for func in ['intersectsCourtyard','intersectsFrontCourtyard','intersectsBackCourtyard']:
        case('court_'+func,court_items,rule(f"A.{func}('R1')"))
    # Front and back are the footprint's own sides: for a flipped footprint, front is B.CrtYd (x 23..27).
    flipped_items=[footprint('R1',20,10,back=True,courtyard=True),track('front-court',18,11,21,11),track('back-court',23,11,26,11,layer='B.Cu'),track('front-in-back-court',23,9,26,9),track('outside-court',35,10,38,10)]
    for func in ['intersectsCourtyard','intersectsFrontCourtyard','intersectsBackCourtyard']:
        case('court_flipped_'+func,flipped_items,rule(f"A.{func}('R1')"),note='Flipped R1: F.CrtYd at x 18..22, B.CrtYd at x 23..27.')
    case('court_lib_id',court_items,rule("A.intersectsCourtyard('Test:*')"),note='A selector with a colon matches the footprint library id.')
    # A second F.CrtYd rectangle beside the first (x 18..22): KiCad merges overlapping and touching outlines.
    def court_rect(ref,a,b,c,d):
        return f'(fp_rect (start {a} {b}) (end {c} {d}) (stroke (width 0.05) (type solid)) (fill none) (layer "F.CrtYd") (uuid "{uid(ref+"extra-court")}"))'
    for shape,rect in [('overlap',(1,-2,5,2)),('touch',(2,-2,5,2)),('disjoint',(2.5,-2,5,2))]:
        items=[footprint('R1',20,10,courtyard=True,graphics=court_rect('R1',*rect))]+court_items[1:]
        for func in ['intersectsCourtyard','intersectsFrontCourtyard']:
            case(f'court_{shape}_{func}',items,rule(f"A.{func}('R1')"),
                 note=f'Two F.CrtYd rectangles ({shape}): x 18..22 and x {20+rect[0]}..25; B.CrtYd at x 23..27.')
    pads=[footprint('R1',10,5),footprint('R2',10,10,size=(2,0.5)),footprint('C1',10,15,'thru_hole'),footprint('H1',10,20,'np_thru_hole'),track('free-track',20,5,23,5),via('free-via',20,10)]
    for selector in ['R*','R1']:
        case('membership_'+selector.replace('*','wildcard'),pads,rule(f"A.memberOfFootprint('{selector}')",'pad track'))
    case('membership_lib_id',pads,rule("A.memberOfFootprint('Test:R')",'pad track'))
    for prop in ['A.Reference','A.Parent.Reference','Parent.Reference','B.Reference']:
        case('reference_'+prop.replace('.','_'),pads,rule(f"{prop} == 'R1'",'pad track'))
    case('pad_type',pads,rule("A.Pad_Type == 'SMD'",'pad'))
    for name,condition in [('size_x_quoted',"A.Size_X == '1mm'"),('size_x_mm','A.Size_X == 1mm'),('size_y_half_mm','A.Size_Y == 0.5mm'),('size_x_mil','A.Size_X > 40mil'),('position_x','A.Position_X > 15mm')]:
        case(name,pads,rule(condition,'pad track via'))
    width_items=[track('thin',5,5,8,5,width=0.2),track('quarter',5,9,8,9,width=0.25),track('wide',5,13,8,13,width=0.4)]
    for name,condition in [('width_gt_mm','A.Width > 0.3mm'),('width_eq_mm','A.Width == 0.25mm'),('width_eq_mil','A.Width == 9.84251968503937mil'),('width_gt_double','A.Width > 0.25'),('width_eq_double','A.Width == 0.25'),('fractional_nm','A.Width == 0.0000001mm'),('fractional_nm_gt','A.Width > 0.0000001mm')]:
        case(name,width_items,rule(condition))
    case('plated',pads,rule('A.isPlated()','pad via'))
    case('exists_on_front',static,rule("A.existsOnLayer('F.Cu')",'track via'))
    case('item_layer_front',static,rule("A.Layer == 'F.Cu'",'track via'))
    case('item_layer_back',static,rule("A.Layer == 'B.Cu'",'track via'))
    case('bare_layer_front',static,rule("L == 'F.Cu'",'track via'))
    for name,condition in [('malformed_quote',"A.NetName == 'X"),('malformed_paren',"(A.NetName == 'X'"),('unknown_property','A.Foo == 1'),('unknown_function','A.notAFunction()'),('unknown_short_circuit',"A.NetName == 'X' || A.Foo == 1")]:
        case(name,tracks,rule(condition),note='Negative-language probe: KiCad rejection/ignored rule is a finding, not a successful positive test.')
    case('severity_ignore',tracks,rule(severity='ignore'))
    case('later_ignore',tracks,rule(name='ban')+rule("A.NetName == 'X'",severity='ignore',name='exception'))
    case('earlier_ignore',tracks,rule("A.NetName == 'X'",severity='ignore',name='exception')+rule(name='ban'))
    phc=[footprint('R1',10,5),via('via-in-SMD',10,5),footprint('C1',20,5,'thru_hole'),via('via-in-PTH',20,5),footprint('R2',30,5),via('via-outside',35,5)]
    case('physical_hole_smd',phc,rule("A.Type == 'Via' && B.Pad_Type == 'SMD'",constraint='(constraint physical_hole_clearance (min 0.05mm))'),types=['hole_clearance'])
    case('physical_hole_b_reference',phc,rule("A.Type == 'Via' && B.Pad_Type == 'SMD' && B.Reference == 'R1'",constraint='(constraint physical_hole_clearance (min 0.05mm))'),types=['hole_clearance'])
    case('custom_track_width',width_items,rule("A.NetName == 'X'",constraint='(constraint track_width (min 0.3mm))'),types=['track_width'])
    case('custom_clearance',[track('near-A',5,5,8,5),track('near-B',5,5.6,8,5.6,net=2),track('far-B',20,10,23,10,net=2)],rule("A.NetName == 'X' && B.NetName == 'Y'",constraint='(constraint clearance (min 0.8mm))'),types=['clearance'])
    for name,condition in [('width_gt_bare_zero','A.Width > 0'),('width_gt_bare_integer','A.Width > 250000'),('width_eq_bare_integer','A.Width == 250000'),('fractional_nm_constant','0.0000001mm > 0mm'),('fractional_nm_not_zero','0.0000001mm != 0mm'),('literal_mm_mil','1mm == 39.37007874015748mil')]:
        case(name,width_items,rule(condition))
    # Decisive language probes: valid OR controls distinguish invalid conditions from false branches.
    for name,condition in [
        ('width_ne_double','A.Width != 0.25'),
        ('width_gt_double_or',"A.Width > 0.25 || A.NetName == 'X'"),
        ('width_ne_double_or',"A.Width != 0.25 || A.NetName == 'X'"),
        ('width_eq_double_or',"A.Width == 0.25 || A.NetName == 'X'"),
        ('width_gt_double_multiliteral_true_or','A.Width > 0.25 || 1mm == 1mm'),
        ('width_gt_double_multiliteral_false_or','A.Width > 0.25 || 1mm == 0mm'),
        ('width_eq_integer_multiliteral_false_or','A.Width == 250000 || 1mm == 0mm'),
        ('width_eq_double_multiliteral_false_or','A.Width == 0.25 || 1mm == 0mm'),
        ('width_ne_double_multiliteral_false_or','A.Width != 0.25 || 1mm == 0mm'),
        ('width_gt_in','A.Width > 0.01in'),
        ('width_gt_um','A.Width > 250um'),
        ('width_gt_cm','A.Width > 0.025cm'),
        ('width_gt_mils','A.Width > 10mils'),
        ('width_gt_um_or',"A.Width > 250um || A.NetName == 'X'"),
        ('width_gt_cm_or',"A.Width > 0.025cm || A.NetName == 'X'"),
        ('width_gt_mils_or',"A.Width > 10mils || A.NetName == 'X'"),
        ('width_eq_spaced_mm','A.Width == 0.25 mm'),
        ('width_gt_spaced_in','A.Width > 0.01 in'),
        ('width_eq_upper_mm_or',"A.Width == 0.25MM || A.NetName == 'X'"),
        ('width_gt_upper_in_or',"A.Width > 0.01IN || A.NetName == 'X'"),
        ('width_eq_inch_or',"A.Width == 0.00984251968503937inch || A.NetName == 'X'"),
        ('width_eq_thou_or',"A.Width == 9.84251968503937thou || A.NetName == 'X'"),
        ('width_eq_deg','A.Width == 250000deg'),
        ('width_eq_fs','A.Width == 250000fs'),
        ('width_eq_ps','A.Width == 250ps'),
        ('width_gt_zero_fs','A.Width > 0fs'),
        ('width_gt_zero_ps','A.Width > 0ps'),
        ('literal_ps_fs','1ps == 1000fs'),
        ('width_eq_250fs','A.Width == 250fs'),
        ('width_eq_quarter_ps','A.Width == 0.25ps'),
        ('width_eq_quoted_integer',"A.Width == '250000'"),
        ('width_eq_quoted_mm',"A.Width == '0.25mm'"),
        ('width_ne_quoted_mm',"A.Width != '0.25mm'"),
        ('width_gt_quoted_mm',"A.Width > '0.25mm'"),
        ('width_eq_quoted_mm_or',"A.Width == '0.25mm' || A.NetName == 'X'"),
    ]:
        case(name,width_items,rule(condition),note='Dimensional expression probe; OR branches are deliberate true/false controls as written.')
    for name,condition in [
        ('size_x_quoted_integer',"A.Size_X == '1000000'"),
        ('size_x_quoted_quarter',"A.Size_X == '0.25mm'"),
        ('size_x_ne_quoted_mm',"A.Size_X != '1mm'"),
        ('size_x_quoted_or',"A.Size_X == '1mm' || A.Type == 'Pad'"),
    ]:
        case(name,pads,rule(condition,'pad'),note='Quoted dimensional comparison with SMD, PTH and NPTH pads.')
    case('later_different_disallow',static,rule(name='ban-track')+rule(word='via',name='ban-via'),
         note='Both rules match every item; their disallow item types differ.')
    case('later_different_disallow_ignore',static,rule(name='ban-track')+rule(word='via',severity='ignore',name='ignore-via'),
         note='Later matching ignore disallow has a different item type.')
    case('later_different_disallow_condition',static,rule(name='ban-track')+rule("A.NetName == 'X'",word='via',name='ban-X-via'),
         note='Later rule matches X only and has a different item type.')
    # KiCad's || binds tighter than &&, and ! tighter than the comparisons (measured with KiCad 10.0.6).
    for name,condition in [
        ('precedence_and_or',"A.NetName == 'Y' && A.Layer == 'B.Cu' || A.NetName == 'X'"),
        ('precedence_or_and',"A.NetName == 'X' || A.NetName == 'Y' && A.Layer == 'B.Cu'"),
        ('precedence_or_and_parenthesized',"A.NetName == 'X' || (A.NetName == 'Y' && A.Layer == 'B.Cu')"),
        ('precedence_mixed',"A.NetName == 'Y' || A.NetName == 'X' && A.Layer == 'B.Cu' || A.Layer == 'In1.Cu'"),
        ('precedence_not_eq',"!A.NetName == 'X'"),
        ('precedence_not_ne',"!A.NetName != 'X'"),
        ('precedence_not_parenthesized',"!(A.NetName == 'X')"),
        ('precedence_not_call',"!A.existsOnLayer('F.Cu') && A.NetName == 'X'"),
    ]:
        case(name,static,rule(condition),note='Operator precedence probe: X tracks on F.Cu, In1.Cu and B.Cu, one Y track on F.Cu.')
    # Width exists on tracks and arcs only: undefined (not 0, not the via diameter) for pads and vias.
    for name,condition in [
        ('width_non_track_eq_zero','A.Width == 0mm'),
        ('width_non_track_ne','A.Width != 0.25mm'),
        ('width_non_track_lt','A.Width < 0.3mm'),
        ('width_non_track_eq_diameter','A.Width == 0.8mm'),
        ('width_non_track_gt','A.Width > 0.5mm'),
    ]:
        case(name,pads,rule(condition,'pad track via'),note='Pads, a 0.25 mm track and a 0.8 mm via: Width is a track property.')
    # Numbers: equality is exact in double precision (1.001 * 1e6 and 1.1 * 25400 are not whole nanometres),
    # and a literal is sign, digits and an optional fraction.
    number_items=[track('inexact-mm',5,5,8,5,width=1.001),track('inexact-mil',5,9,8,9,width=0.02794),track('plain',5,13,8,13,width=0.4)]
    for name,condition in [
        ('number_inexact_eq_mm','A.Width == 1.001mm'),
        ('number_inexact_ne_mm','A.Width != 1.001mm'),
        ('number_inexact_gt_mm','A.Width > 1.001mm'),
        ('number_inexact_eq_mil','A.Width == 1.1mil'),
        ('number_inexact_ne_mil','A.Width != 1.1mil'),
        ('number_inexact_ge_mil','A.Width >= 1.1mil'),
        ('number_leading_dot','A.Width > .3mm'),
        ('number_exponent','A.Width > 3e-1mm'),
        ('number_plus_sign','A.Width > +0.3mm'),
        ('number_negative','A.Width > -1mm'),
        ('number_trailing_dot','A.Width > 0.mm'),
    ]:
        case(name,number_items,rule(condition),note='Tracks of 1.001 mm, 0.02794 mm (1.1 mil) and 0.4 mm.')
    # Functions KiCad compiles and TraceMaker does not evaluate: the rule is kept and the X branch still decides.
    # The last two are unknown to KiCad as well, and drop the rule.
    for function,arguments in [
        ('hasNetclass',"'a'"),('hasExactNetclass',"'a'"),('hasComponentClass',"'a'"),('fromTo',"'a','b'"),
        ('isMicroVia',''),('isBlindBuriedVia',''),('isBlindVia',''),('isBuriedVia',''),('isCoupledDiffPair',''),
        ('getField',"'a'"),('memberOf',"'a'"),('memberOfGroup',"'a'"),('memberOfSheet',"'a'"),
        ('memberOfSheetOrChildren',"'a'"),('insideCourtyard',"'a'"),('insideFrontCourtyard',"'a'"),
        ('insideBackCourtyard',"'a'"),('hasExactComponentClass',"'a'"),('isThroughVia',''),
    ]:
        case('kicad_function_'+function,static,rule(f"A.NetName == 'X' || A.{function}({arguments})"),
             note='A rule KiCad applies selects the three X tracks; a rule it cannot compile selects nothing.')
    case('kicad_function_clearance',[track('near-A',5,5,8,5),track('near-B',5,5.6,8,5.6,net=2),track('far-B',20,10,23,10,net=2)],
         rule("(A.NetName == 'X' && B.NetName == 'Y') || A.hasNetclass('a')",constraint='(constraint clearance (min 0.8mm))'),types=['clearance'])
    layer_pads=[footprint('SF',5,5),footprint('SB',5,10,back=True),footprint('PF',5,15,'thru_hole'),
                footprint('PB',5,20,'thru_hole',back=True),footprint('NF',5,25,'np_thru_hole')]+vias
    for name,condition in [
        ('own_layer_pads_front',"A.Layer == 'F.Cu'"),
        ('own_layer_pads_back',"A.Layer == 'B.Cu'"),
        ('own_layer_pads_not_front',"A.Layer != 'F.Cu'"),
        ('own_layer_pads_not_back',"A.Layer != 'B.Cu'"),
    ]:
        case(name,layer_pads,rule(condition,'pad via'),note='SMD, PTH, NPTH and through/blind/micro via own Layer equality/inequality.')
    position_items=[track('position-track',20,5,26,9),arc('position-arc',(20,15),(23,12),(26,15)),
                    footprint('PS',7,22,pad_at=(3,2)),footprint('PR',17,22,angle=90,pad_at=(3,2)),
                    via('position-via',30,30)]
    for name,condition in [
        ('position_anchor_x_20','A.Position_X == 20mm'),
        ('position_anchor_y_5','A.Position_Y == 5mm'),
        ('position_anchor_x_zero','A.Position_X == 0mm'),
        ('position_anchor_y_zero','A.Position_Y == 0mm'),
        ('position_anchor_x_gt_15','A.Position_X > 15mm'),
        ('position_anchor_x_ne_zero','A.Position_X != 0mm'),
        ('position_anchor_x_le_zero','A.Position_X <= 0mm'),
        ('position_anchor_x_lt_one','A.Position_X < 1mm'),
        ('position_anchor_shifted_pad',"A.Position_X == 10mm && A.Position_Y == 24mm"),
        ('position_anchor_rotated_pad',"A.Position_X == 19mm && A.Position_Y == 19mm"),
        ('position_anchor_footprint_origin',"A.Position_X == 17mm && A.Position_Y == 22mm"),
        ('position_anchor_or',"A.Position_X > 15mm || A.Type == 'Track'"),
    ]:
        case(name,position_items,rule(condition,'pad track via'),note='Track start (20,5), arc start (20,15), shifted pad global (10,24), rotated pad global (19,19), via (30,30).')
    def court_graphics(ref,segments=(),arcs=()):
        text=''
        for index,(start,end) in enumerate(segments):
            text += f'(fp_line (start {start[0]} {start[1]}) (end {end[0]} {end[1]}) (stroke (width 0.05) (type solid)) (layer "F.CrtYd") (uuid "{uid(ref+"line"+str(index))}"))'
        for index,(start,mid,end) in enumerate(arcs):
            text += f'(fp_arc (start {start[0]} {start[1]}) (mid {mid[0]} {mid[1]}) (end {end[0]} {end[1]}) (stroke (width 0.05) (type solid)) (layer "F.CrtYd") (uuid "{uid(ref+"arc"+str(index))}"))'
        return text
    square=[((-3,-3),(3,-3)),((3,-3),(3,3)),((3,3),(-3,3)),((-3,3),(-3,-3))]
    for shape,graphics in [
        ('line',court_graphics('CL',square)),
        ('unclosed',court_graphics('CL',square[:-1])),
        ('arc',court_graphics('CL',arcs=[((-3,0),(0,-3),(3,0)),((3,0),(0,3),(-3,0))])),
    ]:
        items=[footprint('CL',20,20,graphics=graphics),track('court-shape-inside',19,20,21,20),
               track('court-shape-crossing',15,21,19,21),track('court-shape-outside',28,20,30,20),
               track('court-shape-corner',22.5,22.5,22.7,22.7)]
        for func in ['intersectsCourtyard','intersectsFrontCourtyard']:
            case(f'court_{shape}_{func}',items,rule(f"A.{func}('CL')"),
                 note=f'{shape} front courtyard; corner lies in bounding square but outside circular courtyard.')
    def rule_area(label,contours):
        polygons=''.join('(polygon (pts '+''.join(f'(xy {x} {y})' for x,y in points)+'))' for points in contours)
        return f'(zone (net 0) (net_name "") (layer "F.Cu") (uuid "{uid(label)}") (name "{label}") (hatch edge 0.5) (connect_pads (clearance 0)) (min_thickness 0.25) (keepout (tracks allowed) (vias allowed) (pads allowed) (copperpour not_allowed) (footprints allowed)) (fill (thermal_gap 0.5) (thermal_bridge_width 0.5)) {polygons})'
    concave=[(10,5),(25,5),(25,10),(15,10),(15,20),(10,20)]
    # The slit bridge represents a polygon hole in one contour without relying on polygon ordering.
    hole=[(10,5),(25,5),(25,20),(10,20),(10,5),(14,9),(14,16),(21,16),(21,9),(14,9),(10,5)]
    for shape,points in [('concave',concave),('hole',hole)]:
        items=[rule_area(shape,[points]),track('area-shape-solid',11,7,13,7),
               track('area-shape-void',17,12,19,12),track('area-shape-crossing',12,13,18,13),
               track('area-shape-outside',30,15,33,15),track('area-shape-back',11,8,13,8,layer='B.Cu')]
        for func in ['insideArea','intersectsArea','enclosedByArea']:
            case(f'area_{shape}_{func}',items,rule(f"A.{func}('{shape}')"),
                 note=f'{shape} F.Cu area; solid, void/notch, crossing, outside and back controls.')

    def filled_zone(label, layers):
        # Keep one immutable metadata label for the original zone across its fill layers.
        # TraceMaker reports the outline start; KiCad reports the original zone UUID.
        points = [(20,3),(24,3),(24,32),(20,32)]
        polygon = '(pts '+''.join(f'(xy {x} {y})' for x,y in points)+')'
        ITEMS[label] = {'kind':'Zone','pos':list(points[0])}
        layer_clause = f'(layer "{layers[0]}")' if len(layers) == 1 else '(layers '+ ' '.join(json.dumps(layer) for layer in layers)+')'
        fills = ''.join(f'(filled_polygon (layer "{layer}") {polygon})' for layer in layers)
        return capture(f'(zone (net 2) (net_name "Y") {layer_clause} (uuid "{uid(label)}") (name "{label}") (hatch edge 0.5) (connect_pads (clearance 0)) (min_thickness 0.01) (fill yes (thermal_gap 0.5) (thermal_bridge_width 0.5)) (polygon {polygon}) {fills})', [label])
    zone_neighbors = [
        track('zone-front-track',18,5,19.35,5),
        track('zone-inner-track',18,8,19.35,8,layer='In1.Cu'),
        track('zone-back-track',18,11,19.35,11,layer='B.Cu'),
        footprint('ZSF',19,14), footprint('ZSB',19,17,back=True),
        footprint('ZPF',19,20,'thru_hole'), footprint('ZPB',19,23,'thru_hole',back=True),
        via('zone-through-via',19.1,26),
    ]
    for shape,layers in [('front',('F.Cu',)),('inner',('In1.Cu',)),('multi',('F.Cu','In1.Cu'))]:
        zone_items = zone_neighbors + [filled_zone('zone-'+shape, layers)]
        selectors = [
            ('unconditional',''),
            ('zone_type',"B.Type == 'Zone'"),
            ('same_layer',"B.Type == 'Zone' && A.Layer == B.Layer"),
            ('zone_front',"B.Type == 'Zone' && B.Layer == 'F.Cu'"),
            ('zone_not_front',"B.Type == 'Zone' && B.Layer != 'F.Cu'"),
            ('different_layer',"B.Type == 'Zone' && A.Layer != B.Layer"),
            ('item_front',"B.Type == 'Zone' && A.Layer == 'F.Cu'"),
            ('item_back',"B.Type == 'Zone' && A.Layer == 'B.Cu'"),
            ('exists_front',"B.Type == 'Zone' && A.existsOnLayer('F.Cu')"),
        ]
        if shape == 'multi':
            selectors += [
                ('zone_empty',"B.Type == 'Zone' && B.Layer == ''"),
                ('zone_undefined',"B.Type == 'Zone' && B.Layer == 'undefined'"),
                ('zone_wildcard',"B.Type == 'Zone' && B.Layer == '*'"),
            ]
        for selector,condition in selectors:
            case(f'zone_pair_{shape}_{selector}',zone_items,
                 rule(condition,constraint='(constraint clearance (min 0.8mm))'),
                 note=f'Cached explicit {"/".join(layers)} fill, no refill. Neighbor gaps 0.5–0.525 mm exceed default clearance; 0.8 mm custom clearance detects positive controls. PTH footprints on both sides and through via distinguish own Layer from checked copper layer. One zone label/UUID is shared across multilayer fills.',
                 types=['clearance'])

    return dict(sorted(CASES.items()))


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("work", type=Path)
    arguments = parser.parse_args()
    print(f"Generated {len(generate(arguments.work))} cases")
