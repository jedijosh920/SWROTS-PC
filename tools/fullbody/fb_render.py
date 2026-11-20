#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Full-body select pictures (s_selduel_fb_<name>_v1/_v2) rendered from a fighter's own model, in Blender (headless).

    blender -b --factory-startup --python fb_render.py -- job.json      then  python fb_stx.py ...

The input comes from the user's own copy of the game (the model, its textures, its hilts and its animation clips,
unpacked from the level PAKs, see docs/modding/dumping-assets.md); nothing of the game is in this repository. The
models are read by a Blender importer for the game's .msh files ("importer"); it is not part of this repository.
Every path of a job is taken from the job file's own folder unless it is absolute. yoda.json, the job for Yoda's
two pictures, expects next to it: io_import_swe3_msh.py (the importer), yoda/yoda/ (yoda.msh and its textures, from
meshes\\chars\\yoda of res_u113_cor_yoda.pak), yoda/lsaberyoda/ (his hilt, from meshes\\weapons) and yoda/anim/
(his clips, from animation\\yoda: yoda_nav_stalkf.bnm, yoda_nav_run_out.bnm).

job.json: {"mode": "probe" | "final", "out": dir, "work": dir, "importer": path, "stx": path (optional),
           "chars": [{"tag", "mesh": dir (mods\\meshes\\chars\\<folder>), "duel": bool,
                      "hilts": [[dir, bone], ...], "clips": [bnm path, ...],
                      "saber": [r, g, b], "pose": [clip stem, fraction], "yaw": deg,
                      "blade": [base, length] (optional, game units; default 4.273, 45),
                      "dark": brightness (optional: the dark side look, as Sith Yoda's),
                      "turns": [deg, ...] (optional), "fit": bool (optional), "center": "body" (optional),
                      "frame_units": the picture's height in game units (optional; 125 gives the game's own
                      pictures' scale, checked on Anakin's), "feet": share (optional; 0.19: the game's feet line),
                      "edits": [{"edit": "turn" | "hang" | "point" | "aim" | "grip", ...}] (optional, see
                      edit_pose)}]}
probe: per character a sheet, a row per clip, frames 0..1 in 6 steps (Workbench).
final: per character its picture at "pose", 128x256 (rendered at 4x), transparent, and its blades alone (_glow),
which fb_stx.py turns into the picture's STX.
"""
import importlib.util
import json
import math
import os
import shutil
import struct
import sys
import traceback

import bpy
import numpy as np
from mathutils import Matrix, Vector

JOB = sys.argv[sys.argv.index("--") + 1]
ARGS = json.load(open(JOB))
BASE = os.path.dirname(os.path.abspath(JOB))


def at(path):
    """A path of the job: as given if absolute, else from the job file's own folder."""
    return path if os.path.isabs(path) else os.path.normpath(os.path.join(BASE, path))


for _key in ("out", "work", "importer", "stx"):
    if _key in ARGS:
        ARGS[_key] = at(ARGS[_key])
for _c in ARGS["chars"]:
    _c["mesh"] = at(_c["mesh"])
    _c["hilts"] = [[at(d), bone] for d, bone in _c.get("hilts", [])]
    _c["clips"] = [at(clip) for clip in _c.get("clips", [])]
    if "candidates" in _c:
        _c["candidates"] = [[at(clip), frac] for clip, frac in _c["candidates"]]
os.makedirs(ARGS["out"], exist_ok=True)
LOG = open(os.path.join(ARGS["out"], "fb_render.log"), "a", encoding="utf-8")


def log(*a):
    s = " ".join(str(x) for x in a)
    print(s)
    LOG.write(s + "\n")
    LOG.flush()


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


imp = load("io_import_swe3_msh", ARGS["importer"])
stx = load("stx_decode", ARGS["stx"]) if ARGS.get("stx") else None  # only for models whose textures are .stx
imp._log = lambda m: log("  [imp]", m)

SCALE = 0.05
OPTS = {"scale": SCALE, "y_up": True, "mirror_x": True, "import_skeleton": True, "import_anims": True,
        "anim_limit": 0, "anim_layout": "STASH", "origin_bone": True, "root_motion": False,
        "clip_markers": False, "anim_conjugate": True}
STATIC = dict(OPTS, import_skeleton=False, import_anims=False)
BLADE_BASE, BLADE_LEN = 4.273, 45.0  # game units along the hilt's +y
S = Matrix.Diagonal((-1.0, 1.0, 1.0))
LOCAL = {}  # each hilt part's matrix in its hand bone's frame


def write_tga(path, rgba):
    h, w = rgba.shape[:2]
    with open(path, "wb") as f:
        f.write(struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, w, h, 32, 0x28))
        f.write(rgba[:, :, [2, 1, 0, 3]].tobytes())


def stage_folder(src, dst, duel):
    """The .msh and the textures (.stx decoded to .tga; for player 2 its _duel ones under the plain names)."""
    os.makedirs(dst, exist_ok=True)
    files = os.listdir(src)
    low = {f.lower(): f for f in files}
    for f in files:
        stem, ext = os.path.splitext(f)
        ext = ext.lower()
        if ext in (".msh", ".tga", ".png"):
            shutil.copy(os.path.join(src, f), os.path.join(dst, f))
        elif ext == ".stx":
            if stem.lower().endswith("_duel"):
                continue
            use = f
            if duel and (stem + "_duel.stx").lower() in low:
                use = low[(stem + "_duel.stx").lower()]
            rgba = stx.decode(open(os.path.join(src, use), "rb").read())
            write_tga(os.path.join(dst, stem + ".tga"), rgba)


