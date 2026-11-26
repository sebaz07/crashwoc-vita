#!/usr/bin/env python3
"""Generate the GX recorder boilerplate (port/src/gx_rec_gen.h).

The GX thread (port/src/gx_rec.c) moves the GX emulation off the game
thread: the public GX functions become wrappers that, while the GX thread
runs, append a record to a ring buffer; the GX thread replays the records
with the original implementations (renamed gxi_<name> in gx_gl.c). This
script writes, from the table below, the name remapping used inside
gx_gl.c, the record structs, the wrappers and the replay cases. Pointer
arguments that the implementation dereferences at once are copied into the
record (mat34, mat44, texobj, lightobj); 'ptr' arguments are kept as is.

Usage: tools/gen_gx_rec.py > src/gx_rec_gen.h
"""

# (name, [(type, arg)]); types: int u8 u16 u32 f32 GXColor ptr mat34 mat44 texobj lightobj
FUNCS = [
    ("GXSetArray", [("ptr", "base_unused"), ]),  # placeholder, replaced below
    ("GXInvalidateVtxCache", []),
    ("GXLoadTexObj", [("texobj", "obj"), ("int", "map")]),
    ("GXInvalidateTexAll", []),
    ("GXLoadPosMtxImm", [("mat34", "m"), ("u32", "id")]),
    ("GXLoadNrmMtxImm", [("mat34", "m"), ("u32", "id")]),
    ("GXLoadTexMtxImm", [("mat34", "m"), ("u32", "id"), ("int", "type")]),
    ("GXSetCurrentMtx", [("u32", "id")]),
    ("GXSetProjection", [("mat44", "m"), ("int", "type")]),
    ("GXSetNumChans", [("u8", "n")]),
    ("GXSetChanCtrl", [("int", "chan"), ("u8", "enable"), ("int", "amb_src"), ("int", "mat_src"), ("u32", "light_mask"),
                       ("int", "diff_fn"), ("int", "attn_fn")]),
    ("GXSetChanAmbColor", [("int", "chan"), ("GXColor", "c")]),
    ("GXSetChanMatColor", [("int", "chan"), ("GXColor", "c")]),
    ("GXLoadLightObjImm", [("lightobj", "obj"), ("u32", "light")]),
    ("GXSetNumTevStages", [("u8", "n")]),
    ("GXSetNumTexGens", [("u8", "n")]),
    ("GXSetTexCoordGen2", [("int", "dst"), ("int", "func"), ("int", "src"), ("u32", "mtx"), ("u8", "normalize"),
                           ("u32", "postmtx")]),
    ("GXSetTevOrder", [("int", "stage"), ("int", "coord"), ("int", "map"), ("int", "color")]),
    ("GXSetTevColorIn", [("int", "stage"), ("int", "a"), ("int", "b"), ("int", "c"), ("int", "d")]),
    ("GXSetTevAlphaIn", [("int", "stage"), ("int", "a"), ("int", "b"), ("int", "c"), ("int", "d")]),
    ("GXSetTevColorOp", [("int", "stage"), ("int", "op"), ("int", "bias"), ("int", "scale"), ("int", "clamp"),
                         ("int", "reg")]),
    ("GXSetTevAlphaOp", [("int", "stage"), ("int", "op"), ("int", "bias"), ("int", "scale"), ("int", "clamp"),
                         ("int", "reg")]),
    ("GXSetTevOp", [("int", "stage"), ("int", "mode")]),
    ("GXSetTevColor", [("int", "reg"), ("GXColor", "c")]),
    ("GXSetAlphaCompare", [("int", "comp0"), ("u8", "ref0"), ("int", "op"), ("int", "comp1"), ("u8", "ref1")]),
    ("GXSetFog", [("int", "type"), ("f32", "startz"), ("f32", "endz"), ("f32", "nearz"), ("f32", "farz"),
                  ("GXColor", "color")]),
    ("GXSetBlendMode", [("int", "type"), ("int", "src"), ("int", "dst"), ("int", "op")]),
    ("GXSetZMode", [("u8", "enable"), ("int", "func"), ("u8", "update")]),
    ("GXSetZCompLoc", [("u8", "before_tex")]),
    ("GXSetAlphaUpdate", [("u8", "enable")]),
    ("GXSetColorUpdate", [("u8", "enable")]),
    ("GXSetCullMode", [("int", "mode")]),
    ("GXSetClipMode", [("int", "mode")]),
    ("GXSetDither", [("u8", "d")]),
    ("GXSetViewport", [("f32", "left"), ("f32", "top"), ("f32", "wd"), ("f32", "ht"), ("f32", "nearz"),
                       ("f32", "farz")]),
    ("GXSetScissor", [("u32", "left"), ("u32", "top"), ("u32", "wd"), ("u32", "ht")]),
    ("GXSetCopyClear", [("GXColor", "color"), ("u32", "z")]),
    ("GXPixModeSync", []),
    ("GXDrawDone", []),
    ("GXFlush", []),
    ("GXCopyDisp", [("ptr", "dest"), ("u8", "clear")]),
    ("GXSetTexCopySrc", [("u16", "left"), ("u16", "top"), ("u16", "wd"), ("u16", "ht")]),
    ("GXSetTexCopyDst", [("u16", "wd"), ("u16", "ht"), ("int", "fmt"), ("u8", "mipmap")]),
    ("GXCopyTex", [("ptr", "dest"), ("u8", "clear")]),
    ("port_gx_set_drawable", [("int", "win_w"), ("int", "win_h"), ("int", "off_x"), ("int", "off_y"),
                              ("int", "draw_w"), ("int", "draw_h")]),
    ("port_gx_flush", []),
    ("port_gx_begin_frame", []),
]
FUNCS[0] = ("GXSetArray", [("int", "attr"), ("ptr", "base"), ("u8", "stride")])

