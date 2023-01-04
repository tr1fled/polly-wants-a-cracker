from enum import Enum
import sys
import io
import os
import struct

GAME_NAME = "(ROMNAME)"
TX_DUMP_PATH = "(DUMP PATH)"
TX_IMG_PATH = TX_DUMP_PATH + GAME_NAME + "/GLideNHQ/"
FILENAME = TX_DUMP_PATH + GAME_NAME + "/GLideNHQ/scene_rips/n64_scene.glr"

# ENUMS
########
class TxWrapMode(Enum):
    """
    WN: WRAP/NO CLAMP
    MN: MIRROR/NO CLAMP
    WC: WRAP/CLAMP
    MC: MIRROR/CLAMP
    NT: NOTEXTURE
    """
    WN_WN = 0
    WN_MN = 1
    WN_WC = 2
    WN_MC = 3 # possibly just goes to UV Map (same img settings)
    MN_WN = 4
    MN_MN = 5
    MN_WC = 6
    MN_MC = 7
    WC_WN = 8
    WC_MN = 9
    WC_WC = 10
    WC_MC = 11
    MC_WN = 12
    MC_MN = 13
    MC_WC = 14
    MC_MC = 15 # possibly just goes to UV Map (same img settings)
    NT_NT = 16

########
# Gen Data functions
########
def genVertexList(num_tris, num_vertices, tri_data):
    vertices = list([None]) * num_vertices
    vtxCounter = 0
    for i in range(num_tris):
        vertices[vtxCounter] = tri_data[i].v0.getLocation()
        vertices[vtxCounter+1] = tri_data[i].v1.getLocation()
        vertices[vtxCounter+2] = tri_data[i].v2.getLocation()
        vtxCounter += 3
    return vertices

def genFaceMapList(num_tris):
    #TODO: Add merged option
    vtxCounter0 = 0
    vtxCounter1 = 1
    vtxCounter2 = 2
    face_list = list([None]) * num_tris
    for i in range(num_tris):
        face_list[i] = (vtxCounter0, vtxCounter1, vtxCounter2)
        vtxCounter0 += 3
        vtxCounter1 += 3
        vtxCounter2 += 3
    return face_list

def genUVMapList(num_tris, num_vertices, tri_data, uv_map_num):
    st_data = list([None]) * num_vertices
    vtxCounter = 0
    for i in range(num_tris):
        st_data[vtxCounter] = tri_data[i].v0.getST(uv_map_num)
        st_data[vtxCounter+1] = tri_data[i].v1.getST(uv_map_num)
        st_data[vtxCounter+2] = tri_data[i].v2.getST(uv_map_num)
        vtxCounter += 3
    return st_data

def genVCList(num_tris, num_vertices, tri_data):
    vc_vertices = list([None]) * num_vertices
    vtxCounter = 0
    for i in range(num_tris):
        vc_vertices[vtxCounter] = tri_data[i].v0.getVC()
        vc_vertices[vtxCounter+1] = tri_data[i].v1.getVC()
        vc_vertices[vtxCounter+2] = tri_data[i].v2.getVC()
        vtxCounter += 3
    return vc_vertices

def genTCList(num_tris, num_vertices, tri_data, color_mode):
    tc_vertices = list([None]) * num_vertices
    vtxCounter = 0
    for i in range(num_tris):
        tc_color_data = tri_data[i].getTC(color_mode)
        tc_vertices[vtxCounter] = tc_color_data
        tc_vertices[vtxCounter+1] = tc_color_data
        tc_vertices[vtxCounter+2] = tc_color_data
        vtxCounter += 3
    return tc_vertices

def genTxList(romname, num_tris, tri_data):
    tx_tris = list([None]) * num_tris
    for i in range(num_tris):
        tx0_filename = tri_data[i].getTextureName(romname, 0)
        tx1_filename = tri_data[i].getTextureName(romname, 1)
        tx_tris[i] = (tx0_filename, tx1_filename)
    return tx_tris

