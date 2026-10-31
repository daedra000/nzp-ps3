import struct,sys
def load_all(p):
    d=open(p,'rb').read()
    h=struct.unpack('<4si3f3ff3f8if',d[:84])
    scale=h[2:5];tr=h[5:8];nskins,sw,shh,nv,nt,nf=h[12:18]
    o=84
    for _ in range(nskins):
        g,=struct.unpack('<i',d[o:o+4]);o+=4
        if g==0:o+=sw*shh
        else:
            n,=struct.unpack('<i',d[o:o+4]);o+=4+4*n+n*sw*shh
    o+=nv*12+nt*16
    frames=[]
    for f in range(nf):
        t,=struct.unpack('<i',d[o:o+4]);o+=4
        if t==0:
            o+=8; name=d[o:o+16].split(b'\0')[0].decode(errors='replace');o+=16
            pts=[tuple(scale[k]*d[o+i*4+k]+tr[k] for k in range(3)) for i in range(nv)];o+=nv*4
            frames.append((name,pts))
        else:
            n,=struct.unpack('<i',d[o:o+4]);o+=4+8+4*n
            for g in range(n):
                o+=8; name=d[o:o+16].split(b'\0')[0].decode(errors='replace');o+=16
                pts=[tuple(scale[k]*d[o+i*4+k]+tr[k] for k in range(3)) for i in range(nv)];o+=nv*4
                frames.append((name,pts))
    return nv,frames


if __name__ == "__main__":
    # python3 tools/mdl_frames.py path/to/models/player.mdl > vril/source/player_hands.h
    import math
    nv, fr = load_all(sys.argv[1])
    p0 = fr[0][1]
    hand = [i for i in range(nv) if math.dist(p0[i], p0[340]) < 3.0]
    print("/* player_hands.h: where models/player.mdl's right hand is in each of its %d frames" % len(fr))
    print(" * (the middle of the %d vertices around vertex 340, the hand at the end of the" % len(hand))
    print(" * arm), in model units. Made by tools/mdl_frames.py from the nzp-team/assets")
    print(" * player.mdl (see cl_tent.c CL_AddPlayerWeapons). */")
    print("#define PLAYER_HAND_FRAMES %d" % len(fr))
    print("static const float player_hand[PLAYER_HAND_FRAMES][3] = {")
    for name, p in fr:
        print("\t{%.2ff, %.2ff, %.2ff}," % tuple(sum(p[i][k] for i in hand) / len(hand) for k in range(3)))
    print("};")
