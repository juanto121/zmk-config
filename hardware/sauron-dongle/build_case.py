#!/usr/bin/env python3
"""Parametric Barad-dur-inspired dongle enclosure. All dimensions in mm.

X = left/right, Y = front(-)/rear(+), Z = desk/up.
STLs are individually placed on the print bed; STEP preserves assembly coordinates.
See README.md for sources, assumed hardware, print settings and assembly.
"""
from pathlib import Path
import json

import cadquery as cq
import trimesh

OUT = Path(__file__).resolve().parent

# Measure your actual clone before changing these. No pin headers or carrier PCB.
MCU_W = 18.0               # manufacturer: 17.78; rounded up for clone variation
MCU_L = 33.0
MCU_PCB_T = 1.0            # assembly assumption, not specified in outline drawing
MCU_Y = 8.0
MCU_BOTTOM = 6.4
MCU_SIDE_GAP = 0.40
MCU_END_GAP = 0.60
DISPLAY_W = 14.0
DISPLAY_L = 36.0
DISPLAY_T = 2.9
DISPLAY_BOTTOM = 86.0
DISPLAY_GLASS_Y = -9.4
DISPLAY_PCB_BACK_Y = -7.5   # 0.9 glass + 1.0 PCB from manufacturer's section
DISPLAY_PAD_T = 2.0
WINDOW_W = DISPLAY_W + 1.2  # exposes whole PCB; no bezel pressure on glass/FPC
WINDOW_H = DISPLAY_L + 1.2  # includes 0.1-0.2 V-cut size variation
BASE_W, BASE_D, FLOOR = 66.0, 60.0, 3.2
USB_W, USB_H = 14.0, 9.0   # cable-overmold clearance, not just metal receptacle
USB_Z = 8.0                # adjustable opening center; clone-dependent
SCREW_CLEAR = 2.8
SCREW_PILOT = 2.1          # M2.5 self-tap into plastic; tune on your printer
SCREW_HEAD = 4.8
BASE_SCREWS = [(x, y) for x in (-25.0, 25.0) for y in (-19.0, 19.0)]
COVER_SCREWS = [28.0, 119.5]


def box(w, d, h, x=0, y=0, z=0):
    return cq.Workplane('XY').box(w, d, h, centered=(True, True, False)).translate((x, y, z)).val()


def octagon(w, d, c, x=0, y=0):
    return [(x+a, y+b) for a, b in [(-w/2+c, -d/2), (w/2-c, -d/2),
            (w/2, -d/2+c), (w/2, d/2-c), (w/2-c, d/2),
            (-w/2+c, d/2), (-w/2, d/2-c), (-w/2, -d/2+c)]]


def loft(levels):
    """Ruled faceted loft: (z, width, depth, corner chamfer, center X, center Y)."""
    wires = [cq.Workplane('XY').workplane(offset=z).polyline(octagon(w,d,c,x,y)).close().val()
             for z,w,d,c,x,y in levels]
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