def genFogBoundingBox(mesh, fog_color):
    cube_verts = [None] * 8
    mesh_matrix = mesh.matrix_world
    cube_face_layout = \
                    [
                        (0, 1, 2, 3),
                        (5, 4, 7, 6),
                        (4, 0, 3, 7),
                        (1, 5, 6, 2),
                        (6, 7, 3, 2),
                        (5, 1, 0, 4)
                    ]
    for i in range(8):
        cube_verts[i] = mesh_matrix @ Vector(mesh.bound_box[i])
    fog_mesh = bpy.data.meshes.new("n64_scene_fog_mesh")
    fog_mesh.from_pydata(cube_verts, [], cube_face_layout)
    # Creating fog material
    nm = bpy.data.materials.new("fog")
    nm.use_nodes = True
    nm_ntn = nm.node_tree.nodes
    nm_ntn.remove(nm_ntn["Principled BSDF"])
    mon = nm_ntn["Material Output"]
    mon.location = (800,350)
    npvn = nm_ntn.new("ShaderNodeVolumePrincipled")
    npvn.inputs[0].default_value = fog_color
    npvn.inputs[2].default_value = 0.0075
    npvn.location = (450, 350)
    nm.node_tree.links.new(npvn.outputs[0], mon.inputs[1])
    # creating fog object, parenting to n64_scene mesh
    fog_obj = bpy.data.objects.new("n64_scene_fog", fog_mesh)
    fog_obj.parent = mesh
    fog_obj.display_type = 'WIRE'
    bpy.context.scene.collection.objects.link(fog_obj)
    fog_obj.data.materials.append(nm)

##########
# Blender specific functions
##########
def getMatName(t0_rCrc, t1_rCrc, tx_combined_wrapmode):
    (
        t0_texture,
        t0_palette,
    ) = struct.unpack("<2I", t0_rCrc.to_bytes(8, "little"))
    (
        t1_texture,
        t1_palette,
    ) = struct.unpack("<2I", t1_rCrc.to_bytes(8, "little"))
    returning = "%08X%08X(%02d)" % (t0_texture, t0_palette, t0_wrapmode.value)
    if t1_rCrc != 0:
        returning += ':' + "%08X%08X(%02d)" % (t1_texture, t1_palette, t1_wrapmode.value)
    return returning

def createBaseMaterial(mat_name):
    nm = bpy.data.materials.new(mat_name)
    nm.use_nodes = True
    nm_ntn = nm.node_tree.nodes
    nm_ntn.remove(nm_ntn["Principled BSDF"])
    mon = nm_ntn["Material Output"]
    mon.location = (800,350)
    nen = nm_ntn.new("ShaderNodeEmission")
    nen.location = (350,150)
    nvcn = nm_ntn.new("ShaderNodeVertexColor")
    nvcn.name = nvcn.label = nvcn.layer_name = "VC"
    nvcn.location = (-265,75)
    nvcn2 = nm_ntn.new("ShaderNodeVertexColor")
    nvcn2.name = nvcn2.label = nvcn2.layer_name = "PC"
    nvcn2.location = (-265,-100)
    nvcn3 = nm_ntn.new("ShaderNodeVertexColor")
    nvcn3.name = nvcn3.label = nvcn3.layer_name = "EC"
    nvcn3.location = (-265,-275)
    nvcn4 = nm_ntn.new("ShaderNodeVertexColor")
    nvcn4.name = nvcn3.label = nvcn3.layer_name = "BC"
    nvcn4.location = (-265,-450)
    nmcn = nm_ntn.new("ShaderNodeMixRGB")
    nmcn.inputs[0].default_value = 1.0
    nmcn.inputs[1].default_value = (1.0, 1.0, 1.0, 1.0)
    nmcn.blend_type = "MULTIPLY"
    nmcn.name = nmcn.label = "Vert Mixer"
    nmcn.location = (150,250)
    nmcn2 = nm_ntn.new("ShaderNodeMixRGB")
    nmcn2.inputs[0].default_value = 1.0
    nmcn2.blend_type = "MULTIPLY"
    nmcn2.name = nmcn2.label = "VC Mixer"
    nmcn2.location = (-50, 75)
    nmcn3 = nm_ntn.new("ShaderNodeMixRGB")
    nmcn3.inputs[0].default_value = 1.0
    nmcn3.blend_type = "MULTIPLY"
    nmcn3.name = nmcn3.label = "PC Mixer"
    nmcn3.location = (-50, -100)
    nmcn4 = nm_ntn.new("ShaderNodeMixRGB")
    nmcn4.inputs[0].default_value = 1.0
    nmcn4.blend_type = "MULTIPLY"
    nmcn4.name = nmcn4.label = "EC Mixer"
    nmcn4.location = (-50, -275)
    nmcn5 = nm_ntn.new("ShaderNodeMixRGB")
    nmcn5.inputs[0].default_value = 1.0
    nmcn5.inputs[1].default_value = (1.0, 1.0, 1.0, 1.0)
    nmcn5.blend_type = "MULTIPLY"
    nmcn5.name = nmcn5.label = "BC Mixer"
    nmcn5.location = (-50,-450)
    nm.node_tree.links.new(nvcn.outputs[0], nmcn2.inputs[2])
    nm.node_tree.links.new(nvcn2.outputs[0], nmcn3.inputs[2])
    nm.node_tree.links.new(nvcn3.outputs[0], nmcn4.inputs[2])
    nm.node_tree.links.new(nvcn4.outputs[0], nmcn5.inputs[2])
    nm.node_tree.links.new(nmcn2.outputs[0], nmcn.inputs[2])
    nm.node_tree.links.new(nmcn3.outputs[0], nmcn2.inputs[1])
    nm.node_tree.links.new(nmcn4.outputs[0], nmcn3.inputs[1])
    nm.node_tree.links.new(nmcn5.outputs[0], nmcn4.inputs[1])
    nm.node_tree.links.new(nmcn.outputs[0], nen.inputs[0])
    nm.node_tree.links.new(nen.outputs[0], mon.inputs[0])
    mesh.materials.append(nm)