def rest_world(msh_path):
    """{bone name: its rest matrix in the armature's space (mirrored, scaled)}, as the importer builds it."""
    parsed = imp.parse_msh(open(msh_path, "rb").read())
    out = {}
    for name, _p, m12 in parsed["bones"]:
        m = imp.bind_matrix(m12)
        m2 = (S @ m.to_3x3() @ S).to_4x4()
        m2.translation = (S @ m.translation) * SCALE
        out[name] = m2
    return out


def emission(name, rgb, strength):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    em = nt.nodes.new("ShaderNodeEmission")
    em.inputs[0].default_value = (*rgb, 1.0)
    em.inputs[1].default_value = strength
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    nt.links.new(em.outputs[0], out.inputs[0])
    mat.diffuse_color = (*rgb, 1.0)
    return mat


def make_blade(rgb):
    """A blade along +y from the emitter, as an object to place with its hilt: a white core in a coloured sheath."""
    objs = []
    for name, radius, col, strength in (("core", 0.018, (1, 1, 1), 6.0), ("glow", 0.032, rgb, 12.0)):
        bpy.ops.mesh.primitive_cylinder_add(vertices=16, radius=radius, depth=BLADE_LEN * SCALE)
        o = bpy.context.active_object
        o.name = "blade_" + name
        me = o.data
        # along +y, starting at the emitter
        me.transform(Matrix.Translation((0, (BLADE_BASE + BLADE_LEN / 2) * SCALE, 0)) @ Matrix.Rotation(math.radians(-90), 4, "X"))
        me.materials.append(emission("blade_" + name, col, strength))
        if name == "glow":
            o.visible_shadow = False
        objs.append(o)
    return objs


def opaque(o):
    """The game draws a body opaque: its textures' alpha is not transparency."""
    for slot in o.material_slots:
        mat = slot.material
        if not mat:
            continue
        mat.use_backface_culling = False
        for attr, value in (("blend_method", "OPAQUE"), ("surface_render_method", "DITHERED")):
            try:
                setattr(mat, attr, value)
            except Exception:
                pass
        if mat.node_tree:
            for n in mat.node_tree.nodes:
                if "Alpha" in n.inputs:
                    for l in list(n.inputs["Alpha"].links):
                        mat.node_tree.links.remove(l)
                    n.inputs["Alpha"].default_value = 1.0
            for n in mat.node_tree.nodes:  # a mix with a transparent shader (alpha cut)
                if n.type == "BSDF_TRANSPARENT":
                    for l in list(n.outputs[0].links):
                        mat.node_tree.links.remove(l)


def grip(rest, wrist):
    """The hilt in the hand: its axis (+y) across the fist toward the thumb (the knuckles' line, index side), its
    hand point in the fist (between the knuckles and the wrist, toward the palm), in the wrist bone's frame."""
    side = wrist.split("_")[1]
    inv = rest[wrist].inverted()
    pos = lambda b: (inv @ rest["%s_%s_g" % (b, side)]).translation
    index, middle, thumb = pos("index1"), pos("middle1"), pos("thumb1")
    axis = (index - middle).normalized()
    knuckles = (index + middle) / 2
    fingers = knuckles.normalized()
    fingers = (fingers - axis * fingers.dot(axis)).normalized()
    palm = axis.cross(fingers).normalized()
    if palm.dot(thumb - knuckles) < 0:
        palm = -palm
    centre = knuckles * 0.72 + palm * (1.5 * SCALE)
    # hilt x, y, z -> fingers, axis, x cross y
    z = fingers.cross(axis)
    rot = Matrix((fingers, axis, z)).transposed().to_4x4()
    return Matrix.Translation(centre) @ rot


def darken_folder(folder, level):
    """The dark side look the port gives Sith Yoda (DarkColour, src/game/roster.cpp) on the staged textures (the copies
    in the work folder, before the import): nearly grey, darkened and cooled; `level` its brightness (the port's 0.22
    reads as black in a picture: 0.45)."""
    for f in sorted(os.listdir(folder)):
        if not f.lower().endswith(".tga"):
            continue
        path = os.path.join(folder, f)
        img = bpy.data.images.load(path)
        px = np.array(img.pixels[:], dtype=np.float32).reshape(-1, 4)
        rgb = px[:, :3]
        grey = (rgb * np.array([0.299, 0.587, 0.114], np.float32)).sum(1, keepdims=True)
        px[:, :3] = np.clip((rgb + (grey - rgb) * 0.95) * level * np.array([0.95, 0.92, 1.08], np.float32), 0, 1)
        img.pixels[:] = px.ravel()
        img.filepath_raw = path
        img.file_format = "TARGA"
        img.save()
        bpy.data.images.remove(img)
        log(" dark", f)


