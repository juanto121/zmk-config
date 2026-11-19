#!/usr/bin/env python3
"""Parametric Barad-dur-inspired dongle enclosure. All dimensions in mm.

X = left/right, Y = front(-)/rear(+), Z = desk/up.
The base sits flat on the desk. Everything above it (shaft, head, crown, display,
rear cover) is modelled upright in a local frame, then leaned back by LEAN_DEG
about the rear cover's bottom inner edge so the screen faces slightly upward.
STLs are individually placed on the print bed; STEP preserves assembly coordinates.
See README.md for sources, assumed hardware, print settings and assembly.
"""
from pathlib import Path
import json
import math

import cadquery as cq
import trimesh

OUT = Path(__file__).resolve().parent

# Measure your actual clone before changing these. No carrier PCB.
MCU_W = 18.0               # manufacturer: 17.78; rounded up for clone variation
MCU_L = 33.0
MCU_PCB_T = 1.0            # assembly assumption, not specified in outline drawing
MCU_BOTTOM = 6.4
MCU_SIDE_GAP = 0.40
MCU_END_GAP = 0.60
STOP_T = 1.2               # tray end stops
WALL = 2.0

# Overall envelope. Width is the widest point seen from the front, ornaments
# included; height is the crown tip above the desk, tray included. Depth is the
# larger of what the controller needs and what keeps the leaned tower over the base.
TOWER_W = 30.0
TOWER_H = 100.0
LEAN_DEG = 12.0            # backward lean of everything above the base
BASE_W = TOWER_W
MCU_BASE_D = MCU_L + 2*(MCU_END_GAP + STOP_T + WALL)
FLOOR = 3.2                # tray thickness
CAVITY_TOP = 13.2          # controller cavity ceiling
BASE_TOP = 14.5

# Upper part, in its local upright frame (before leaning).
DISPLAY_W = 14.0
DISPLAY_L = 36.0
DISPLAY_T = 2.9
DISPLAY_BOTTOM = 42.0
TOP_Z = 84.0               # cornice of the head; the crown rises above it
DISPLAY_PAD_T = 2.0
# Pin header + Dupont plugs (18 mm) + cable bend (7 mm), straight back from the PCB
# at the display's lower (pin) end.
HEADER_CLEAR = 25.0
HEADER_W, HEADER_H = 13.2, 5.0   # 5 plugs at 2.54 mm pitch; Z extent from PCB bottom edge
COVER_T = 2.8
COVER_W = 20.8
REAR_C = 0.5               # small rear-corner chamfer keeps the cover's seat full width
REAR_Y = 17.4              # inner face of the rear cover; the lean pivots on its bottom edge
DISPLAY_PCB_BACK_Y = REAR_Y - HEADER_CLEAR - 0.5
DISPLAY_GLASS_Y = DISPLAY_PCB_BACK_Y - 1.9          # 0.9 glass + 1.0 PCB
FRONT_Y = DISPLAY_GLASS_Y - 3.0                     # head front face; screen recess
WINDOW_W = DISPLAY_W + 1.2  # exposes whole PCB; no bezel pressure on glass/FPC
WINDOW_H = DISPLAY_L + 1.2  # includes 0.1-0.2 V-cut size variation
SHAFT_IN_W = 18.4           # inner width of the wiring shaft and display cavity
UPPER_BOTTOM = BASE_TOP - 10.0   # local; reaches into the base so the leaned shaft meets it

USB_W, USB_H = 14.0, 9.0   # cable-overmold clearance, not just metal receptacle
USB_Z = 8.0                # adjustable opening center; clone-dependent
# M2 self-tapping screws everywhere (no room for M2.5 bosses in a 30 mm base).
SCREW_CLEAR = 2.4
SCREW_PILOT = 1.6          # tune on your printer
SCREW_HEAD = 4.0
SCREW_HEAD_H = 1.6
BOSS_D = 4.4
BOSS_X = MCU_W/2 + MCU_SIDE_GAP + BOSS_D/2   # bosses double as controller side guides
COVER_SCREWS = [BASE_TOP+6.0, TOP_Z-4.8]     # local heights