def loadNewImage(image_filename):
    try:
        bpy.data.images.load(TX_IMG_PATH + image_filename)
    except RuntimeError:
        print("EXCEPTION: %s doesn't exist!" % (TX_IMG_PATH + image_filename))
    return bpy.data.images.find(image_filename)

def addTxToMatIdx(mat_idx, tx_wrapmode):
    mat = bpy.data.materials[mat_idx]
    mat_ntn = mat.node_tree.nodes
    base_ntin_idx = mat_ntn.find("Base Texture")
    detail_ntin_idx = mat_ntn.find("Detail Texture")
    if detail_ntin_idx != -1:
        print("ERROR: Attempted loading 2nd detail texture!\nmat_idx: %d, tx_wrapmode: %s, mat.name: %s" % (mat_idx, tx_wrapmode, mat.name))
        return
    evmn = mat_ntn[mat_ntn.find("Vert Mixer")]
    ntin = mat_ntn.new("ShaderNodeTexImage")
    if base_ntin_idx == -1:
        img_txt_name = tx0_filename
    else:
        img_txt_name = tx1_filename
    img_txt_index = bpy.data.images.find(img_txt_name)
    if img_txt_index == -1:
        img_txt_index = loadNewImage(img_txt_name)
    ntin.image = bpy.data.images[img_txt_index]
    if base_ntin_idx == -1:
        ntin.name = ntin.label = "Base Texture"
        ntin.location = (-600, 430)
    else:
        ntin.name = ntin.label = "Detail Texture"
        ntin.location = (-600, -25)
    ntiun = mat_ntn.new("ShaderNodeUVMap")
    if base_ntin_idx == -1:
        ntiun.uv_map = "UV1"
        ntiun.name = ntiun.label = "Base UV Map"
        ntiun.location = (-1400, 175)
    else:
        ntiun.uv_map = "UV2"
        ntiun.name = ntiun.label = "Detail UV Map"
        ntiun.location = (-1400, -190)
    if( tx_wrapmode.value == 0 or
        tx_wrapmode.value == 10 or
        tx_wrapmode.value == 16    ):
        mat.node_tree.links.new(ntiun.outputs[0], ntin.inputs[0])
        if tx_wrapmode.value == 10:
            ntin.extension = "EXTEND"
    else:
        ntin.extension = "EXTEND"
        nsvn = mat_ntn.new("ShaderNodeSeparateXYZ")
        ncvn = mat_ntn.new("ShaderNodeCombineXYZ")
        mat.node_tree.links.new(ntiun.outputs[0], nsvn.inputs[0])
        mat.node_tree.links.new(ncvn.outputs[0], ntin.inputs[0])
        if base_ntin_idx == -1:
            nsvn.location = (-1200, 185)
            ncvn.location = (-800, 200)
        else:
            nsvn.location = (-1200, -315)
            ncvn.location = (-800, -300)
        if( tx_wrapmode.value == 1 or
            tx_wrapmode.value == 2 or 
            tx_wrapmode.value == 3 or 
            tx_wrapmode.value == 4 or 
            tx_wrapmode.value == 8 or 
            tx_wrapmode.value == 12    ):
                nsnm = mat_ntn.new("ShaderNodeMath")
                nsnm.operation = "WRAP"
                nsnm.inputs[1].default_value = 0.0
                nsnm.inputs[2].default_value = 1.0
                if(base_ntin_idx == -1):
                    nsnm.location = (-995,320)
                else:
                    nsnm.location = (-995,-180)
        if( tx_wrapmode.value == 1 or
            tx_wrapmode.value == 3 or 
            tx_wrapmode.value == 4 or 
            tx_wrapmode.value == 5 or 
            tx_wrapmode.value == 6 or 
            tx_wrapmode.value == 7 or 
            tx_wrapmode.value == 9 or 
            tx_wrapmode.value == 11 or 
            tx_wrapmode.value == 12 or 
            tx_wrapmode.value == 13 or 
            tx_wrapmode.value == 14 or 
            tx_wrapmode.value == 15    ):
                nsnm2 = mat_ntn.new("ShaderNodeMath")
                nsnm2.operation = "PINGPONG"
                nsnm2.inputs[1].default_value = 1.0
                if(base_ntin_idx == -1):
                    nsnm2.location = (-995,130)
                else:
                    nsnm2.location = (-995,-370)
        if( tx_wrapmode.value == 5 or
            tx_wrapmode.value == 7 or
            tx_wrapmode.value == 13 or
            tx_wrapmode.value == 15     ):
                nsnm3 = mat_ntn.new("ShaderNodeMath")
                nsnm3.operation = "PINGPONG"
                nsnm3.inputs[1].default_value = 1.0
                if(base_ntin_idx == -1):
                    nsnm3.location = (-995,320)
                else:
                    nsnm3.location = (-995,320)
        if( tx_wrapmode.value == 1 or
            tx_wrapmode.value == 2 or 
            tx_wrapmode.value == 3     ):
                #1,2,3 (connect x to wrap node)
                mat.node_tree.links.new(nsvn.outputs[0], nsnm.inputs[0])
                mat.node_tree.links.new(nsnm.outputs[0], ncvn.inputs[0])
        if( tx_wrapmode.value == 4 or
            tx_wrapmode.value == 8 or 
            tx_wrapmode.value == 12    ):
                #4,8,12 (connect y to wrap node)
                mat.node_tree.links.new(nsvn.outputs[1], nsnm.inputs[0])
                mat.node_tree.links.new(nsnm.outputs[0], ncvn.inputs[1])
        if( tx_wrapmode.value == 8 or
            tx_wrapmode.value == 9 or 
            tx_wrapmode.value == 10 or
            tx_wrapmode.value == 11     ):
                #8,9,10,11 (connect to x directly)
                mat.node_tree.links.new(nsvn.outputs[0], ncvn.inputs[0])
        if( tx_wrapmode.value == 2 or
            tx_wrapmode.value == 6 or 
            tx_wrapmode.value == 10 or
            tx_wrapmode.value == 14     ):
                #2,6,10,14 (connect to y directly)
                mat.node_tree.links.new(nsvn.outputs[1], ncvn.inputs[1])
        if( tx_wrapmode.value == 4 or
            tx_wrapmode.value == 5 or 
            tx_wrapmode.value == 6 or 
            tx_wrapmode.value == 7 or 
            tx_wrapmode.value == 12 or 
            tx_wrapmode.value == 13 or 
            tx_wrapmode.value == 14 or 
            tx_wrapmode.value == 15    ):
                #5,7,13,15 (edge case, 2 pingpong nodes, connect x to new nsnm3)
                if( tx_wrapmode.value == 5 or
                    tx_wrapmode.value == 7 or
                    tx_wrapmode.value == 13 or
                    tx_wrapmode.value == 15    ):
                    mat.node_tree.links.new(nsvn.outputs[0], nsnm3.inputs[0])
                    mat.node_tree.links.new(nsnm3.outputs[0], ncvn.inputs[0])
                else:
                    #4,6,7,12,13,14 (connect x to pingpong node)
                    mat.node_tree.links.new(nsvn.outputs[0], nsnm2.inputs[0])
                    mat.node_tree.links.new(nsnm2.outputs[0], ncvn.inputs[0])
        if( tx_wrapmode.value == 1 or
            tx_wrapmode.value == 3 or 
            tx_wrapmode.value == 5 or 
            tx_wrapmode.value == 7 or 
            tx_wrapmode.value == 9 or 
            tx_wrapmode.value == 11 or 
            tx_wrapmode.value == 13 or 
            tx_wrapmode.value == 15    ):
                #1,3,5,7,9,11,13,15 (connect y to pingpong node)
                mat.node_tree.links.new(nsvn.outputs[1], nsnm2.inputs[0])
                mat.node_tree.links.new(nsnm2.outputs[0], ncvn.inputs[1])
    if base_ntin_idx == -1:
        ntbn = mat_ntn.new("ShaderNodeBsdfTransparent")
        ntbn.location = (350,250)
        nsmn = mat_ntn.new("ShaderNodeMixShader")
        nsmn.location = (575,350)
        mat.node_tree.links.new(ntin.outputs[0], evmn.inputs[1])
        mat.node_tree.links.new(ntin.outputs[1], nsmn.inputs[0])
        mat.node_tree.links.new(mat_ntn[mat_ntn.find("Emission")].outputs[0], nsmn.inputs[2])
        mat.node_tree.links.new(ntbn.outputs[0], nsmn.inputs[1])
        mat.node_tree.links.new(nsmn.outputs[0], mat_ntn[mat_ntn.find("Material Output")].inputs[0])
    else:
        nmcn = mat_ntn.new("ShaderNodeMixRGB")
        nmcn.inputs[0].default_value = 0.5
        nmcn.blend_type = "MIX"
        nmcn.name = nmcn.label = "Alpha Mixer"
        nmcn.location = (-265,430)
        nmcn2 = mat_ntn.new("ShaderNodeMixRGB")
        nmcn2.inputs[0].default_value = 0.5
        nmcn2.blend_type = "MIX"
        nmcn2.name = nmcn2.label = "Base + Detail Mixer"
        nmcn2.location = (-265,250)
        mat.node_tree.links.new(ntin.outputs[1], nmcn.inputs[2])
        mat.node_tree.links.new(nmcn.outputs[0], mat_ntn[mat_ntn.find("Mix Shader")].inputs[0])
        mat.node_tree.links.new(mat_ntn[mat_ntn.find("Base Texture")].outputs[0], nmcn2.inputs[1])
        mat.node_tree.links.new(mat_ntn[mat_ntn.find("Base Texture")].outputs[1], nmcn.inputs[1])
        mat.node_tree.links.new(nmcn2.outputs[0], evmn.inputs[1])
        mat.node_tree.links.new(ntin.outputs[0], nmcn2.inputs[2])