def import_character(c, work):
    global BLADE_BASE, BLADE_LEN
    BLADE_BASE, BLADE_LEN = c.get("blade", (4.273, 45.0))  # its hilt's ATT_saber_base and its blade's length
    bpy.ops.wm.read_factory_settings(use_empty=True)
    root = os.path.join(work, c["tag"])
    if os.path.isdir(root):
        shutil.rmtree(root)
    folder = os.path.basename(c["mesh"].rstrip("\\/"))
    dst = os.path.join(root, "meshes", "chars", folder)
    stage_folder(c["mesh"], dst, c.get("duel", False))
    if c.get("dark"):
        darken_folder(dst, c["dark"])
    anim = os.path.join(root, "animation", "clips")
    os.makedirs(anim, exist_ok=True)
    for clip in c["clips"]:
        shutil.copy(clip, anim)
    msh = os.path.join(dst, folder + ".msh")
    objs, msg = imp.import_msh(bpy.context, msh, OPTS)
    log("import", c["tag"], msg)
    meshes = [o for o in objs if o.type == "MESH"]
    for o in meshes:
        opaque(o)
    for img in bpy.data.images:  # the textures' alpha is the game's, not transparency
        img.alpha_mode = "NONE"
    arm = next((m.object for o in meshes for m in o.modifiers if m.type == "ARMATURE" and m.object), None)
    rest = rest_world(msh)
    hilts = []
    for k, (hdir, bone) in enumerate(c.get("hilts", [])):
        hname = os.path.basename(hdir.rstrip("\\/"))
        hdst = os.path.join(root, "meshes", "weapons", hname + ("" if k == 0 else "_%d" % k))
        stage_folder(hdir, hdst, False)
        hobjs, hmsg = imp.import_msh(bpy.context, os.path.join(hdst, hname + ".msh"), STATIC)
        log("hilt", hname, hmsg)
        parts = [o for o in hobjs if o.type == "MESH"] + make_blade(c["saber"])
        local = grip(rest, bone)
        for o in parts:
            LOCAL[o.name] = local
        hilts.append((parts, bone))
    for img in bpy.data.images:
        img.alpha_mode = "NONE"
    return arm, meshes, rest, hilts


def clip_action(arm, stem):
    names = [a for a in bpy.data.actions]
    for a in names:
        if stem.lower() in a.name.lower():
            return a
    log("no action", stem, "among", [a.name for a in names][:20])
    return None


def set_clip(arm, act):
    ad = arm.animation_data
    for t in ad.nla_tracks:
        t.mute = True
    ad.action = None
    imp.assign_action(ad, act, arm)
    return act.frame_range


def place_hilts(arm, rest, hilts):
    bpy.context.view_layer.update()
    for parts, bone in hilts:
        pb = arm.pose.bones.get(bone)
        if pb is None or bone not in rest:
            log("no bone", bone)
            continue
        m = arm.matrix_world @ pb.matrix @ pb.bone.matrix_local.inverted() @ rest[bone]
        for o in parts:
            o.matrix_world = m @ LOCAL.get(o.name, Matrix.Identity(4))


def bounds(objs):
    dg = bpy.context.evaluated_depsgraph_get()
    pts = []
    for o in objs:
        ev = o.evaluated_get(dg)
        me = ev.to_mesh()
        mw = ev.matrix_world
        pts += [mw @ v.co for v in me.vertices]
        ev.to_mesh_clear()
    lo = Vector([min(p[i] for p in pts) for i in range(3)])
    hi = Vector([max(p[i] for p in pts) for i in range(3)])
    return lo, hi


def camera(yaw):
    cam = bpy.data.cameras.new("cam")
    cam.type = "ORTHO"
    co = bpy.data.objects.new("cam", cam)
    bpy.context.scene.collection.objects.link(co)
    bpy.context.scene.camera = co
    co["yaw"] = yaw
    return co


def aim(co, objs, fill=0.9):
    """The camera at the yaw, looking level at the objects, fitting them in a 1:2 frame."""
    yaw = math.radians(co["yaw"])
    lo, hi = bounds(objs)
    c = (lo + hi) / 2
    d = 20.0
    co.location = c + Vector((math.sin(yaw) * d, -math.cos(yaw) * d, 0))
    co.rotation_euler = (math.radians(90), 0, yaw)
    # extent across the view
    right = Vector((math.cos(yaw), math.sin(yaw), 0))
    pts = [Vector((x, y, z)) for x in (lo.x, hi.x) for y in (lo.y, hi.y) for z in (lo.z, hi.z)]
    w = max(p.dot(right) for p in pts) - min(p.dot(right) for p in pts)
    h = hi.z - lo.z
    co.data.ortho_scale = max(h, w * 2) / fill  # ortho scale spans the larger side (the height)


def workbench(w, h):
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_WORKBENCH"
    sc.display.shading.light = "STUDIO"
    sc.display.shading.color_type = "TEXTURE"
    sc.display.shading.show_backface_culling = False
    sc.render.resolution_x, sc.render.resolution_y = w, h
    sc.render.film_transparent = True
    sc.render.image_settings.file_format = "PNG"
    sc.render.image_settings.color_mode = "RGBA"


def render_to(path):
    bpy.context.scene.render.filepath = path
    bpy.ops.render.render(write_still=True)