SIN, COS = math.sin(math.radians(LEAN_DEG)), math.cos(math.radians(LEAN_DEG))


def to_world(y, z, pivot_y):
    """Leaned (world) Y, Z of a local upright point."""
    dy, dz = y-REAR_Y, z-BASE_TOP
    return pivot_y + dy*COS + dz*SIN, BASE_TOP - dy*SIN + dz*COS


def local_z(y, z_world):
    """Local height that lands at z_world once leaned, for a point at local y."""
    return BASE_TOP + (z_world - BASE_TOP + (y-REAR_Y)*SIN)/COS


def box(w, d, h, x=0, y=0, z=0):
    return cq.Workplane('XY').box(w, d, h, centered=(True, True, False)).translate((x, y, z)).val()


def above(z):
    """World half-space z >= z (as a large box)."""
    return box(400, 400, 400, z=z)


def band(z0, z1):
    return box(400, 400, z1-z0, z=z0)


def lean(shape, pivot_y):
    shape = shape.rotate(cq.Vector(0, REAR_Y, BASE_TOP), cq.Vector(1, REAR_Y, BASE_TOP), -LEAN_DEG)
    return shape.translate(cq.Vector(0, pivot_y-REAR_Y, 0))


def octagon(w, d, c, x=0, y=0, cr=None):
    """Chamfered rectangle; cr (default c) is the chamfer of the two rear (+Y) corners."""
    cr = c if cr is None else cr
    return [(x+a, y+b) for a, b in [(-w/2+c, -d/2), (w/2-c, -d/2),
            (w/2, -d/2+c), (w/2, d/2-cr), (w/2-cr, d/2),
            (-w/2+cr, d/2), (-w/2, d/2-cr), (-w/2, -d/2+c)]]


def loft(levels):
    """Ruled faceted loft: (z, width, depth, chamfer, center X, center Y[, rear chamfer])."""
    wires = [cq.Workplane('XY').workplane(offset=lv[0]).polyline(octagon(*lv[1:])).close().val()
             for lv in levels]
    return cq.Solid.makeLoft(wires, ruled=True)


def cyl_z(d, h, x, y, z):
    return cq.Solid.makeCylinder(d/2, h, cq.Vector(x,y,z), cq.Vector(0,0,1))


def cyl_y(d, h, x, y, z):
    return cq.Solid.makeCylinder(d/2, h, cq.Vector(x,y,z), cq.Vector(0,1,0))


def union(items):
    s = items[0]
    for item in items[1:]:
        s = s.fuse(item)
    return s.clean()


def level(z, w, front, c, rear=REAR_Y):
    """Loft level from a front face and a rear face (default: the cover plane).
    Levels on the cover plane get a small rear chamfer so the cover is backed."""
    return (z, w, rear-front, c, 0, (rear+front)/2, REAR_C if rear == REAR_Y else c)


# --- upper part, local upright frame ---------------------------------------------------