# functions with hand-written wrappers in gx_rec.c (renamed too)
SPECIAL = ["GXClearVtxDesc", "GXSetVtxDesc", "GXSetVtxAttrFmt", "GXBegin", "port_gs_draw_indexed", "port_gs_draw_tskin", "port_gs_skin_draw",
           "port_gx_present", "port_gx_idle_for_load", "port_gx_finish"]

PARAM_C = {  # parameter declaration in the wrapper prototype
    "int": "int {}", "u8": "u8 {}", "u16": "u16 {}", "u32": "u32 {}", "f32": "f32 {}", "GXColor": "GXColor {}",
    "ptr": "void *{}", "mat34": "const float {}[3][4]", "mat44": "const float {}[4][4]", "texobj": "void *{}",
    "lightobj": "void *{}",
}
FIELD_C = {  # field in the record struct
    "int": "int {};", "u8": "u8 {};", "u16": "u16 {};", "u32": "u32 {};", "f32": "f32 {};", "GXColor": "GXColor {};",
    "ptr": "void *{};", "mat34": "float {}[3][4];", "mat44": "float {}[4][4];", "texobj": "PortTexObj {};",
    "lightobj": "PortLightObj {};",
}
STORE_C = {  # wrapper: record field <- argument
    "mat34": "memcpy(r->{0}, {0}, sizeof(r->{0}));", "mat44": "memcpy(r->{0}, {0}, sizeof(r->{0}));",
    "texobj": "memcpy(&r->{0}, {0}, sizeof(r->{0}));", "lightobj": "memcpy(&r->{0}, {0}, sizeof(r->{0}));",
}
# state setters whose state only that one function sets: a call repeating the
# previous recorded call of the same function changes nothing and is dropped
# while recording (the shadow is reset when a recorded pass starts). The
# engine sets the same lighting/material state for every model (castle: 1700
# of 5100 records a frame). Not the TEV stage setters (GXSetTevOp overlaps
# GXSetTevColorIn/Op...) nor loads (textures, matrices, lights).
DEDUP = ["GXSetCurrentMtx", "GXSetNumChans", "GXSetChanCtrl", "GXSetChanAmbColor", "GXSetChanMatColor",
         "GXSetNumTevStages", "GXSetNumTexGens", "GXSetAlphaCompare", "GXSetFog", "GXSetBlendMode", "GXSetZMode",
         "GXSetCullMode"]

ARG_C = {  # replay: argument <- record field
    "texobj": "&r->{}", "lightobj": "&r->{}",
}