def read_png(path):
    img = bpy.data.images.load(path)
    w, h = img.size
    a = np.array(img.pixels[:], dtype=np.float32).reshape(h, w, 4)[::-1]
    bpy.data.images.remove(img)
    return a


def save_png(path, a):
    h, w = a.shape[:2]
    img = bpy.data.images.new("sheet", w, h, alpha=True)
    img.pixels = a[::-1].ravel().tolist()
    img.filepath_raw = path
    img.file_format = "PNG"
    img.save()


def probe(c):
    arm, meshes, rest, hilts = import_character(c, ARGS["work"])
    co = camera(c.get("yaw", 0))
    W, H = 160, 320
    workbench(W, H)
    tmp = os.path.join(ARGS["out"], "tmp.png")
    rows = []
    if c.get("rest"):
        arm.data.pose_position = "REST"
        row = []
        for yaw in (0, 90, 180, 270, 45, 315):
            co["yaw"] = yaw
            aim(co, meshes, 0.92)
            render_to(tmp)
            row.append(read_png(tmp))
        rows.append(np.concatenate(row, 1))
        arm.data.pose_position = "POSE"
        co["yaw"] = c.get("yaw", 0)
    for clip in c["clips"]:
        stem = os.path.splitext(os.path.basename(clip))[0]
        act = clip_action(arm, stem)
        row = []
        if act:
            lo, hi = set_clip(arm, act)
            log(" clip", stem, "frames", lo, hi)
            for k in range(6):
                bpy.context.scene.frame_set(int(round(lo + (hi - lo) * k / 5)))
                place_hilts(arm, rest, hilts)
                aim(co, meshes + [o for parts, _ in hilts for o in parts], 0.92)
                render_to(tmp)
                row.append(read_png(tmp))
        while len(row) < 6:
            row.append(np.zeros((H, W, 4), np.float32))
        rows.append(np.concatenate(row, 1))
    sheet = np.concatenate(rows, 0)
    bg = np.array([0.16, 0.2, 0.31, 1.0], np.float32)
    sheet = sheet[..., :3] * sheet[..., 3:] + bg[:3] * (1 - sheet[..., 3:])
    sheet = np.dstack([sheet, np.ones(sheet.shape[:2], np.float32)])
    save_png(os.path.join(ARGS["out"], "probe_%s.png" % c["tag"]), sheet)


def handprobe(c):
    """Close-ups of the hand holding the hilt: per clip (first frame), three views."""
    arm, meshes, rest, hilts = import_character(c, ARGS["work"])
    co = camera(0)
    W, H = 240, 240
    workbench(W, H)
    tmp = os.path.join(ARGS["out"], "tmp.png")
    rows = []
    bone = c["hilts"][0][1]
    for clip in c["clips"]:
        stem = os.path.splitext(os.path.basename(clip))[0]
        act = clip_action(arm, stem)
        row = []
        if act:
            lo, hi = set_clip(arm, act)
            bpy.context.scene.frame_set(int(lo))
            place_hilts(arm, rest, hilts)
            bpy.context.view_layer.update()
            pb = arm.pose.bones[bone]
            centre = arm.matrix_world @ pb.head
            for yaw in (0, 90, 180, 270):
                y = math.radians(yaw)
                co.location = centre + Vector((math.sin(y) * 10, -math.cos(y) * 10, 0))
                co.rotation_euler = (math.radians(90), 0, y)
                co.data.ortho_scale = 0.9
                render_to(tmp)
                row.append(read_png(tmp))
        while len(row) < 4:
            row.append(np.zeros((H, W, 4), np.float32))
        rows.append(np.concatenate(row, 1))
    sheet = np.concatenate(rows, 0)
    bg = np.array([0.16, 0.2, 0.31, 1.0], np.float32)
    sheet = sheet[..., :3] * sheet[..., 3:] + bg[:3] * (1 - sheet[..., 3:])
    sheet = np.dstack([sheet, np.ones(sheet.shape[:2], np.float32)])
    save_png(os.path.join(ARGS["out"], "hand_%s.png" % c["tag"]), sheet)


def lights():
    sc = bpy.context.scene
    world = bpy.data.worlds.new("w")
    world.use_nodes = True
    bg = world.node_tree.nodes.get("Background")
    bg.inputs[0].default_value = (0.05, 0.055, 0.07, 1)
    bg.inputs[1].default_value = 1.0
    sc.world = world
    for name, rot, strength, colour in (
            ("key", (55, 0, -35), 3.2, (1.0, 0.97, 0.92)),
            ("fill", (70, 0, 40), 1.2, (0.85, 0.9, 1.0)),
            ("rim_l", (75, 0, -150), 4.0, (0.75, 0.85, 1.0)),
            ("rim_r", (75, 0, 150), 3.0, (0.75, 0.85, 1.0)),
            ("top", (0, 0, 0), 0.8, (1, 1, 1))):
        ld = bpy.data.lights.new(name, "SUN")
        ld.energy = strength
        ld.color = colour
        ld.angle = math.radians(8)
        lo = bpy.data.objects.new(name, ld)
        lo.rotation_euler = [math.radians(a) for a in rot]
        sc.collection.objects.link(lo)
        LIGHTS.append(lo)