def make_upper():
    db, t = DISPLAY_BOTTOM, TOP_Z
    body = [
        # shaft, neck, head and a flared cornice
        loft([level(UPPER_BOTTOM,26,-14.0,2.5), level(BASE_TOP+3,26,-14.0,2.5),
              level(26,24,-13.4,2.2), level(db-8,22.4,-12.6,2), level(db-4,22.4,-12.6,2),
              level(db-1.5,27,FRONT_Y-0.6,2.5), level(t-2.5,25.6,FRONT_Y,2.5),
              level(t-1,27.6,FRONT_Y-1.0,2.6), level(t,27.6,FRONT_Y-1.0,2.6)]),
        # collar under the head
        loft([level(db-7,24.4,-13.8,2), level(db-5,23.4,-13.2,2)]),
        # stepped battlement roof
        loft([level(t,23,FRONT_Y+1.5,2.2,REAR_Y-1.5), level(t+2.5,21,FRONT_Y+2.5,2,REAR_Y-2.5)]),
        loft([level(t+2.5,15,FRONT_Y+5.5,1.5,REAR_Y-5.5), level(t+4.5,11,FRONT_Y+7.5,1.2,REAR_Y-7.5)]),
    ]
    # Buttressed silhouette: four side ribs and a front blade.
    for sign in (-1,1):
        for y in (-9.5, REAR_Y-6):
            body.append(loft([(UPPER_BOTTOM,3,5,0.6,sign*13.4,y), (BASE_TOP+4,3,5,0.6,sign*13.4,y),
                              (26,2.4,4,0.5,sign*12.2,y), (db-6,1.6,3,0.3,sign*11.4,y)]))
    body.append(loft([(UPPER_BOTTOM,3,3.4,0.5,0,-14.6), (BASE_TOP+4,3,3.4,0.5,0,-14.6),
                      (26,2.4,2.8,0.4,0,-14.0), (db-6,1.6,2.4,0.3,0,-13.2)]))
    # The fork: two great horns rise beside the head, swell outward, then sweep in
    # over the roof until their tips almost meet. The tips set the tower height.
    y0 = FRONT_Y + 7.0
    tip_y = y0 + 2.4
    tip_z = local_z(tip_y, TOWER_H - 0.2)
    h = tip_z - t
    for sign in (-1,1):
        body.append(loft([(db+22,4.0,11,0.8,sign*12.6,y0),
                          (t-3,4.2,10.5,0.8,sign*12.9,y0),
                          (t+0.30*h,3.6,8,0.7,sign*12.5,y0+0.6),
                          (t+0.60*h,2.8,5.6,0.5,sign*10.6,y0+1.2),
                          (t+0.82*h,2.0,3.6,0.35,sign*7.6,y0+1.8),
                          (t+0.95*h,1.1,1.8,0.2,sign*4.9,y0+2.2),
                          (tip_z,0.4,0.6,0.1,sign*3.6,tip_y)]))
    # Jagged teeth on the battlement corners, taller spires at the rear.
    for x,y,rise in [(-9.4,FRONT_Y+3.6,3.5), (9.4,FRONT_Y+3.6,3.5), (0,FRONT_Y+3.0,3.0),
                     (-9.4,REAR_Y-3.6,4.5), (9.4,REAR_Y-3.6,4.5)]:
        body.append(loft([(t+2.4,2.4,2.4,0.4,x,y), (t+2.5+rise,0.5,0.5,0.1,x*0.95,y)]))
    for sign in (-1,1):
        body.append(loft([(t+2.4,2.6,2.6,0.4,sign*6.0,REAR_Y-5.0),
                          (t+7.5,1.6,1.6,0.3,sign*6.4,REAR_Y-5.0),
                          (t+11,0.5,0.5,0.1,sign*5.6,REAR_Y-5.0)]))
    return union(body)


def upper_cutters():
    """Local-frame voids: (solid, world clip) pairs; clip None = no clipping."""
    shaft_front = DISPLAY_PCB_BACK_Y + 1.5
    cav_front = DISPLAY_GLASS_Y + 0.5
    return [
        # opening from the shaft into the controller cavity (roof only)
        (box(SHAFT_IN_W, REAR_Y-1-shaft_front, BASE_TOP+2-UPPER_BOTTOM,
             y=(REAR_Y-1+shaft_front)/2, z=UPPER_BOTTOM), band(CAVITY_TOP-0.1, BASE_TOP+0.5)),
        # wiring shaft, open to the rear
        (box(SHAFT_IN_W, REAR_Y+2-shaft_front, TOP_Z-2-UPPER_BOTTOM,
             y=(REAR_Y+2+shaft_front)/2, z=UPPER_BOTTOM), above(BASE_TOP)),
        # display cavity and front window
        (box(SHAFT_IN_W, REAR_Y+3-cav_front, 29.4, y=(REAR_Y+3+cav_front)/2,
             z=DISPLAY_BOTTOM+4.4), None),
        (box(WINDOW_W,12,WINDOW_H, y=DISPLAY_GLASS_Y-1.6, z=DISPLAY_BOTTOM-0.6), None),
        # rear-cover seating plane
        (box(COVER_W+0.6,30,TOP_Z+1-UPPER_BOTTOM, y=REAR_Y+15, z=UPPER_BOTTOM), above(BASE_TOP)),
    ]