def make_shell():
    s = union([
        loft([(FLOOR,66,60,8,0,0), (6.0,66,60,8,0,0),
              (8.0,62,56,7,0,0), (13,60,54,7,0,0), (20,48,44,6,0,0)]),
        loft([(18,46,30,3,0,-5), (30,38,26,3,0,-3),
              (58,30,22,2.5,0,-1), (80,28,21.5,2,0,-0.75),
              (88,34,23,2.5,0,-1.5), (126,30,23,2.5,0,-1.5)]),
    ])
    # Buttressed silhouette: four tall side ribs, three fluted front blades.
    ribs = []
    for sign in (-1,1):
        for y in (-9,5):
            ribs.append(loft([(7,8,8,1,sign*27,y), (24,6,7,1,sign*21,y),
                              (54,4,5,0.6,sign*15,y), (83,2.4,3,0.4,sign*13,y)]))
        ribs.append(loft([(7,7,9,0.8,sign*15,-22), (28,5,7,0.8,sign*12,-15),
                          (76,2.4,4,0.4,sign*10.4,-11.5)]))
    ribs.append(loft([(8,5,6,0.6,0,-25), (30,4,5,0.5,0,-16),
                      (78,2.4,3,0.4,0,-11.4)]))
    for z,w,d,cy in [(33,38,28,-3), (57,33,25,-1), (80,32,25,-1.5)]:
        ribs.append(loft([(z,w,d,2,0,cy), (z+2,w-2,d-2,2,0,cy)]))
    # Two inward-curving crown talons and two shorter rear spires.
    for sign in (-1,1):
        ribs.append(loft([(108,10,18,1.4,sign*16,-4), (124,9,15,1.3,sign*21,-3),
                          (138,5,9,0.8,sign*20,-2), (152,1.6,3,0.3,sign*14,-1)]))
        ribs.append(loft([(116,6,7,0.8,sign*12,5.5), (130,5,6,0.6,sign*14,5.5),
                          (143,1.6,2.4,0.3,sign*11,5.5)]))
    s = union([s] + ribs)
    # Bottom-accessible base cavity, wiring shaft, and fully open rear service slot.
    s = s.cut(box(44,44,13.3, z=3.1))
    s = s.cut(box(MCU_W+2*MCU_SIDE_GAP+3.2, MCU_L+2*MCU_END_GAP+3.2,
                  11.4, y=MCU_Y, z=3.1))
    s = s.cut(box(20.8,18,108, y=3, z=16))  # -6..12, open to rear
    s = s.cut(box(18.4,22,29.4,y=2.1,z=DISPLAY_BOTTOM+4.4))
    s = s.cut(box(WINDOW_W,12,WINDOW_H, y=-11, z=DISPLAY_BOTTOM-0.6))
    s = s.cut(box(25.8,30,106.1,y=25,z=20))  # rear-cover seating plane
    s = s.cut(box(USB_W,14,USB_H, y=28, z=USB_Z-USB_H/2))
    # Base screws drive upward. Bosses overlap side walls, outside controller.
    for x,y in BASE_SCREWS:
        s = s.fuse(cyl_z(7.2,11.2,x,y,FLOOR))
        s = s.cut(cyl_z(SCREW_PILOT,10.2,x,y,FLOOR-0.1))
    # Rear cover screws drive toward front; crossbars leave front wiring space.
    for z in COVER_SCREWS:
        s = s.fuse(box(27,8,7.6,y=6,z=z-3.8))
        s = s.cut(cyl_y(SCREW_PILOT,8.2,0,1.9,z))
    # Recessed lancet grooves on the lower front facade.
    for x in (-6,6):
        groove = loft([(29,1.4,5,0.2,x,-17), (61,1.2,5,0.2,x,-13),
                       (68,0.5,4,0.15,x,-12.5)])
        s = s.cut(groove)
    return s.clean()


def make_tray():
    s = loft([(0,66,60,8,0,0), (FLOOR,66,60,8,0,0)])
    side_x = MCU_W/2 + MCU_SIDE_GAP + 0.6
    ends = [MCU_Y-MCU_L/2-MCU_END_GAP-0.6, MCU_Y+MCU_L/2+MCU_END_GAP+0.6]
    for sign in (-1,1):
        s = s.fuse(box(1.2,MCU_L+2*MCU_END_GAP+2.4,5.0,x=sign*side_x,y=MCU_Y,z=FLOOR))
        for y in (-2,18):
            s = s.fuse(box(1.2,3.0,MCU_BOTTOM-FLOOR,x=sign*(MCU_W/2-0.6),y=y,z=FLOOR))
    # End stops resist cable force. Rear stop split around USB receptacle.
    s = s.fuse(box(MCU_W+2*MCU_SIDE_GAP+2.4,1.2,5.0,y=ends[0],z=FLOOR))
    for sign in (-1,1):
        s = s.fuse(box(3.2,1.2,5.0,x=sign*9.0,y=ends[1],z=FLOOR))
    # Loose registration tabs fit inside the base cavity.
    for sign in (-1,1):
        s = s.fuse(box(1.2,10,1.6,x=sign*21.1,z=FLOOR))
    for x,y in BASE_SCREWS:
        s = s.cut(cyl_z(SCREW_CLEAR,FLOOR+0.2,x,y,-0.1))
        s = s.cut(cyl_z(SCREW_HEAD,1.8,x,y,-0.01))
    return s.clean()


def make_cover():
    s = box(25.2,2.8,106, y=11.4,z=20)
    # Outer face stays flat so this part prints without supports, rear face down.
    # Four narrow pads contact ONLY PCB edge via 2 mm removable foam.
    pad_face = DISPLAY_PCB_BACK_Y + DISPLAY_PAD_T
    for sign in (-1,1):
        x = sign*(DISPLAY_W/2-0.8)
        for z in (DISPLAY_BOTTOM+9, DISPLAY_BOTTOM+27):
            s = s.fuse(box(1.4,10-pad_face+0.1,3,x=x,y=(10+pad_face)/2,z=z-1.5))
        # Lateral fences: 0.3 mm gap to each side of the nominal board.
        s = s.fuse(box(1.2,18.7,24,x=sign*(DISPLAY_W/2+0.9),y=0.65,z=DISPLAY_BOTTOM+5))
    for z in COVER_SCREWS:
        s = s.cut(cyl_y(SCREW_CLEAR,3.0,0,9.9,z))
        s = s.cut(cyl_y(SCREW_HEAD,1.8,0,11.1,z))
    return s.clean()