def cycles(w, h):
    sc = bpy.context.scene
    sc.render.engine = "CYCLES"
    try:
        prefs = bpy.context.preferences.addons["cycles"].preferences
        for kind in ("OPTIX", "CUDA"):
            try:
                prefs.compute_device_type = kind
                prefs.get_devices()
                if any(d.type == kind for d in prefs.devices):
                    for d in prefs.devices:
                        d.use = True
                    sc.cycles.device = "GPU"
                    log(" cycles on", kind)
                    break
            except Exception:
                continue
    except Exception as e:
        log(" no gpu", e)
    sc.cycles.samples = 96
    try:
        sc.cycles.use_denoising = True
    except Exception:
        pass
    sc.render.resolution_x, sc.render.resolution_y = w, h
    sc.render.film_transparent = True
    sc.view_settings.view_transform = "Standard"
    sc.view_settings.exposure = 0.3
    sc.render.image_settings.file_format = "PNG"
    sc.render.image_settings.color_mode = "RGBA"


LIGHTS = []
CHOSEN = {}  # each fighter's player 1 pose


def holdout(objs):
    mat = bpy.data.materials.new("holdout")
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    h = nt.nodes.new("ShaderNodeHoldout")
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    nt.links.new(h.outputs[0], out.inputs[0])
    for o in objs:
        o.data.materials.clear()
        o.data.materials.append(mat)


def points(objs):
    dg = bpy.context.evaluated_depsgraph_get()
    pts = []
    for o in objs:
        ev = o.evaluated_get(dg)
        me = ev.to_mesh()
        mw = ev.matrix_world
        pts += [mw @ v.co for v in me.vertices]
        ev.to_mesh_clear()
    return pts


def facing(arm):
    """Where the fighter looks, flat: its head's and chest's forward (their rest forward is -y, toward a camera at
    yaw 0) turned as the pose turns them."""
    bpy.context.view_layer.update()
    f = Vector()
    for name, weight in (("head_g", 0.6), ("spine3_g", 0.4)):
        pb = arm.pose.bones.get(name)
        if not pb:
            continue
        rest = (arm.matrix_world @ pb.bone.matrix_local).to_3x3()
        now = (arm.matrix_world @ pb.matrix).to_3x3()
        d = now @ rest.inverted() @ Vector((0, -1, 0))
        d.z = 0
        if d.length > 1e-6:
            f += d.normalized() * weight
    return f.normalized() if f.length > 1e-6 else Vector((0, -1, 0))


def frame_body(co, arm, meshes, c, blades=()):
    """The camera turned so that the fighter looks toward the middle of the screen (player 1, on the left, to its
    right; player 2 to its left), at the same scale for every fighter (its standing height a fixed share of the
    picture), its feet near the bottom; the view widened, if need be, to keep its blades in the frame."""
    def heads(names, rest):
        pose = arm.data.pose_position
        arm.data.pose_position = "REST" if rest else "POSE"
        bpy.context.view_layer.update()
        out = {n: arm.matrix_world @ arm.pose.bones[n].head for n in names if arm.pose.bones.get(n)}
        arm.data.pose_position = pose
        bpy.context.view_layer.update()
        return out
    feet = ("ankle_L_g", "ankle_R_g", "toe_L_g", "toe_R_g")
    rest = heads(("head_g",) + feet, True)
    floor = min(rest[n].z for n in feet if n in rest)
    height = (rest["head_g"].z - floor) * 1.13
    base = height / c.get("height", 0.66)
    if c.get("frame_units"):  # the picture's height in game units: one scale for every model, as the game's pictures
        base = c["frame_units"] * SCALE
    now = heads(("head_g", "root_pelvis_g", "spine2_g") + feet, False)
    low = min(now[n].z for n in feet if n in now) - 0.04 * height
    # the yaw: the fighter's facing at `turn` degrees from the camera, toward the screen's middle
    f = facing(arm)
    turn = math.radians(c.get("turn", 30))
    f0 = Vector((math.sin(turn), -math.cos(turn), 0))
    yaw = math.atan2(f.y, f.x) - math.atan2(f0.y, f0.x)
    if c.get("yaw_kept") is not None:  # the yaw found before the pose's edits (a body tilted keeps its camera)
        yaw = math.radians(c["yaw_kept"])
    co["yaw"] = math.degrees(yaw)
    right = Vector((math.cos(yaw), math.sin(yaw), 0))
    # the body's points near its skeleton (some bodies keep stray hidden parts far away), and the blades'
    pelvis = now.get("root_pelvis_g", sum((now[n] for n in now), Vector()) / len(now))
    pts = [p for p in points(meshes) if (p - pelvis).length < 0.75 * height] + points(blades)
    xs = [p.dot(right) for p in pts]
    zs = [p.z for p in pts]
    m = 0.08 * height  # room for the glow
    feet_at = c.get("feet", 0.11)
    need = max(base, 2 * (max(xs) - min(xs) + 2 * m), (max(zs) + m - low) / (1 - feet_at),
               (low - min(zs) + m) / feet_at if min(zs) < low else 0)
    scale = base if c.get("fixed") else need
    centre = sum((now[n] for n in now), Vector()) / len(now)
    mid = (max(xs) + min(xs)) / 2
    if c.get("center") == "body":  # centred on the body alone, its blades free to reach out
        body = [p.dot(right) for p in pts[:len(pts) - len(points(blades))]] or xs
        mid = (max(body) + min(body)) / 2
    centre = centre + right * (mid - centre.dot(right))
    centre.z = low + scale * (0.5 - feet_at)
    d = 20.0
    co.location = centre + Vector((math.sin(yaw) * d, -math.cos(yaw) * d, 0))
    co.rotation_euler = (math.radians(90), 0, yaw)
    co.data.ortho_scale = scale
    log(" frame", c["tag"], "yaw %.0f" % co["yaw"], "need %.2f (base %.2f)" % (need, base))
    return need, base