def make_cover_local():
    bottom = BASE_TOP - 1.5   # trimmed flat to the base top after leaning
    s = box(COVER_W,COVER_T,TOP_Z-bottom, y=REAR_Y+COVER_T/2,z=bottom)
    # Outer face stays flat so this part prints without supports, rear face down.
    # Four narrow pads contact ONLY PCB edge via 2 mm removable foam. Each pad is
    # widened outward into its side fence so the long cantilever is braced.
    pad_face = DISPLAY_PCB_BACK_Y + DISPLAY_PAD_T
    fence_x = DISPLAY_W/2 + 0.9
    pad_x0 = DISPLAY_W/2 - 1.5             # inner edge on the PCB's bare edge strip
    for sign in (-1,1):
        x = sign*(pad_x0+fence_x)/2
        for z in (DISPLAY_BOTTOM+9, DISPLAY_BOTTOM+27):
            s = s.fuse(box(fence_x-pad_x0,REAR_Y-pad_face+0.1,3,x=x,
                           y=(REAR_Y+pad_face)/2,z=z-1.5))
        # Lateral fences: 0.3 mm gap to each side of the nominal board.
        fence_front = DISPLAY_PCB_BACK_Y - 1.2
        s = s.fuse(box(1.2,REAR_Y-fence_front+0.1,24,x=sign*fence_x,
                       y=(REAR_Y+fence_front)/2,z=DISPLAY_BOTTOM+5))
    for z in COVER_SCREWS:
        s = s.cut(cyl_y(SCREW_CLEAR,COVER_T+0.2,0,REAR_Y-0.1,z))
        s = s.cut(cyl_y(SCREW_HEAD,SCREW_HEAD_H+0.1,0,REAR_Y+COVER_T-SCREW_HEAD_H,z))
    return s.clean()


# --- layout ------------------------------------------------------------------------------

def make_layout(upper, cover_local):
    """Base depth and pivot position so the leaned tower stands entirely over the base."""
    u = lean(upper, 0.0).intersect(above(BASE_TOP-0.3)).BoundingBox()
    c = lean(cover_local, 0.0).intersect(above(BASE_TOP)).BoundingBox()
    ymin, ymax = min(u.ymin, c.ymin), max(u.ymax, c.ymax)
    base_d = max(MCU_BASE_D, round(ymax - ymin + 1.8, 1))
    pivot_y = base_d/2 - ymax
    mcu_y = base_d/2 - WALL - STOP_T - MCU_END_GAP - MCU_L/2
    return {'base_d': base_d, 'pivot_y': round(pivot_y, 3), 'mcu_y': mcu_y}


def base_screws(L):
    return [(x, L['mcu_y']+y) for x in (-BOSS_X, BOSS_X) for y in (-10.0, 10.0)]


# --- printed parts (world frame) ------------------------------------------------------

def make_shell(L, upper):
    d = L['base_d']
    base = loft([(FLOOR,BASE_W,d,3,0,0), (9.0,BASE_W,d,3,0,0),
                 (BASE_TOP,BASE_W-1.6,d-1.6,2.5,0,0)])
    s = base.fuse(lean(upper, L['pivot_y']).intersect(above(BASE_TOP-0.3))).clean()
    s = s.cut(box(BASE_W-2*WALL, d-2*WALL, CAVITY_TOP-FLOOR+0.1, z=FLOOR-0.1))
    for solid, clip in upper_cutters():
        cutter = lean(solid, L['pivot_y'])
        s = s.cut(cutter.intersect(clip) if clip is not None else cutter)
    s = s.cut(box(USB_W,2*WALL+2,USB_H, y=d/2, z=USB_Z-USB_H/2))
    # Base screws drive upward into bosses beside the controller's long edges.
    for x,y in base_screws(L):
        s = s.fuse(cyl_z(BOSS_D,CAVITY_TOP-FLOOR+0.1,x,y,FLOOR))
        s = s.cut(cyl_z(SCREW_PILOT,CAVITY_TOP-FLOOR-1,x,y,FLOOR-0.1))
    # Rear cover screws drive toward front; crossbars leave front wiring space.
    for z in COVER_SCREWS:
        s = s.fuse(lean(box(COVER_W,8,7.6,y=REAR_Y-4,z=z-3.8), L['pivot_y']))
        s = s.cut(lean(cyl_y(SCREW_PILOT,8.2,0,REAR_Y-8.1,z), L['pivot_y']))
    return s.clean()


