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
    def via(label, x, y, kind='', layers=('F.Cu','B.Cu'), net=1):
        ITEMS[label] = {'kind':'Via','pos':[x,y]}
        return capture(f'(via {kind} (at {x} {y}) (size 0.8) (drill 0.4) (layers "{layers[0]}" "{layers[1]}") (net {net}) (uuid "{uid(label)}"))', [label])
    def footprint(ref,x,y,padtype='smd',size=(1,1),back=False,courtyard=False,angle=0,pad_at=(0,0),graphics=''):
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
        return capture(f'(footprint "Test:R" (layer "{layer}") (at {x} {y} {angle}) (uuid "{uid(ref)}") (property "Reference" "{ref}" (at 0 -3) (layer "F.SilkS")) {graphics} (pad "1" {padtype} rect (at {pad_at[0]} {pad_at[1]}) (size {size[0]} {size[1]}) {drill} (layers {layers}) (net 1 "X") (uuid "{uid(ref+":pad1")}")))', [ref+':pad1'])
    def board(items):
        return '(kicad_pcb (version 20240108) (generator "pcbnew") (general (thickness 1.6)) (paper "A4") (layers (0 "F.Cu" signal) (1 "In1.Cu" signal) (2 "In2.Cu" signal) (31 "B.Cu" signal) (36 "B.SilkS" user "b.silkscreen") (37 "F.SilkS" user "f.silkscreen") (44 "Edge.Cuts" user) (46 "B.CrtYd" user "b.courtyard") (47 "F.CrtYd" user "f.courtyard")) (setup (pad_to_mask_clearance 0)) (net 0 "") (net 1 "X") (net 2 "Y") (gr_rect (start 0 0) (end 50 40) (stroke (width 0.05) (type solid)) (fill none) (layer "Edge.Cuts") (uuid "'+uid('edge')+'"))\n'+'\n'.join(items)+'\n)\n'
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