def main():
    out = ["/* generated by port/tools/gen_gx_rec.py: do not edit */", "#ifdef GX_REC_NAMES",
           "/* inside gx_gl.c the implementations are gxi_<name> */"]
    for name, _ in FUNCS:
        out.append("#define %s gxi_%s" % (name, name))
    for name in SPECIAL:
        out.append("#define %s gxi_%s" % (name, name))
    out.append("#endif /* GX_REC_NAMES */")
    out += ["", "#ifdef GX_REC_BODY"]
    for name, _ in FUNCS:
        out.append("#undef %s" % name)
    for name in SPECIAL:
        out.append("#undef %s" % name)
    out.append("enum {")
    out.append("    RECOP_PAD = 0, RECOP_BLOCK, RECOP_DRAW_INDEXED, RECOP_DRAW_TSKIN, RECOP_SKIN_DRAW, RECOP_PRESENT,")
    out.append("    RECOP_IDLE_FOR_LOAD, RECOP_FINISH, RECOP_SYNC, RECOP_GXClearVtxDesc, RECOP_GXSetVtxDesc,")
    out.append("    RECOP_GXSetVtxAttrFmt, RECOP_QUADS, RECOP_HUD_BEGIN,")
    for name, _ in FUNCS:
        out.append("    RECOP_%s," % name)
    out.append("    RECOP_N")
    out.append("};")
    for name, params in FUNCS:
        if params:
            out.append("typedef struct { %s } Rec_%s;" % (" ".join(FIELD_C[t].format(a) for t, a in params), name))
    for i, name in enumerate(DEDUP):
        out.append("static Rec_%s g_rdd_%s; /* last recorded call (bit %d of g_rec_dd) */" % (name, name, i))
    for name, params in FUNCS:
        proto = ", ".join(PARAM_C[t].format(a) for t, a in params) or "void"
        call = ", ".join(a for _, a in params)
        out.append("void %s(%s) {" % (name, proto))
        out.append("    if (!g_rec_active) {")
        out.append("        gxi_%s(%s);" % (name, call))
        out.append("        return;")
        out.append("    }")
        if name in DEDUP:
            bit = DEDUP.index(name)
            out.append("    {")
            out.append("        Rec_%s t, *r = &t;" % name)
            out.append("        memset(&t, 0, sizeof(t));")
            for t, a in params:
                out.append("        " + STORE_C.get(t, "r->{0} = {0};").format(a))
            out.append("        if ((g_rec_dd & (1u << %d)) && !memcmp(&t, &g_rdd_%s, sizeof(t))) {" % (bit, name))
            out.append("            g_rec_ddn++;")
            out.append("            return;")
            out.append("        }")
            out.append("        g_rdd_%s = t;" % name)
            out.append("        g_rec_dd |= 1u << %d;" % bit)
            out.append("        r = rec_alloc(RECOP_%s, sizeof(Rec_%s));" % (name, name))
            out.append("        *r = t;")
            out.append("    }")
            out.append("    rec_commit();")
            out.append("}")
            continue
        if params:
            out.append("    {")
            out.append("        Rec_%s *r = rec_alloc(RECOP_%s, sizeof(Rec_%s));" % (name, name, name))
            for t, a in params:
                out.append("        " + STORE_C.get(t, "r->{0} = {0};").format(a))
            out.append("    }")
        else:
            out.append("    rec_alloc(RECOP_%s, 0);" % name)
        out.append("    rec_commit();")
        out.append("}")
    out.append("static void rec_replay_generated(int op, const void *p) {")
    out.append("    switch (op) {")
    for name, params in FUNCS:
        if params:
            args = ", ".join(ARG_C.get(t, "r->{}").format(a) for t, a in params)
            out.append("    case RECOP_%s: { Rec_%s *r = (Rec_%s *)p; gxi_%s(%s); break; }" % (name, name, name, name, args))
        else:
            out.append("    case RECOP_%s: (void)p; gxi_%s(); break;" % (name, name))
    out.append("    default: port_log(\"gxrec: unknown record %d\", op); break;")
    out.append("    }")
    out.append("}")
    out.append("#endif /* GX_REC_BODY */")
    print("\n".join(out))


if __name__ == "__main__":
    main()