def make_tray(L):
    d, mcu_y = L['base_d'], L['mcu_y']
    s = loft([(0,BASE_W,d,3,0,0), (FLOOR,BASE_W,d,3,0,0)])
    ends = [mcu_y-MCU_L/2-MCU_END_GAP-STOP_T/2, mcu_y+MCU_L/2+MCU_END_GAP+STOP_T/2]
    # Edge saddles under the PCB; the shell's screw bosses guide its long sides.
    for sign in (-1,1):
        for y in (mcu_y-10, mcu_y+10):
            s = s.fuse(box(1.2,3.0,MCU_BOTTOM-FLOOR,x=sign*(MCU_W/2-0.6),y=y,z=FLOOR))
    # End stops resist cable force. Rear stop split around USB receptacle.
    s = s.fuse(box(MCU_W+2*MCU_SIDE_GAP+2.4,STOP_T,5.0,y=ends[0],z=FLOOR))
    for sign in (-1,1):
        s = s.fuse(box(3.2,STOP_T,5.0,x=sign*9.0,y=ends[1],z=FLOOR))
    for x,y in base_screws(L):
        s = s.cut(cyl_z(SCREW_CLEAR,FLOOR+0.2,x,y,-0.1))
        s = s.cut(cyl_z(SCREW_HEAD,SCREW_HEAD_H,x,y,-0.01))
    return s.clean()


def make_cover(L, cover_local):
    return lean(cover_local, L['pivot_y']).intersect(above(BASE_TOP)).clean()


def make_gauge():
    # A cheap full-height window coupon; test 36x14 display outline without tower.
    return box(WINDOW_W+8,3,WINDOW_H+8,z=DISPLAY_BOTTOM-4.6).cut(
        box(WINDOW_W,4,WINDOW_H,z=DISPLAY_BOTTOM-0.6)).clean()


def mock_hardware(L):
    """Keepout/envelope solids, not detailed vendor component models."""
    mcu_y, p = L['mcu_y'], L['pivot_y']
    pcb = box(MCU_W,MCU_L,MCU_PCB_T,y=mcu_y,z=MCU_BOTTOM)
    usb = box(9,7.5,3.3,y=mcu_y+MCU_L/2-2.5,z=MCU_BOTTOM-1.1)
    # Deliberately conservatively allow 2 mm below and 3.4 mm above PCB.
    mcu_envelope = box(MCU_W,MCU_L,MCU_PCB_T+5.4,y=mcu_y,z=MCU_BOTTOM-2)
    display = box(DISPLAY_W,DISPLAY_T,DISPLAY_L,
                  y=DISPLAY_GLASS_Y+DISPLAY_T/2,z=DISPLAY_BOTTOM)
    # Active region from 160x68 geometry and dimension drawing (pins at bottom).
    active = box(10.8,0.05,25.3,y=DISPLAY_GLASS_Y-0.025,z=DISPLAY_BOTTOM+4.6)
    # Header pins, Dupont plugs and their cable bend, straight back from the pin end.
    header = box(HEADER_W,HEADER_CLEAR,HEADER_H,
                 y=DISPLAY_PCB_BACK_Y+HEADER_CLEAR/2,z=DISPLAY_BOTTOM)
    return {'controller_pcb':pcb, 'usb_connector':usb,
            'display_envelope':lean(display,p), 'active_screen':lean(active,p),
            'display_header_cables':lean(header,p)}, mcu_envelope


