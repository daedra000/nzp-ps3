#!/usr/bin/env python3
"""Builds ps3gl_shader_data.h: every RSX program of the PS3 GL layer.

    build_shaders.py CGCOMP OUT.h [VASM_DIR]

The programs are NV40 assembly assembled by PSL1GHT's cgcomp in its assembler mode
(-a), which needs no NVIDIA Cg compiler, so the build makes them from source every time:

  nzp_vp          rsx/shaders/nzp_vp.vasm: the game's vertex program (mvp, linear GL fog,
                  texture matrix on unit 0)
  present_vp      plain mvp + pass-through, for ps3_video.c's brightness/gamma pass
  present_fp      that pass: rgb = saturate(pow(frame, tc1.y) * tc1.x)
  ps3gl_fp[40]    one fragment program per combination of the three texture units'
                  GL texture environments (ps3gl_draw.c ChooseFP), each ending in GL fog

cgcomp skips opcodes it doesn't know without a word, so every program's instruction
count is checked against the lines written: a typo fails the build.
"""
import os
import struct
import subprocess
import sys
import tempfile

U0, U1, U2 = 4, 5, 2   # PS3GL_U0_*, PS3GL_U1_*, PS3GL_U2_* (ps3gl.h)

FP_HEADER = """!!ARBfp1.0
#var sampler2D tex0 : TEXUNIT0 : texunit 0 : -1 : 1
#var sampler2D tex1 : TEXUNIT1 : texunit 1 : -1 : 1
#var sampler2D tex2 : TEXUNIT2 : texunit 2 : -1 : 1
"""

# GL linear fog, colour and factor from nzp_vp's texcoord[2] (factor 1 = no fog).
FP_FOG = "LRP R0.xyz, fragment.texcoord[2].wwww, R0, fragment.texcoord[2];"


def texenv_fp(u0, u1, u2):
    """GL's texture environments, as TyrQuakeCell's gen_fp.py does them in Cg."""
    b = []
    if u0 == 0:
        b.append("MOV R0, fragment.color;")
    elif u0 == 1:
        b.append("TEX R0, fragment.texcoord[0], texture[0], 2D;")
    elif u0 == 2:
        b.append("TEX R1, fragment.texcoord[0], texture[0], 2D;")
        b.append("MUL R0, R1, fragment.color;")
    else:  # GL_COMBINE modulate x2, clamped
        b.append("TEX R1, fragment.texcoord[0], texture[0], 2D;")
        b.append("MUL R1, R1, fragment.color;")
        b.append("ADD_SAT R0.xyz, R1, R1;")
        b.append("MOV R0.w, R1.w;")
    if u1 == 1:
        b.append("TEX R1, fragment.texcoord[1], texture[1], 2D;")
        b.append("MUL R0, R0, R1;")
    elif u1 in (2, 3):  # x2 / x4, clamped
        b.append("TEX R1, fragment.texcoord[1], texture[1], 2D;")
        b.append("MUL R2, R0, R1;")
        if u1 == 3:
            b.append("ADD R2.xyz, R2, R2;")
        b.append("ADD_SAT R0.xyz, R2, R2;")
        b.append("MOV R0.w, R2.w;")
    elif u1 == 4:  # GL_DECAL
        b.append("TEX R1, fragment.texcoord[1], texture[1], 2D;")
        b.append("LRP R0.xyz, R1.wwww, R1, R0;")
    if u2 == 1:  # GL_DECAL on unit 2, sampled with texcoord 0 (fullbright masks)
        b.append("TEX R1, fragment.texcoord[0], texture[2], 2D;")
        b.append("LRP R0.xyz, R1.wwww, R1, R0;")
    b.append(FP_FOG)
    b.append("MOV result.color, R0;")
    return FP_HEADER + "\n".join(b) + "\nEND\n"


PRESENT_VP = """!!VP2.0
# present_vp: mvp * position, colour and both texcoords passed through.
#var float4x4 mvp :  : c[0], 4 : -1 : 1
#var float4 position : $vin.ATTR0 : ATTR0 : -1 : 1
#var float4 color : $vin.ATTR3 : ATTR3 : -1 : 1
#var float2 tc0 : $vin.ATTR8 : ATTR8 : -1 : 1
#var float2 tc1 : $vin.ATTR9 : ATTR9 : -1 : 1
DP4 R0.x, c[0], v[0];
DP4 R0.y, c[1], v[0];
DP4 R0.z, c[2], v[0];
DP4 R0.w, c[3], v[0];
MOV result.position, R0;
MOV result.color, v[3];
MOV result.texcoord[0], v[8];
MOV result.texcoord[1], v[9];
END
"""