def createMat(mat_name, tx_wrapmode):
    createBaseMaterial(mat_name)
    new_mat_idx = bpy.data.materials.find(mat_name)
    addTxToMatIdx(new_mat_idx, tx_wrapmode)

class RipVertice:
    def __init__(self,
                sx, sy, sz,
                r, g, b, a,
                s0, t0, s1, t1
                ):
        self.sx : float = sx
        self.sy : float = sy
        self.sz : float = sz
        self.r : float = r
        self.g : float = g
        self.b : float = b
        self.a : float = a
        self.s0 : float = s0
        self.t0 : float = t0
        self.s1 : float = s1
        self.t1 : float = t1
    def getLocation(self):
        sx_f = float("%.6f" % self.sx)
        sy_f = float("%.6f" % self.sy)
        sz_f = float("%.6f" % self.sz)
        return (sx_f, sy_f, sz_f)
    def getVC(self, combined=True):
        if(combined):
            r_f = float("%.6f" % (self.r * self.a))
            g_f = float("%.6f" % (self.g * self.a))
            b_f = float("%.6f" % (self.b * self.a))
            a_f = 1.0
        else:
            r_f = float("%.6f" % self.r)
            g_f = float("%.6f" % self.g)
            b_f = float("%.6f" % self.b)
            a_f = float("%.6f" % self.a)
        return (r_f, g_f, b_f, a_f)
    def getST(self, uv_map_num):
        if uv_map_num == 0:
            s_f = float("%.6f" % self.s0)
            t_f = float("%.6f" % self.t0)
        else:
            s_f = float("%.6f" % self.s1)
            t_f = float("%.6f" % self.t1)
        return (s_f, t_f)

