// Offline converter for the benchmark scene: glTF/GLB (with KHR_draco_mesh_compression) -> .vab, a flat binary the host's
// benchmark renderer loads without any glTF/Draco code. Built in WSL by tools/bench_scene/build.sh (Draco 1.5.7, cgltf 1.14).
//
// .vab v1 (little endian; every array is padded to 4 bytes):
//   "VAB1" u32 version
//   u32 images;     each: u32 mime (0 png, 1 jpeg), u32 bytes, data
//   u32 materials;  each: f32 baseColor[4], i32 baseTex, i32 baseUv, i32 aoTex, i32 aoUv, f32 aoStrength, f32 emissive[3],
//                         i32 emissiveTex, i32 emissiveUv, u32 alphaMode (0 opaque, 1 mask, 2 blend), f32 alphaCutoff, u32 doubleSided
//   u32 meshes;     each: u32 prims; each prim: i32 material, u32 verts, u32 indices, f32 pos[3v], f32 nrm[3v], f32 uv0[2v], f32 uv1[2v], u32 idx[i]
//   u32 nodes;      each: i32 parent, i32 mesh, f32 t[3], f32 r[4] (xyzw), f32 s[3], u32 hasMatrix, f32 matrix[16] (column major)
//   u32 channels;   each: i32 node, u32 path (0 T, 1 R, 2 S), u32 interp (0 linear, 1 step, 2 cubic), u32 keys, f32 times[k], f32 values[k*n*(cubic?3:1)]
//   f32 boundsMin[3], f32 boundsMax[3]   (world space, rest pose), f32 animationLength
// Textures are referenced by image index (the scene's samplers are all repeat/linear).
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#include "draco/compression/decode.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::vector<uint8_t> out;
static void put(const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; out.insert(out.end(), b, b + n); while (out.size() % 4) out.push_back(0); }
static void u32(uint32_t v) { put(&v, 4); }
static void i32(int32_t v) { put(&v, 4); }
static void f32(float v) { put(&v, 4); }
static void f32s(const float* v, size_t n) { if (n) put(v, n * 4); }

struct Prim { int material = -1; std::vector<float> pos, nrm, uv0, uv1; std::vector<uint32_t> idx; };

static cgltf_data* g = nullptr;

static int imageOf(const cgltf_texture_view& v) { return v.texture && v.texture->image ? (int)(v.texture->image - g->images) : -1; }

static bool primDraco(const cgltf_primitive& p, Prim& o) {
    const auto& dm = p.draco_mesh_compression;
    draco::DecoderBuffer buf;
    buf.Init((const char*)dm.buffer_view->buffer->data + dm.buffer_view->offset, dm.buffer_view->size);
    draco::Decoder dec;
    auto r = dec.DecodeMeshFromBuffer(&buf);
    if (!r.ok()) { fprintf(stderr, "draco: %s\n", r.status().error_msg()); return false; }
    std::unique_ptr<draco::Mesh> m = std::move(r).value();
    const uint32_t n = m->num_points();
    auto read = [&](const char* sem, int comps, std::vector<float>& dst) {
        dst.assign((size_t)n * comps, 0.f);
        for (cgltf_size a = 0; a < dm.attributes_count; a++) {
            if (strcmp(dm.attributes[a].name, sem) != 0) continue;
            const int uid = (int)(dm.attributes[a].data - g->accessors); // cgltf stores the draco attribute id as an accessor "index"
            const draco::PointAttribute* att = m->GetAttributeByUniqueId(uid);
            if (!att) { fprintf(stderr, "draco attribute %s (%d) missing\n", sem, uid); return false; }
            float v[4];
            for (uint32_t i = 0; i < n; i++) {
                att->ConvertValue<float>(att->mapped_index(draco::PointIndex(i)), comps, v);
                memcpy(&dst[(size_t)i * comps], v, comps * 4);
            }
            return true;
        }
        return false;
    };
    if (!read("POSITION", 3, o.pos)) return false;
    read("NORMAL", 3, o.nrm);
    read("TEXCOORD_0", 2, o.uv0);
    read("TEXCOORD_1", 2, o.uv1);
    o.idx.resize((size_t)m->num_faces() * 3);
    for (uint32_t f = 0; f < m->num_faces(); f++)
        for (int k = 0; k < 3; k++) o.idx[(size_t)f * 3 + k] = m->face(draco::FaceIndex(f))[k].value();
    return true;
}