# tc1.x = brightness (gain), tc1.y = exponent (gamma); see ps3_video.c ColorPass.
PRESENT_FP = """!!ARBfp1.0
#var sampler2D tex0 : TEXUNIT0 : texunit 0 : -1 : 1
#const c[0] = 1 0 0 0
TEX R0, fragment.texcoord[0], texture[0], 2D;
POW R1.x, R0.x, fragment.texcoord[1].y;
POW R1.y, R0.y, fragment.texcoord[1].y;
POW R1.z, R0.z, fragment.texcoord[1].y;
MUL_SAT R0.xyz, R1, fragment.texcoord[1].xxxx;
MOV R0.w, c[0].x;
MOV result.color, R0;
END
"""


# What cgcomp emits per source line, measured with its -a mode: in fragment programs
# LRP is 2 instructions and POW 3 (LG2, MUL, EX2), and an instruction that reads a
# c[] constant is followed by the constant's own 16-byte slot, counted as one more.
# END is never counted.
FP_EXPANDS = {"LRP": 2, "POW": 3}


def count_insns(src, is_vp):
    n = 0
    for line in src.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("!!") or line == "END":
            continue
        if is_vp:
            n += 1
        else:
            op = line.split()[0].split("_")[0]
            n += FP_EXPANDS.get(op, 1) + (1 if "c[" in line else 0)
    return n


def assemble(cgcomp, tmp, name, src, is_vp):
    asm = os.path.join(tmp, name + (".vasm" if is_vp else ".fasm"))
    out = os.path.join(tmp, name + (".vpo" if is_vp else ".fpo"))
    with open(asm, "w") as f:
        f.write(src)
    r = subprocess.run([cgcomp, "-v" if is_vp else "-f", "-a", asm, out],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0 or not os.path.exists(out):
        sys.exit("cgcomp failed on %s:\n%s" % (name, r.stdout.decode(errors="replace")))
    data = open(out, "rb").read()
    num_insn = struct.unpack(">H", data[10:12])[0]
    want = count_insns(src, is_vp)
    if num_insn != want:
        sys.exit("%s: cgcomp assembled %d instructions, %d written -- an opcode it "
                 "doesn't know was dropped" % (name, num_insn, want))
    return data


def c_array(name, data):
    lines = ["static const unsigned char %s[] __attribute__((aligned(16))) = {" % name]
    for i in range(0, len(data), 12):
        lines.append("\t" + ", ".join("0x%02x" % b for b in data[i:i + 12]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    cgcomp, out_h = sys.argv[1], sys.argv[2]
    here = os.path.dirname(os.path.abspath(__file__))
    vasm_dir = sys.argv[3] if len(sys.argv) > 3 else os.path.join(here, "..", "rsx", "shaders")
    nzp_vp = open(os.path.join(vasm_dir, "nzp_vp.vasm")).read()

    with tempfile.TemporaryDirectory() as tmp:
        parts = []
        parts.append(c_array("nzp_vp_vpo", assemble(cgcomp, tmp, "nzp_vp", nzp_vp, True)))
        parts.append(c_array("present_vp_vpo", assemble(cgcomp, tmp, "present_vp", PRESENT_VP, True)))
        parts.append(c_array("present_fp_fpo", assemble(cgcomp, tmp, "present_fp", PRESENT_FP, False)))
        names = []
        for u2 in range(U2):
            for u1 in range(U1):
                for u0 in range(U0):
                    name = "ps3gl_fp_%d_%d_%d" % (u0, u1, u2)
                    names.append(name + "_fpo")
                    parts.append(c_array(name + "_fpo",
                                         assemble(cgcomp, tmp, name, texenv_fp(u0, u1, u2), False)))

    with open(out_h, "w") as f:
        f.write("/* ps3gl_shader_data.h -- GENERATED by tools/build_shaders.py, don't edit. */\n")
        f.write("#ifndef PS3GL_SHADER_DATA_H\n#define PS3GL_SHADER_DATA_H\n\n")
        f.write("\n\n".join(parts))
        f.write("\n\n#define PS3GL_NUM_FP %d\n" % len(names))
        f.write("static const unsigned char *const ps3gl_fp_data[PS3GL_NUM_FP] = {\n")
        for n in names:
            f.write("\t%s,\n" % n)
        f.write("};\n\n#endif\n")


if __name__ == "__main__":
    main()
