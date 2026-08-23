import math

# ---------- oklch -> srgb ----------
def oklch_to_srgb(L,C,Hdeg):
    h=math.radians(Hdeg); a=C*math.cos(h); b=C*math.sin(h)
    l_=L+0.3963377774*a+0.2158037573*b
    m_=L-0.1055613458*a-0.0638541728*b
    s_=L-0.0894841775*a-1.2914855480*b
    l=l_**3; m=m_**3; s=s_**3
    r= 4.0767416621*l -3.3077115913*m +0.2309699292*s
    g=-1.2684380046*l +2.6097574011*m -0.3413193965*s
    bb=-0.0041960863*l -0.7034186147*m +1.7076147010*s
    def enc(u):
        u=max(0.0,min(1.0,u))
        return 12.92*u if u<=0.0031308 else 1.055*(u**(1/2.4))-0.055
    return tuple(enc(x)*255 for x in (r,g,bb))

def over(fg, a, bg):
    return tuple(a*fg[i]+(1-a)*bg[i] for i in range(3))

def lum(c):
    def lin(v):
        v=v/255.0
        return v/12.92 if v<=0.04045 else ((v+0.055)/1.055)**2.4
    r,g,b=[lin(x) for x in c]
    return 0.2126*r+0.7152*g+0.0722*b

def ratio(c1,c2):
    a,b=lum(c1),lum(c2)
    if a<b: a,b=b,a
    return (a+0.05)/(b+0.05)

# ---------- the compositing chain ----------
VEIL=(4,6,9); VEIL_A=0.62          # compositor blur+darkening 0.8, modelled
SURF=(8,9,11); SURF_A=0.86         # surface/base

def console_bg(game):
    return over(SURF, SURF_A, over(VEIL, VEIL_A, game))

GAMES={'black night scene':(0,0,0),'orange fire':(217,118,42),'white snow':(255,255,255)}
BGS={k:console_bg(v) for k,v in GAMES.items()}

def elev(bg,a): return over((255,255,255),a,bg)

TEXT=(239,245,251)
print("=== effective console background ===")
for k,v in BGS.items(): print(f"  {k:20s} rgb{tuple(round(x) for x in v)}  L={lum(v):.5f}")

print("\n=== text roles (alpha of #EFF5FB) over base surface / focused row (e2 6%) ===")
for name,a in [('primary',.96),('label',.82),('meta',.62),('faint',.44),
               ('OLD label .68',.68),('OLD meta .46',.46),('OLD disabled .30',.30)]:
    out=[]
    for k,bg in BGS.items():
        for ename,ebg in (('base',bg),('row',elev(bg,.06)),('rail',elev(bg,.035))):
            out.append(ratio(over(TEXT,a,ebg),ebg))
    print(f"  text/{name:15s} min={min(out):5.2f}:1  max={max(out):5.2f}:1")

print("\n=== accent roles, hue swept 0..359 (worst hue reported) ===")
ACC={'accent (tick/fill)':(.74,.12),'accent-edge':(.80,.12),'accent-value':(.84,.10),
     'accent-text':(.82,.10),'accent-knob':(.86,.08),'accent-seg':(.90,.07)}
for name,(L,C) in ACC.items():
    worst=(999,None)
    for h in range(0,360):
        col=oklch_to_srgb(L,C,h)
        r=min(ratio(col,bg) for bg in BGS.values())
        r2=min(ratio(col,elev(bg,.06)) for bg in BGS.values())
        r=min(r,r2)
        if r<worst[0]: worst=(r,h)
    print(f"  {name:22s} worst {worst[0]:5.2f}:1 at hue {worst[1]}")

print("\n=== state roles (hue-fixed) ===")
for name,(L,C,H) in {'ok':(.78,.16,145),'warn':(.72,.17,55),'danger':(.70,.17,25)}.items():
    col=oklch_to_srgb(L,C,H)
    r=min(ratio(col,bg) for bg in BGS.values())
    print(f"  state/{name:8s} rgb{tuple(round(x) for x in col)} worst {r:5.2f}:1")

print("\n=== state-carrying UI parts (need >=3:1) ===")
# switch OFF knob: white @ alpha over track fill (white@7% over surface)
for a in (.55,.72,.80):
    out=[]
    for bg in BGS.values():
        track=elev(bg,.07)
        knob=over((255,255,255),a,track)
        out.append(ratio(knob,bg))
    print(f"  switch-off knob white@{int(a*100)}%   vs surface: {min(out):5.2f}:1")
# slider rail (unfilled track) vs surface -- must be visible as a component
for a in (.16,.22,.28):
    out=[ratio(elev(bg,a),bg) for bg in BGS.values()]
    print(f"  slider rail white@{int(a*100)}%       vs surface: {min(out):5.2f}:1")
# hairlines
for a in (.045,.07,.10,.12,.34):
    out=[ratio(elev(bg,a),bg) for bg in BGS.values()]
    print(f"  hairline white@{str(int(a*100))+'%':4s}          vs surface: {min(out):5.2f}:1")
# focused row fill vs base surface (state indicator)
for a in (.06,.09):
    out=[ratio(elev(bg,a),bg) for bg in BGS.values()]
    print(f"  focus row fill white@{int(a*100)}%    vs surface: {min(out):5.2f}:1")

print("\n=== segmented control: inactive label & active fill ===")
for a in (.50,.72):
    out=[]
    for bg in BGS.values():
        cell=elev(bg,.05)
        out.append(ratio(over(TEXT,a,cell),cell))
    print(f"  inactive segment label @{int(a*100)}%: {min(out):5.2f}:1")