static bool primPlain(const cgltf_primitive& p, Prim& o) {
    cgltf_size n = 0;
    auto read = [&](cgltf_attribute_type t, int set, int comps, std::vector<float>& dst) {
        for (cgltf_size a = 0; a < p.attributes_count; a++) {
            if (p.attributes[a].type != t || p.attributes[a].index != set) continue;
            const cgltf_accessor* acc = p.attributes[a].data;
            n = acc->count;
            dst.assign(n * comps, 0.f);
            for (cgltf_size i = 0; i < n; i++) cgltf_accessor_read_float(acc, i, &dst[i * comps], comps);
            return true;
        }
        return false;
    };
    if (!read(cgltf_attribute_type_position, 0, 3, o.pos)) return false;
    const size_t verts = o.pos.size() / 3;
    if (!read(cgltf_attribute_type_normal, 0, 3, o.nrm)) o.nrm.assign(verts * 3, 0.f);
    if (!read(cgltf_attribute_type_texcoord, 0, 2, o.uv0)) o.uv0.assign(verts * 2, 0.f);
    if (!read(cgltf_attribute_type_texcoord, 1, 2, o.uv1)) o.uv1.assign(verts * 2, 0.f);
    if (p.indices) { o.idx.resize(p.indices->count); for (cgltf_size i = 0; i < p.indices->count; i++) o.idx[i] = (uint32_t)cgltf_accessor_read_index(p.indices, i); }
    else { o.idx.resize(verts); for (size_t i = 0; i < verts; i++) o.idx[i] = (uint32_t)i; }
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: convert in.glb out.vab\n"); return 2; }
    cgltf_options opt{};
    if (cgltf_parse_file(&opt, argv[1], &g) != cgltf_result_success || cgltf_load_buffers(&opt, g, argv[1]) != cgltf_result_success) {
        fprintf(stderr, "cannot parse %s\n", argv[1]);
        return 1;
    }
    put("VAB1", 4);
    u32(1);
    // images
    u32((uint32_t)g->images_count);
    for (cgltf_size i = 0; i < g->images_count; i++) {
        const cgltf_image& im = g->images[i];
        if (!im.buffer_view) { fprintf(stderr, "image %zu is not embedded\n", i); return 1; }
        u32(im.mime_type && strstr(im.mime_type, "jpeg") ? 1 : 0);
        u32((uint32_t)im.buffer_view->size);
        put((const uint8_t*)im.buffer_view->buffer->data + im.buffer_view->offset, im.buffer_view->size);
    }
    // materials
    u32((uint32_t)g->materials_count);
    for (cgltf_size i = 0; i < g->materials_count; i++) {
        const cgltf_material& m = g->materials[i];
        f32s(m.pbr_metallic_roughness.base_color_factor, 4);
        i32(imageOf(m.pbr_metallic_roughness.base_color_texture)); i32(m.pbr_metallic_roughness.base_color_texture.texcoord);
        i32(imageOf(m.occlusion_texture)); i32(m.occlusion_texture.texcoord); f32(m.occlusion_texture.texture ? m.occlusion_texture.scale : 1.f);
        f32s(m.emissive_factor, 3);
        i32(imageOf(m.emissive_texture)); i32(m.emissive_texture.texcoord);
        u32(m.alpha_mode == cgltf_alpha_mode_mask ? 1 : m.alpha_mode == cgltf_alpha_mode_blend ? 2 : 0);
        f32(m.alpha_cutoff);
        u32(m.double_sided ? 1 : 0);
    }
    // meshes
    size_t tris = 0, verts = 0;
    std::vector<std::vector<Prim>> meshes(g->meshes_count);
    for (cgltf_size i = 0; i < g->meshes_count; i++) {
        for (cgltf_size j = 0; j < g->meshes[i].primitives_count; j++) {
            const cgltf_primitive& p = g->meshes[i].primitives[j];
            if (p.type != cgltf_primitive_type_triangles) continue;
            Prim o;
            o.material = p.material ? (int)(p.material - g->materials) : -1;
            if (!(p.has_draco_mesh_compression ? primDraco(p, o) : primPlain(p, o))) { fprintf(stderr, "mesh %zu prim %zu failed\n", i, j); return 1; }
            const size_t n = o.pos.size() / 3;
            o.nrm.resize(n * 3); o.uv0.resize(n * 2); o.uv1.resize(n * 2);
            tris += o.idx.size() / 3; verts += n;
            meshes[i].push_back(std::move(o));
        }
    }
    u32((uint32_t)meshes.size());
    for (auto& m : meshes) {
        u32((uint32_t)m.size());
        for (auto& p : m) {
            const uint32_t n = (uint32_t)(p.pos.size() / 3);
            i32(p.material); u32(n); u32((uint32_t)p.idx.size());
            f32s(p.pos.data(), p.pos.size()); f32s(p.nrm.data(), p.nrm.size()); f32s(p.uv0.data(), p.uv0.size()); f32s(p.uv1.data(), p.uv1.size());
            put(p.idx.data(), p.idx.size() * 4);
        }
    }
    // nodes
    u32((uint32_t)g->nodes_count);
    for (cgltf_size i = 0; i < g->nodes_count; i++) {
        const cgltf_node& n = g->nodes[i];
        i32(n.parent ? (int)(n.parent - g->nodes) : -1);
        i32(n.mesh ? (int)(n.mesh - g->meshes) : -1);
        const float t0[3] = { 0, 0, 0 }, r0[4] = { 0, 0, 0, 1 }, s0[3] = { 1, 1, 1 };
        f32s(n.has_translation ? n.translation : t0, 3); f32s(n.has_rotation ? n.rotation : r0, 4); f32s(n.has_scale ? n.scale : s0, 3);
        u32(n.has_matrix ? 1 : 0);
        float mtx[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        if (n.has_matrix) memcpy(mtx, n.matrix, sizeof mtx);
        f32s(mtx, 16);
    }
    // animation (all channels of all animations; the scene has one)
    std::vector<const cgltf_animation_channel*> ch;
    float animLen = 0;
    for (cgltf_size a = 0; a < g->animations_count; a++)
        for (cgltf_size c = 0; c < g->animations[a].channels_count; c++) {
            const auto& x = g->animations[a].channels[c];
            if (x.target_node && (x.target_path == cgltf_animation_path_type_translation || x.target_path == cgltf_animation_path_type_rotation || x.target_path == cgltf_animation_path_type_scale)) ch.push_back(&x);
        }
    u32((uint32_t)ch.size());
    for (auto* x : ch) {
        const int comps = x->target_path == cgltf_animation_path_type_rotation ? 4 : 3;
        const cgltf_animation_sampler* s = x->sampler;
        i32((int)(x->target_node - g->nodes));
        u32(x->target_path == cgltf_animation_path_type_translation ? 0 : x->target_path == cgltf_animation_path_type_rotation ? 1 : 2);
        u32(s->interpolation == cgltf_interpolation_type_step ? 1 : s->interpolation == cgltf_interpolation_type_cubic_spline ? 2 : 0);
        const cgltf_size k = s->input->count;
        u32((uint32_t)k);
        std::vector<float> times(k), vals(s->output->count * comps);
        for (cgltf_size i = 0; i < k; i++) { cgltf_accessor_read_float(s->input, i, &times[i], 1); animLen = std::max(animLen, times[i]); }
        for (cgltf_size i = 0; i < s->output->count; i++) cgltf_accessor_read_float(s->output, i, &vals[i * comps], comps);
        f32s(times.data(), times.size());
        f32s(vals.data(), vals.size());
    }
    // world bounds in the rest pose
    float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (cgltf_size i = 0; i < g->nodes_count; i++) {
        const cgltf_node& n = g->nodes[i];
        if (!n.mesh) continue;
        float w[16];
        cgltf_node_transform_world(&n, w);
        for (auto& p : meshes[n.mesh - g->meshes])
            for (size_t v = 0; v < p.pos.size(); v += 3) {
                const float x = p.pos[v], y = p.pos[v + 1], z = p.pos[v + 2];
                const float q[3] = { w[0] * x + w[4] * y + w[8] * z + w[12], w[1] * x + w[5] * y + w[9] * z + w[13], w[2] * x + w[6] * y + w[10] * z + w[14] };
                for (int c = 0; c < 3; c++) { lo[c] = std::min(lo[c], q[c]); hi[c] = std::max(hi[c], q[c]); }
            }
    }
    f32s(lo, 3); f32s(hi, 3); f32(animLen);
    FILE* f = fopen(argv[2], "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    printf("{\"images\":%zu,\"materials\":%zu,\"meshes\":%zu,\"nodes\":%zu,\"channels\":%zu,\"vertices\":%zu,\"triangles\":%zu,\"bytes\":%zu,"
           "\"bounds\":[[%.2f,%.2f,%.2f],[%.2f,%.2f,%.2f]],\"animation_s\":%.2f}\n",
           g->images_count, g->materials_count, meshes.size(), g->nodes_count, ch.size(), verts, tris, out.size(), lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], animLen);
    cgltf_free(g);
    return 0;
}