def matte(meshes):
    """Less gloss than the importer's materials: the game's look, not polished plastic."""
    for o in meshes:
        for slot in o.material_slots:
            mat = slot.material
            if not mat or not mat.node_tree:
                continue
            for n in mat.node_tree.nodes:
                if n.type != "BSDF_PRINCIPLED":
                    continue
                for name, value in (("Roughness", 0.62), ("Specular IOR Level", 0.22), ("Metallic", 0.0),
                                    ("Coat Weight", 0.0)):
                    if name in n.inputs:
                        for l in list(n.inputs[name].links):
                            mat.node_tree.links.remove(l)
                        n.inputs[name].default_value = value


def view_dir(co, v):
    """[right, up, toward the camera] in the camera's view -> a world direction."""
    yaw = math.radians(co["yaw"])
    right = Vector((math.cos(yaw), math.sin(yaw), 0))
    toward = Vector((math.sin(yaw), -math.cos(yaw), 0))
    return (right * v[0] + Vector((0, 0, 1)) * v[1] + toward * v[2]).normalized()


def turn_bone(arm, name, frm, to):
    """The bone (and what hangs from it) turned about its head so that the world direction `frm` becomes `to`."""
    pb = arm.pose.bones.get(name)
    if pb is None:
        log("no bone", name)
        return
    m = arm.matrix_world.to_3x3().inverted()
    q = (m @ frm).normalized().rotation_difference((m @ to).normalized()).to_matrix().to_4x4()
    head = pb.matrix.translation.copy()
    pb.matrix = Matrix.Translation(head) @ q @ Matrix.Translation(-head) @ pb.matrix
    bpy.context.view_layer.update()


def world_head(arm, name):
    return arm.matrix_world @ arm.pose.bones[name].head


def edit_pose(co, arm, rest, hilts, edits):
    """Hand-made changes to the pose, in order (after the camera's yaw is known):
    hang:  {"bones": [cloth bones, parents first], "out": share, "back": share} a cape let fall;
    turn:  {"bone": e.g. the root, "from": [right, up, toward], "to": [...]} turns it in the view (a body tilted);
    point: {"bone": arm's root, "child": its hand, "from": a bone, "at": [right, up, toward] game units} turns the
           arm so that the hand goes toward that point (the bone's head moved by the offset in the view);
    aim:   {"bone": the hand, "dir": [right, up, toward]} turns the hand so that the blade points that way;
    grip:  {"bone": arm's root, "child": the other hand, "along": game units} the other hand onto the hilt."""
    unit = SCALE
    # the pose frozen: without its action, the bones keep the frame's values and the edits below (else the render,
    # evaluating the animation again, puts the bones back and leaves the hilts in the air)
    ad = arm.animation_data
    if ad:
        for t in ad.nla_tracks:
            t.mute = True
        ad.action = None
    bpy.context.view_layer.update()
    for e in edits:
        kind = e["edit"]
        if kind == "point":
            yaw = math.radians(co["yaw"])
            right = Vector((math.cos(yaw), math.sin(yaw), 0))
            toward = Vector((math.sin(yaw), -math.cos(yaw), 0))
            at = e["at"]
            target = world_head(arm, e["from"]) + (right * at[0] + Vector((0, 0, 1)) * at[1] + toward * at[2]) * unit
            root = world_head(arm, e["bone"])
            turn_bone(arm, e["bone"], world_head(arm, e["child"]) - root, target - root)
        elif kind == "turn":  # a bone (and all it carries) turned in the view: "from" -> "to", [right, up, toward]
            turn_bone(arm, e["bone"], view_dir(co, e["from"]), view_dir(co, e["to"]))
        elif kind == "hang":  # cloth bones (a cape) let fall: down, a little outward and behind, in the view
            yaw = math.radians(co["yaw"])
            right = Vector((math.cos(yaw), math.sin(yaw), 0))
            for name in e["bones"]:
                pb = arm.pose.bones.get(name)
                if pb is None:
                    log("no bone", name)
                    continue
                head, tail = arm.matrix_world @ pb.head, arm.matrix_world @ pb.tail
                side = 1 if (tail - head).dot(right) >= 0 else -1
                want = view_dir(co, [side * e.get("out", 0.35), -1.0, -e.get("back", 0.5)])
                turn_bone(arm, name, tail - head, want)
        elif kind == "aim":
            place_hilts(arm, rest, hilts)
            core = next(o for parts, _ in hilts for o in parts if o.name.startswith("blade_core"))
            now = core.matrix_world.to_3x3() @ Vector((0, 1, 0))
            turn_bone(arm, e["bone"], now, view_dir(co, e["dir"]))
        elif kind == "grip":
            place_hilts(arm, rest, hilts)
            core = next(o for parts, _ in hilts for o in parts if o.name.startswith("blade_core"))
            target = core.matrix_world @ Vector((0, e.get("along", -2.5) * unit, 0))
            root = world_head(arm, e["bone"])
            turn_bone(arm, e["bone"], world_head(arm, e["child"]) - root, target - root)
            log(" grip", e["child"], "off by %.3f" % (world_head(arm, e["child"]) - target).length)
        place_hilts(arm, rest, hilts)
    yaw = math.radians(co["yaw"])
    right = Vector((math.cos(yaw), math.sin(yaw), 0))
    for n in ("wrist_L_g", "wrist_R_g", "head_g", "spine2_g"):
        if arm.pose.bones.get(n):
            p = world_head(arm, n)
            log("  %s screen x %.3f z %.3f" % (n, p.dot(right), p.z))
    for parts, _ in hilts:
        for o in parts:
            if o.name.startswith("blade_core"):
                a, b = o.matrix_world @ Vector((0, 0, 0)), o.matrix_world @ Vector((0, BLADE_BASE * SCALE, 0))
                log("  hilt from x %.3f z %.3f to x %.3f z %.3f" % (a.dot(right), a.z, b.dot(right), b.z))
    # where the mesh's hands are (the vertices each wrist bone moves most), against their bones
    dg = bpy.context.evaluated_depsgraph_get()
    for o in bpy.context.scene.objects:
        if o.type != "MESH" or not o.vertex_groups:
            continue
        ev = o.evaluated_get(dg)
        me = ev.to_mesh()
        for n in ("wrist_L_g", "wrist_R_g"):
            g = o.vertex_groups.get(n)
            if g is None:
                continue
            pts = [ev.matrix_world @ me.vertices[v.index].co for v in o.data.vertices
                   for ge in v.groups if ge.group == g.index and ge.weight > 0.5]
            if pts:
                c = sum(pts, Vector()) / len(pts)
                log("  mesh %s %s: %d verts at x %.3f z %.3f" % (o.name, n, len(pts), c.dot(right), c.z))
        ev.to_mesh_clear()
    log(" edits", len(edits))