def make_gauge():
    # A cheap full-height window coupon; test 36x14 display outline without tower.
    return box(WINDOW_W+8,3,WINDOW_H+8,z=DISPLAY_BOTTOM-4.6).cut(
        box(WINDOW_W,4,WINDOW_H,z=DISPLAY_BOTTOM-0.6)).clean()


def mock_hardware():
    """Keepout/envelope solids, not detailed vendor component models."""
    pcb = box(MCU_W,MCU_L,MCU_PCB_T,y=MCU_Y,z=MCU_BOTTOM)
    usb = box(9,7.5,3.3,y=MCU_Y+MCU_L/2-2.5,z=MCU_BOTTOM-1.1)
    # Deliberately conservatively allow 2 mm below and 3.4 mm above PCB.
    mcu_envelope = box(MCU_W,MCU_L,MCU_PCB_T+5.4,y=MCU_Y,z=MCU_BOTTOM-2)
    display = box(DISPLAY_W,DISPLAY_T,DISPLAY_L,
                  y=DISPLAY_GLASS_Y+DISPLAY_T/2,z=DISPLAY_BOTTOM)
    # Active region from 160x68 geometry and dimension drawing (pins at bottom).
    active = box(10.8,0.05,25.3,y=DISPLAY_GLASS_Y-0.025,z=DISPLAY_BOTTOM+4.6)
    return {'controller_pcb':pcb, 'usb_connector':usb,
            'display_envelope':display, 'active_screen':active}, mcu_envelope


def on_bed(shape, rotate=False):
    if rotate:
        shape = shape.rotate((0,0,0),(1,0,0),-90)
    b = shape.BoundingBox()
    return shape.translate((-(b.xmin+b.xmax)/2,-(b.ymin+b.ymax)/2,-b.zmin))


def main():
    (OUT/'stl').mkdir(exist_ok=True)
    (OUT/'cad').mkdir(exist_ok=True)
    print('Building enclosure...',flush=True)
    parts = {'tower_shell':make_shell(), 'base_tray':make_tray(), 'rear_cover':make_cover()}
    hardware, mcu_envelope = mock_hardware()
    report = {'units':'mm', 'physical_fit_verified':False, 'parts':{}, 'checks':{}}
    assy = cq.Assembly(name='Sauron_dongle')
    for name,shape in parts.items():
        assert shape.isValid(), f'{name}: invalid CAD solid'
        assert len(shape.Solids()) == 1, f'{name}: disconnected solids'
        printed = on_bed(shape,rotate=name=='rear_cover')
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
        for hw in ('display_envelope','usb_connector'):
            v = shape.intersect(hardware[hw]).Volume()
            report['checks'][f'{name}_vs_{hw}_intersection_mm3'] = round(v,6)
            assert v < 0.001, f'{name}/{hw} interfere: {v}'
        # Edge saddles intentionally contact PCB bottom, outside this inset keepout.
        keepout = box(MCU_W-2.4,MCU_L-0.2,6.4,y=MCU_Y,z=MCU_BOTTOM-2)
        v = shape.intersect(keepout).Volume()
        report['checks'][f'{name}_vs_controller_inset_keepout_mm3'] = round(v,6)
        assert v < 0.001, f'{name}/controller keepout interfere: {v}'
    # USB plug corridor must be open continuously through the rear base wall.
    corridor = box(USB_W-0.2,14,USB_H-0.2,y=28,z=USB_Z-USB_H/2+0.1)
    v = sum(shape.intersect(corridor).Volume() for shape in parts.values())
    report['checks']['usb_cable_corridor_intersection_mm3'] = round(v,6)
    assert v < 0.001, f'USB cable corridor blocked: {v}'
    sightline = box(DISPLAY_W+0.4,25.6,DISPLAY_L+0.4,
                    y=-22.2,z=DISPLAY_BOTTOM-0.2)
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
    (OUT/'validation.json').write_text(json.dumps(report,indent=2)+'\n')
    print('CAD, meshes and geometric checks passed.',flush=True)


if __name__ == '__main__':
    main()
