#!/usr/bin/env python3
"""Render the generated CAD meshes, plus a dimensioned assembly diagram."""
from pathlib import Path
import os
os.environ.setdefault('MPLCONFIGDIR','/private/tmp/sauron-matplotlib')
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle, Polygon, FancyArrowPatch
import numpy as np
import trimesh

ROOT = Path(__file__).resolve().parent
MESHES = ROOT/'cad'/'preview_meshes'
PAPER = '#f3f0e9'
INK = '#252e37'


def draw_mesh(ax,name,color,offset=(0,0,0),alpha=1):
    mesh = trimesh.load_mesh(MESHES/f'{name}.stl')
    vertices = mesh.vertices+np.array(offset)
    triangles = vertices[mesh.faces]
    rgb = np.array(matplotlib.colors.to_rgb(color))
    light = np.array([-0.5,-0.8,0.9]); light /= np.linalg.norm(light)
    shade = 0.58+0.42*np.clip(mesh.face_normals@light,0,1)
    colors = np.column_stack([np.clip(shade[:,None]*rgb[None,:],0,1), np.full(len(shade),alpha)])
    if not hasattr(ax,'_scene_faces'):
        ax._scene_faces=[]; ax._scene_colors=[]
    ax._scene_faces.extend(triangles)
    ax._scene_colors.extend(colors)


def flat_face(ax,verts,color):
    for i in range(1,len(verts)-1):
        ax._scene_faces.append(np.array([verts[0],verts[i],verts[i+1]]))
        ax._scene_colors.append(matplotlib.colors.to_rgba(color))


def stroke(ax,xs,zs,y,width=.12):
    for x1,z1,x2,z2 in zip(xs[:-1],zs[:-1],xs[1:],zs[1:]):
        length=np.hypot(x2-x1,z2-z1)
        if not length: continue
        dx=-(z2-z1)/length*width/2; dz=(x2-x1)/length*width/2
        flat_face(ax,[(x1+dx,y,z1+dz),(x2+dx,y,z2+dz),
                      (x2-dx,y,z2-dz),(x1-dx,y,z1-dz)],'#444e48')


class Scene:
    def __init__(self):
        self._scene_faces=[]
        self._scene_colors=[]


def raster(scene,azim=-68,elev=15,width=900,height=1200):
    """Orthographic software z-buffer; respects occlusion of recessed parts."""
    faces=np.array(scene._scene_faces)
    colors=np.array(scene._scene_colors)[:,:3]
    az,el=np.radians([azim,elev])
    eye=np.array([np.cos(el)*np.cos(az),np.cos(el)*np.sin(az),np.sin(el)])
    right=np.array([-np.sin(az),np.cos(az),0])
    up=np.cross(eye,right)
    projected=faces@np.array([right,up,eye]).T
    lo=projected[:,:,:2].min(axis=(0,1)); hi=projected[:,:,:2].max(axis=(0,1))
    center=(lo+hi)/2
    scale=min(width/(hi[0]-lo[0]),height/(hi[1]-lo[1]))*.94
    projected[:,:,0]=(projected[:,:,0]-center[0])*scale+width/2
    projected[:,:,1]=height/2-(projected[:,:,1]-center[1])*scale
    rgb=np.empty((height,width,3),dtype=np.float64)
    rgb[:]=matplotlib.colors.to_rgb(PAPER)
    depth=np.full((height,width),-np.inf)
    for v,col in zip(projected,colors):
        x0=max(0,int(np.floor(v[:,0].min()))); x1=min(width-1,int(np.ceil(v[:,0].max())))
        y0=max(0,int(np.floor(v[:,1].min()))); y1=min(height-1,int(np.ceil(v[:,1].max())))
        if x1<x0 or y1<y0: continue
        a,b,c=v
        den=(b[1]-c[1])*(a[0]-c[0])+(c[0]-b[0])*(a[1]-c[1])
        if abs(den)<1e-9: continue
        yy,xx=np.mgrid[y0:y1+1,x0:x1+1]
        xx=xx+.5; yy=yy+.5
        wa=((b[1]-c[1])*(xx-c[0])+(c[0]-b[0])*(yy-c[1]))/den
        wb=((c[1]-a[1])*(xx-c[0])+(a[0]-c[0])*(yy-c[1]))/den
        wc=1-wa-wb
        z=wa*a[2]+wb*b[2]+wc*c[2]
        tile=depth[y0:y1+1,x0:x1+1]
        inside=(wa>=-1e-7)&(wb>=-1e-7)&(wc>=-1e-7)&(z>tile)
        tile[inside]=z[inside]
        rgb[y0:y1+1,x0:x1+1][inside]=col
    # Screen-space ambient shading exposes the cable recess and service openings
    # without inventing geometry. Background samples never cast a shadow.
    finite=np.isfinite(depth)
    occ=np.zeros_like(depth)
    padded=np.pad(depth,64,constant_values=-np.inf)
    for radius in (4,12,28,60):
        for dx,dy in ((radius,0),(-radius,0),(0,radius),(0,-radius)):
            neighbor=padded[64+dy:64+dy+height,64+dx:64+dx+width]
            with np.errstate(invalid='ignore'):
                delta=neighbor-depth
            occ+=finite & (delta>0.4) & (delta<55)
    rgb*=np.clip(1-occ/16*.8,.3,1)[:,:,None]
    return np.uint8(np.clip(rgb*255,0,255))


def show_scene(fig,rect,scene,azim=-68,elev=15):
    ax=fig.add_axes(rect)
    ax.imshow(raster(scene,azim,elev,width=1000,height=1400))
    ax.axis('off')
    return ax