def final(c):
    """Its picture: the pose (clip, fraction of its length), the camera at the yaw; a render of everything and one of
    the blades alone (the body held out), for the glow added afterwards."""
    arm, meshes, rest, hilts = import_character(c, ARGS["work"])
    co = camera(c.get("yaw", 0))
    matte(meshes)
    lights()
    cycles(512, 1024)
    def pose(clip, frac):
        stem = os.path.splitext(os.path.basename(clip))[0]
        act = clip_action(arm, stem)
        if not act:
            return False
        lo, hi = set_clip(arm, act)
        bpy.context.scene.frame_set(int(round(lo + (hi - lo) * frac)))
        place_hilts(arm, rest, hilts)
        return True

    # the pose: the first of the candidates whose blades fit the frame at the common scale (else the one that
    # needs the least room), the picture at that scale
    cores = [o for parts, _ in hilts for o in parts if o.name.startswith("blade_core")]

    def shown(yaw):
        """How much of the blades' length the view shows (1: across the view; 0: end on)."""
        right = Vector((math.cos(yaw), math.sin(yaw), 0))
        worst = 1.0
        for o in cores:
            a = o.matrix_world @ Vector((0, BLADE_BASE * SCALE, 0))
            b = o.matrix_world @ Vector((0, (BLADE_BASE + BLADE_LEN) * SCALE, 0))
            d = b - a
            if d.length > 1e-6:
                worst = min(worst, math.hypot(d.dot(right), d.z) / d.length)
        return worst

    who = c["tag"].rsplit("_v", 1)[0]
    taken = CHOSEN.get(who)
    sign = 1 if c.get("turn", 30) > 0 else -1
    best = None
    for clip, frac in c.get("candidates", [[c["clips"][0], c.get("frac", 0)]]):
        if taken and taken[0] == clip:
            continue  # player 1's animation: player 2 has a pose of its own
        if not pose(clip, frac):
            continue
        found = False
        for turn in c.get("turns", (30, 40, 20, 50, 60, 15)):  # ("turns": the job's own, e.g. near the front)
            need, base = frame_body(co, arm, meshes, dict(c, fixed=False, turn=sign * turn), cores)
            seen = shown(math.radians(co["yaw"]))
            # the hips and the head too turned toward the screen's middle (not a body twisted the other way)
            yaw = math.radians(co["yaw"])
            right = Vector((math.cos(yaw), math.sin(yaw), 0))
            agree = True
            for name in ("root_pelvis_g", "head_g", "spine3_g"):
                pb = arm.pose.bones.get(name)
                if pb:
                    rest_m = (arm.matrix_world @ pb.bone.matrix_local).to_3x3()
                    now_m = (arm.matrix_world @ pb.matrix).to_3x3()
                    d = now_m @ rest_m.inverted() @ Vector((0, -1, 0))
                    d.z = 0
                    if d.length > 1e-6 and d.normalized().dot(right) * sign < 0.05:
                        agree = False
            # the blades clear of the head (not through it)
            clear = True
            for name in ("head_g", "neck_g"):
                pb = arm.pose.bones.get(name)
                if not pb:
                    continue
                h = arm.matrix_world @ ((pb.head + pb.tail) / 2 if name == "head_g" else pb.head)
                for o in cores:
                    a = o.matrix_world @ Vector((0, BLADE_BASE * SCALE, 0))
                    b = o.matrix_world @ Vector((0, (BLADE_BASE + BLADE_LEN) * SCALE, 0))
                    ab = b - a
                    k = max(0.0, min(1.0, (h - a).dot(ab) / max(ab.length_squared, 1e-9)))
                    if (a + ab * k - h).length < 0.11 * base:
                        clear = False
            score = (1 + max(0.0, need / base - 1) * 20 + (0.5 if seen < 0.6 else 0) + (0.5 if not agree else 0)
                     + (0.5 if not clear else 0))
            if best is None or score < best[0] - 1e-6:
                best = (score, clip, frac, sign * turn)
            if need <= base * 1.001 and seen >= 0.6 and agree and clear:
                best = (score, clip, frac, sign * turn)
                found = True
                break
        if found:
            break
    CHOSEN[who] = (best[1], best[2])
    c = dict(c, turn=best[3])
    log(" pose", c["tag"], os.path.basename(best[1]), best[2], "turn", best[3], "score %.2f" % best[0])
    pose(best[1], best[2])
    blades = [o for parts, _ in hilts for o in parts if o.name.startswith("blade_")]
    for o in blades:
        if o.name.startswith("blade_glow"):
            o.hide_render = True
        else:  # the core: white with a touch of the colour
            col = c["saber"]
            o.data.materials.clear()
            o.data.materials.append(emission("core", [0.55 + 0.45 * x for x in col], 5.0))
    if c.get("edits"):
        frame_body(co, arm, meshes, dict(c, fixed=True), cores)  # the camera's yaw, for the edits' directions
        edit_pose(co, arm, rest, hilts, c["edits"])
    # "fit": the frame widened to show the whole pose and its blades (else the fighters' common scale)
    frame_body(co, arm, meshes, dict(c, fixed=not c.get("fit"), yaw_kept=co["yaw"] if c.get("edits") else None), cores)
    render_to(os.path.join(ARGS["out"], "%s.png" % c["tag"]))
    # the blades alone, in their colour, the rest held out
    others = [o for o in bpy.context.scene.objects if o.type == "MESH" and o not in blades]
    holdout(others)
    for o in blades:
        o.hide_render = False
        o.data.materials.clear()
        o.data.materials.append(emission("pure", c["saber"], 3.0))
    for lo_ in LIGHTS:
        lo_.hide_render = True
    bpy.context.scene.cycles.samples = 16
    render_to(os.path.join(ARGS["out"], "%s_glow.png" % c["tag"]))
    LIGHTS.clear()