class RipTriangle:
    def __init__(self,
                v0, v1, v2,
                prim_r, prim_g, prim_b, prim_a,
                env_r, env_g, env_b, env_a,
                blend_r, blend_g, blend_b, blend_a,
                t0_riceCrc, t1_riceCrc,
                t0_wrapmode, t1_wrapmode,
                t0_fmt, t0_size, t1_fmt, t1_size
                ):
        self.v0 : RipVertice = v0
        self.v1 : RipVertice = v1
        self.v2 : RipVertice = v2
        self.prim_r : float = prim_r
        self.prim_g : float = prim_g
        self.prim_b : float = prim_b
        self.prim_a : float = prim_a
        self.env_r : float = env_r
        self.env_g : float = env_g
        self.env_b : float = env_b
        self.env_a : float = env_a
        self.blend_r : float = blend_r
        self.blend_g : float = blend_g
        self.blend_b : float = blend_b
        self.blend_a : float = blend_a
        self.t0_riceCrc : int = t0_riceCrc
        self.t1_riceCrc : int = t1_riceCrc
        self.t0_wrapmode : int = t0_wrapmode
        self.t1_wrapmode : int = t1_wrapmode
        self.t0_fmt : int = t0_fmt
        self.t0_size : int = t0_size
        self.t1_fmt: int  = t1_fmt
        self.t1_size : int = t1_size
    def getTC(self, color_mode, combined=True, inversed=True):
        if color_mode == 1:
            r_f = self.prim_r
            g_f = self.prim_g
            b_f = self.prim_b
            a_f = self.prim_a
        if color_mode == 2:
            r_f = self.env_r
            g_f = self.env_g
            b_f = self.env_b
            a_f = self.env_a
        if color_mode == 3:
            r_f = self.blend_r
            g_f = self.blend_g
            b_f = self.blend_b
            a_f = self.blend_a
        if(inversed):
            if (color_mode == 2 or
                color_mode == 3):
                r_f = abs(r_f - 1.0)
                g_f = abs(g_f - 1.0)
                b_f = abs(b_f - 1.0)
                a_f = abs(a_f - 1.0)
        if combined:
            r_f = float("%.6f" % (r_f * a_f))
            g_f = float("%.6f" % (g_f * a_f))
            b_f = float("%.6f" % (b_f * a_f))
            a_f = 1.0
        else:
            r_f = float("%.6f" % r_f)
            g_f = float("%.6f" % g_f)
            b_f = float("%.6f" % b_f)
            a_f = float("%.6f" % a_f)
        return (r_f, g_f, b_f, a_f)
    def getTextureName(self, romname, texture_num):
        if texture_num == 0:
            calc_crc = self.t0_riceCrc
            calc_fmt = self.t0_fmt
            calc_size = self.t0_size
        else:
            calc_crc = self.t1_riceCrc
            calc_fmt = self.t1_fmt
            calc_size = self.t1_size
        if calc_crc == 0:
            return ""
        filename = ""
        (texture, palette) = struct.unpack("<2I", calc_crc.to_bytes(8, "little"))
        if calc_fmt == 2:
            filename = "%ls#%08X#%01X#%01X#%08X_ciByRGBA.png" % (romname, texture, calc_fmt, calc_size, palette)
        else:
            filename = "%ls#%08X#%01X#%01X_all.png" % (romname, texture, calc_fmt, calc_size)
        return filename
    def getBlenderMaterialName(self):
        if self.t0_riceCrc == 0:
            return ""
        (t0_texture, t0_palette) = struct.unpack("<2I", self.t0_riceCrc.to_bytes(8, "little"))
        (t1_texture, t1_palette) = struct.unpack("<2I", self.t1_riceCrc.to_bytes(8, "little"))
        if t0_palette != 0:
            returning = "%08X%08X(%02d)" % (t0_texture, t0_palette, self.t0_wrapmode)
        else:
            returning = "%08X(%02d)" % (t0_texture, self.t0_wrapmode)
        if self.t1_riceCrc != 0:
            if t1_palette != 0:
                returning += ":%08X%08X(%02d)" % (t1_texture, t1_palette, self.t1_wrapmode)
            else:
                returning += ":%08X(%02d)" % (t1_texture, self.t1_wrapmode)
        return returning
    def getT0WrapMode(self):
        return TxWrapMode(self.t0_wrapmode)
    def getT1WrapMode(self):
        return TxWrapMode(self.t1_wrapmode)