def front_screen(ax):
    # Illustrative content only; the actual firmware draws its own monochrome UI.
    y=-9.47
    screen = np.array([[-5.4,y,90.6],[5.4,y,90.6],[5.4,y,115.9],[-5.4,y,115.9]])
    flat_face(ax,screen,'#c8ccbc')
    for z,length in [(113.7,7.5),(111.8,6.8),(108.8,7.9),(106.8,7.1)]:
        stroke(ax,[-4.2,-4.2+length],[z,z],y-.04,.12)
    t=np.linspace(0,2*np.pi,100)
    # A lidded eye low in the portrait window, matching the README's layout.
    stroke(ax,4.4*np.cos(t),98.3+2.2*np.sin(t)*np.abs(np.sin(t))**.25,y-.05,.16)
    stroke(ax,[0,0],[96.5,100.1],y-.06,.32)


def render():
    fig=plt.figure(figsize=(15,10),facecolor=PAPER)
    fig.text(.055,.935,'THE WATCHTOWER',fontsize=27,fontweight='bold',color=INK)
    fig.text(.055,.901,'Barad-dûr inspired  /  ZMK display dongle',fontsize=12,color='#63707a')
    ax=Scene()
    for name in ['tower_shell','base_tray','rear_cover']:
        draw_mesh(ax,name,'#64717d')
    draw_mesh(ax,'display_envelope','#202c32')
    front_screen(ax)
    show_scene(fig,[.01,.10,.47,.76],ax)
    fig.text(.085,.085,'ACTUAL CAD GEOMETRY · illustrative screen content',fontsize=8,color='#63707a')

    ax2=Scene()
    draw_mesh(ax2,'tower_shell','#81909a')
    draw_mesh(ax2,'base_tray','#5c6c76',offset=(0,0,-22))
    draw_mesh(ax2,'rear_cover','#72838c',offset=(0,45,0))
    draw_mesh(ax2,'controller_pcb','#3e9670')
    draw_mesh(ax2,'usb_connector','#aeb7bb')
    draw_mesh(ax2,'display_envelope','#e2b169',offset=(0,45,0))
    show_scene(fig,[.49,.47,.47,.36],ax2,azim=55,elev=16)
    fig.text(.53,.84,'SERVICEABLE THREE-PART CASE',fontsize=12,fontweight='bold',color=INK)
    fig.text(.53,.415,'Rear cover carries the upright display.\nBase tray carries the horizontal controller.',fontsize=10,color='#63707a',linespacing=1.6)

    ax3=fig.add_axes([.55,.10,.37,.21])
    ax3.set_aspect('equal'); ax3.axis('off'); ax3.set_xlim(-38,42); ax3.set_ylim(-8,158)
    # True side orientation diagram, Y horizontal / Z vertical.
    ax3.add_patch(Polygon([(-30,0),(30,0),(30,7),(23,20),(10,20),(10,126),(-13,126),(-13,84),(-12,58),(-20,20),(-30,7)],
                          facecolor='#d9dedb',edgecolor='#89969c',lw=.8))
    ax3.add_patch(Rectangle((-8.5,6.4),33,1,color='#3e9670'))
    ax3.add_patch(Rectangle((18.25,5.3),7.5,3.3,color='#89969c'))
    ax3.add_patch(Rectangle((-9.4,86),2.9,36,color='#d99a4e'))
    ax3.plot([-5,-5,6],[25,42,42],color='#63707a',lw=.8)
    ax3.text(8,40,'90°',fontsize=9,color=INK)
    ax3.annotate('USB →',xy=(26,7),xytext=(0,-5),fontsize=8,color=INK)
    ax3.text(15,99,'36 × 14 mm\nnice!view',fontsize=8,color=INK)
    ax3.text(-37,53,'33 × 18 mm\nnRF52840\nflat in base',fontsize=8,color=INK)
    fig.text(.53,.355,'66 × 60 × 152 mm assembled',fontsize=16,fontweight='bold',color=INK)
    fig.text(.53,.32,'USB-C at the rear · portrait screen at the front',fontsize=10,color='#63707a')
    fig.text(.055,.04,'PRINT AT 100%  ·  mm  ·  3 STLs + fit gauge  ·  STEP + parametric source included',fontsize=10,color=INK)
    fig.savefig(ROOT/'preview.png',dpi=160,facecolor=PAPER)
    plt.close(fig)

    # Separate large front/rear view for checking openings and assembly.
    fig=plt.figure(figsize=(12,10),facecolor=PAPER)
    for i,(az,title) in enumerate([(-90,'FRONT / DISPLAY'),(90,'REAR / USB + COVER')]):
        ax=Scene()
        for name in ['tower_shell','base_tray','rear_cover']:
            draw_mesh(ax,name,'#6d7d87')
        draw_mesh(ax,'controller_pcb','#3e9670')
        draw_mesh(ax,'usb_connector','#aeb7bb')
        draw_mesh(ax,'display_envelope','#28383b')
        if i==0: front_screen(ax)
        image_ax=show_scene(fig,[.03+i*.49,.04,.45,.87],ax,azim=az,elev=0)
        image_ax.set_title(title,fontsize=12,color=INK,pad=15)
    fig.savefig(ROOT/'orthographic.png',dpi=160,facecolor=PAPER,bbox_inches='tight')
    plt.close(fig)
    print('Rendered preview.png and orthographic.png')


if __name__=='__main__':
    render()