def on_bed(shape, rotate=False):
    if rotate:
        shape = shape.rotate((0,0,0),(1,0,0),-90)
    b = shape.BoundingBox()
    return shape.translate((-(b.xmin+b.xmax)/2,-(b.ymin+b.ymax)/2,-b.zmin))


def main():
    (OUT/'stl').mkdir(exist_ok=True)
    (OUT/'cad').mkdir(exist_ok=True)
    print('Building enclosure...',flush=True)
    upper = make_upper()
    cover_local = make_cover_local()
    L = make_layout(upper, cover_local)
    print(f'  layout: base depth {L["base_d"]} mm, lean {LEAN_DEG} deg',flush=True)
    parts = {'tower_shell':make_shell(L, upper), 'base_tray':make_tray(L),
             'rear_cover':make_cover(L, cover_local)}
    hardware, mcu_envelope = mock_hardware(L)
    report = {'units':'mm', 'physical_fit_verified':False,
              'layout':{'lean_deg':LEAN_DEG, 'base_depth_mm':L['base_d'],
                        'pivot_y_mm':L['pivot_y'], 'controller_center_y_mm':L['mcu_y']},
              'parts':{}, 'checks':{}}
    assy = cq.Assembly(name='Sauron_dongle')
    for name,shape in parts.items():
        assert shape.isValid(), f'{name}: invalid CAD solid'
        assert len(shape.Solids()) == 1, f'{name}: disconnected solids'
        # The rear cover prints flat on its outer face.
        if name == 'rear_cover':
            printed = on_bed(shape.rotate((0,0,0),(1,0,0),LEAN_DEG),rotate=True)
        else:
            printed = on_bed(shape)
        stl = OUT/'stl'/f'{name}.stl'
        cq.exporters.export(printed,str(stl),tolerance=0.04,angularTolerance=0.08)
        cq.exporters.export(shape,str(OUT/'cad'/f'{name}.step'))
        mesh = trimesh.load_mesh(stl)
        assert mesh.is_watertight and mesh.is_winding_consistent and mesh.is_volume, f'{name}: bad STL'
        assert len(mesh.split()) == 1, f'{name}: disconnected STL'
        b = shape.BoundingBox()
        report['parts'][name] = {'cad_valid':True,'solid_count':1,'stl_watertight':True,
            'stl_winding_consistent':True, 'triangles':len(mesh.faces),
            'volume_mm3':round(shape.Volume(),2),
            'assembled_bounds_mm':[[round(b.xmin,2),round(b.ymin,2),round(b.zmin,2)],
                                   [round(b.xmax,2),round(b.ymax,2),round(b.zmax,2)]],
            'print_size_mm':[round(v,2) for v in mesh.extents]}
        assy.add(shape,name=name,color=cq.Color(0.19,0.21,0.24))
        print(f'  {name}: watertight, single solid, {mesh.extents.round(2)} mm',flush=True)
    # Real geometric checks: no part intersections or blocked hardware envelopes.
    names = list(parts)
    for i,a in enumerate(names):
        for b in names[i+1:]:
            v = parts[a].intersect(parts[b]).Volume()
            report['checks'][f'{a}_vs_{b}_intersection_mm3'] = round(v,6)
            assert v < 0.001, f'{a}/{b} interfere: {v}'
    for name,shape in parts.items():
        for hw in ('display_envelope','usb_connector','display_header_cables','controller_pcb'):
            v = shape.intersect(hardware[hw]).Volume()
            report['checks'][f'{name}_vs_{hw}_intersection_mm3'] = round(v,6)
            assert v < 0.001, f'{name}/{hw} interfere: {v}'
        # Edge saddles intentionally contact PCB bottom, outside this inset keepout.
        keepout = box(MCU_W-2.4,MCU_L-0.2,6.4,y=L['mcu_y'],z=MCU_BOTTOM-2)
        v = shape.intersect(keepout).Volume()
        report['checks'][f'{name}_vs_controller_inset_keepout_mm3'] = round(v,6)
        assert v < 0.001, f'{name}/controller keepout interfere: {v}'
    # USB plug corridor must be open continuously through the rear base wall.
    corridor = box(USB_W-0.2,14,USB_H-0.2,y=L['base_d']/2+3,z=USB_Z-USB_H/2+0.1)
    v = sum(shape.intersect(corridor).Volume() for shape in parts.values())
    report['checks']['usb_cable_corridor_intersection_mm3'] = round(v,6)
    assert v < 0.001, f'USB cable corridor blocked: {v}'
    # The rear cover must rest on the shell (or span the open shaft) across its
    # whole width, at every height: no edge may hang past the tower's back face.
    p = L['pivot_y']
    slab = lean(box(COVER_W,0.3,TOP_Z-0.5-(BASE_TOP+1),y=REAR_Y-0.15,z=BASE_TOP+1), p)
    opening = lean(box(SHAFT_IN_W,2,TOP_Z-(BASE_TOP+1),y=REAR_Y,z=BASE_TOP+1), p)
    bare = slab.cut(parts['tower_shell']).cut(opening)
    v = bare.Volume() if bare.Solids() else 0.0
    report['checks']['rear_cover_unsupported_edge_mm3'] = round(v,6)
    assert v < 0.001, f'rear cover overhangs the shell: {v}'
    sightline = lean(box(DISPLAY_W+0.4,25.6,DISPLAY_L+0.4,
                         y=DISPLAY_GLASS_Y-12.8,z=DISPLAY_BOTTOM-0.2), L['pivot_y'])
    v = sum(shape.intersect(sightline).Volume() for shape in parts.values())
    report['checks']['full_display_front_sightline_intersection_mm3'] = round(v,6)
    assert v < 0.001, f'Display front sightline blocked: {v}'
    assy.export(str(OUT/'cad'/'assembly.step'))
    demo = cq.Assembly(name='Assembly_with_hardware_envelopes')
    for name,shape in parts.items():
        demo.add(shape,name=name,color=cq.Color(0.19,0.21,0.24))
    for name,shape in hardware.items():
        demo.add(shape,name=name,color=cq.Color(0.12,0.5,0.32) if 'pcb' in name else cq.Color(0.65,0.65,0.65))
    demo.export(str(OUT/'cad'/'assembly_with_hardware.step'))
    gauge = on_bed(make_gauge(),rotate=True)
    cq.exporters.export(gauge,str(OUT/'stl'/'display_fit_gauge.stl'))
    gm = trimesh.load_mesh(OUT/'stl'/'display_fit_gauge.stl')
    assert gm.is_watertight and gm.is_volume and len(gm.split()) == 1
    # Assembly-space meshes are used only for previews, not included as print STLs.
    preview_dir = OUT/'cad'/'preview_meshes'
    preview_dir.mkdir(exist_ok=True)
    for name,shape in {**parts, **hardware}.items():
        cq.exporters.export(shape,str(preview_dir/f'{name}.stl'),tolerance=0.08,angularTolerance=0.1)
    # Union component bounds explicitly: OCCT's post-tessellation compound bounds
    # can include conservative surface deflection beyond the actual vertices.
    bounds = [p['assembled_bounds_mm'] for p in report['parts'].values()]
    report['assembled_size_mm'] = [
        round(max(b[1][i] for b in bounds)-min(b[0][i] for b in bounds),2)
        for i in range(3)]
    width, depth, height = report['assembled_size_mm']
    assert width <= TOWER_W + 0.01, f'tower is {width} mm wide (limit {TOWER_W})'
    assert height <= TOWER_H + 0.01, f'tower is {height} mm tall (limit {TOWER_H})'
    (OUT/'validation.json').write_text(json.dumps(report,indent=2)+'\n')
    print('CAD, meshes and geometric checks passed.',flush=True)


if __name__ == '__main__':
    main()