def bust(c):
    """Its bust for the select screen (behind the fighters): head and shoulders, nearly facing the camera, a 2:1
    frame, transparent, no saber."""
    c = dict(c, hilts=[])
    arm, meshes, rest, hilts = import_character(c, ARGS["work"])
    co = camera(0)
    matte(meshes)
    lights()
    cycles(1024, 512)
    stem = os.path.splitext(os.path.basename(c["clips"][0]))[0]
    act = clip_action(arm, stem)
    lo, hi = set_clip(arm, act)
    bpy.context.scene.frame_set(int(round(lo + (hi - lo) * c.get("frac", 0))))
    bpy.context.view_layer.update()
    f = facing(arm)
    turn = math.radians(c.get("turn", 12))
    f0 = Vector((math.sin(turn), -math.cos(turn), 0))
    yaw = math.atan2(f.y, f.x) - math.atan2(f0.y, f0.x)
    pose = arm.data.pose_position
    arm.data.pose_position = "REST"
    bpy.context.view_layer.update()
    feet = [arm.matrix_world @ arm.pose.bones[n].head for n in ("ankle_L_g", "ankle_R_g") if arm.pose.bones.get(n)]
    head_rest = arm.matrix_world @ arm.pose.bones["head_g"].head
    height = (head_rest.z - min(p.z for p in feet)) * 1.13
    arm.data.pose_position = pose
    bpy.context.view_layer.update()
    pb = arm.pose.bones["head_g"]
    head = arm.matrix_world @ pb.head
    top = head.z + c.get("crown", 0.115) * height
    frame_h = c.get("bust", 0.2) * height
    centre = head.copy()
    centre.z = top + 0.02 * frame_h - frame_h / 2
    d = 20.0
    co.location = centre + Vector((math.sin(yaw) * d, -math.cos(yaw) * d, 0))
    co.rotation_euler = (math.radians(90), 0, yaw)
    co.data.ortho_scale = frame_h * 2
    render_to(os.path.join(ARGS["out"], "%s.png" % c["tag"]))
    LIGHTS.clear()


def main():
    for c in ARGS["chars"]:
        try:
            log("==", c["tag"])
            mode = c.get("mode", ARGS["mode"])
            if mode == "bust":
                bust(c)
            elif ARGS["mode"] == "probe":
                probe(c)
            elif ARGS["mode"] == "hand":
                handprobe(c)
            elif ARGS["mode"] == "final":
                final(c)
        except Exception:
            log(traceback.format_exc())
    log("done")


main()