# Start of opening file

with io.open(FILENAME, 'rb') as fb:
    if fb.read(6) != b'GL64R\0':
        raise Exception("Not a valid N64 scene rip file")
    (version,) = struct.unpack("<H", fb.read(2))
    if version != 1:
        raise Exception("Unknown N64 Ripper version (%d) encountered" % version)
    romname_raw = fb.read(20)
    if romname_raw[0] == 0:
        raise Exception("Empty rom name encountered")
    try:
        romname_raw = romname_raw[0:romname_raw.index(0)]
    except ValueError:
        pass
    romname = romname_raw.decode()
    (num_tris,) = struct.unpack("<I", fb.read(4))
    if num_tris == 0:
        raise Exception("File triangle count is 0")
    num_vertices = num_tris * 3
    (
        fog_r, fog_g, fog_b
    ) = struct.unpack("<3f", fb.read(12))
    fog_color = (fog_r, fog_g, fog_b, 1.0)
    calc_left = 208 * num_tris # sizeof(RipTriangle): 208
    to_scan = os.path.getsize(FILENAME) - 44
    if to_scan != calc_left:
        raise Exception("Expected to scan %d remaining bytes, found %d left" % (calc_left, to_scan))
    tri_data = [None] * num_tris
    for i in range(num_tris):
        (
            # RipVertice[3]
            v0_sx, v0_sy, v0_sz,
            v0_r, v0_g, v0_b, v0_a,
            v0_s0, v0_t0, v0_s1, v0_t1,
            v1_sx, v1_sy, v1_sz,
            v1_r, v1_g, v1_b, v1_a,
            v1_s0, v1_t0, v1_s1, v1_t1,
            v2_sx, v2_sy, v2_sz,
            v2_r, v2_g, v2_b, v2_a,
            v2_s0, v2_t0, v2_s1, v2_t1,
            # Rip Vertice[3] end (at 132)
            __PAD0,
            t_p_r, t_p_g, t_p_b, t_p_a,
            t_e_r, t_e_g, t_e_b, t_e_a,
            t_b_r, t_b_g, t_b_b, t_b_a,
            t_t0_rCrc, t_t1_rCrc,
            t_t0_wm, t_t1_wm,
            t_t0_fmt, t_t0_size,
            t_t1_fmt, t_t1_size,
            __PAD1
        ) = struct.unpack("<33f1I12f2Q6B1H",fb.read(208))
        # combine everything
        v0 = RipVertice(v0_sx, v0_sy, v0_sz,
                        v0_r, v0_g, v0_b, v0_a,
                        v0_s0, v0_t0,
                        v0_s1, v0_t1)
        v1 = RipVertice(v1_sx, v1_sy, v1_sz,
                        v1_r, v1_g, v1_b, v1_a,
                        v1_s0, v1_t0,
                        v1_s1, v1_t1)
        v2 = RipVertice(v2_sx, v2_sy, v2_sz,
                        v2_r, v2_g, v2_b, v2_a,
                        v2_s0, v2_t0,
                        v2_s1, v2_t1)
        rt = RipTriangle(v0, v1, v2,
                        t_p_r, t_p_g, t_p_b, t_p_a,
                        t_e_r, t_e_g, t_e_b, t_e_a,
                        t_b_r, t_b_g, t_b_b, t_b_a,
                        t_t0_rCrc,
                        t_t1_rCrc,
                        t_t0_wm, t_t1_wm,
                        t_t0_fmt, t_t0_size,
                        t_t1_fmt, t_t1_size)
        tri_data[i] = rt

    # Blender face data prep
    bpy_vertice_xyz_data = genVertexList(num_tris, num_vertices, tri_data)
    bpy_face_layout_data = genFaceMapList(num_tris)

    # UV Map Data Gen
    bpy_vertice_UV1_data = genUVMapList(num_tris, num_vertices, tri_data, 0)
    bpy_vertice_UV2_data = genUVMapList(num_tris, num_vertices, tri_data, 1)

    # Color Data Gen
    bpy_vertice_VC_data = genVCList(num_tris, num_vertices, tri_data)
    bpy_triangle_PC_data = genTCList(num_tris, num_vertices, tri_data, 1)
    bpy_triangle_EC_data = genTCList(num_tris, num_vertices, tri_data, 2)
    bpy_triangle_BC_data = genTCList(num_tris, num_vertices, tri_data, 3)

    # Texture Data Map Gen
    bpy_triangle_tx_data = genTxList(romname, num_tris, tri_data)

    ######### Start of Blender Code ########

    import bpy
    import bmesh
    from math import radians
    from mathutils import Vector

    mesh = bpy.data.meshes.new("n64_scene_mesh")
    mesh.from_pydata(bpy_vertice_xyz_data, [], bpy_face_layout_data)
    obj = bpy.data.objects.new("n64_scene", mesh)
    bpy.context.scene.collection.objects.link(obj)
    
    null_print = obj.data.vertex_colors.new(name="VC")
    null_print = obj.data.vertex_colors.new(name="PC")
    null_print = obj.data.vertex_colors.new(name="EC")
    null_print = obj.data.vertex_colors.new(name="BC")
    null_print = obj.data.uv_layers.new(name="UV1")
    null_print = obj.data.uv_layers.new(name="UV2")

    mesh_VC = obj.data.vertex_colors["VC"]
    mesh_PC = obj.data.vertex_colors["PC"]
    mesh_EC = obj.data.vertex_colors["EC"]
    mesh_BC = obj.data.vertex_colors["BC"]

    mesh_UV1 = obj.data.uv_layers["UV1"]
    mesh_UV2 = obj.data.uv_layers["UV2"]

    for i in range(num_vertices):
        mesh_UV1.data[i].uv = bpy_vertice_UV1_data[i]
        mesh_UV2.data[i].uv = bpy_vertice_UV2_data[i]
        mesh_VC.data[i].color = bpy_vertice_VC_data[i]
        mesh_PC.data[i].color = bpy_triangle_PC_data[i]
        mesh_EC.data[i].color = bpy_triangle_EC_data[i]
        mesh_BC.data[i].color = bpy_triangle_BC_data[i]
    for i in range(num_tris):
        tx0_filename = bpy_triangle_tx_data[i][0]
        tx1_filename = bpy_triangle_tx_data[i][1]
        num_used_textures = 0
        if tx0_filename != "":
            num_used_textures += 1
        if tx1_filename != "":
            num_used_textures += 1
        if num_used_textures == 0:
            no_mat_idx = mesh.materials.find("NO_TEXTURE")
            if no_mat_idx == -1:
                createBaseMaterial("NO_TEXTURE")
                mesh.polygons[i].material_index = mesh.materials.find("NO_TEXTURE")
            else:
                mesh.polygons[i].material_index = no_mat_idx
        else:
            mat_name = tri_data[i].getBlenderMaterialName()
            mat_idx = mesh.materials.find(mat_name)
            if mat_idx == -1:
                createMat(mat_name, tri_data[i].getT0WrapMode())
                mesh.polygons[i].material_index = mesh.materials.find(mat_name)
                mat_idx = mesh.polygons[i].material_index
            else:
                mesh.polygons[i].material_index = mat_idx
            data_mat_idx = bpy.data.materials.find(mat_name)
            mat_dtn = bpy.data.materials[data_mat_idx].node_tree.nodes.find("Detail Texture")
            if num_used_textures == 2 and mat_dtn == -1:
                addTxToMatIdx(data_mat_idx, tri_data[i].getT1WrapMode())
    
    genFogBoundingBox(bpy.data.objects[0], fog_color)
    obj.rotation_euler = ((radians(90),0,0))

    ######## END OF BLENDER CODE ########
    fb.close()