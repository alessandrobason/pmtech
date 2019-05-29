#include "../example_common.h"
#include "../../shader_structs/forward_render.h"

using namespace put;
using namespace put::ecs;

pen::window_creation_params pen_window{
    1280,               // width
    720,                // height
    4,                  // MSAA samples
    "stencil_shadows"   // window title / process name
};

namespace {
    struct shadow_volume_vertex
    {
        vec4f pos;
        vec4f face_normal_0;
        vec4f face_normal_1;
    };
    
    struct shadow_volume_edge
    {
        vec4f pos_0;
        vec4f pos_1;
        vec4f face_normal_0;
        vec4f face_normal_1;
    };
    
    bool almost_equalf(vec4f v1, vec4f v2, f32 epsilon_sq)
    {
        if(dist2(v1, v2) < epsilon_sq)
            return true;
        
        return false;
    }
    
    u32 cube_entity = 0;
    u32 light_0 = 0;
    
    shadow_volume_edge* s_sve;
    geometry_resource s_sgr;
}

void render_stencil_shadows(const scene_view& view)
{
    ecs_scene* scene = view.scene;
    geometry_resource* gr = get_geometry_resource(PEN_HASH("cube"));
    gr = &s_sgr;
    
    pmfx::set_technique_perm(view.pmfx_shader, view.technique, 0);
    pen::renderer_set_constant_buffer(view.cb_view, 0, pen::CBUFFER_BIND_PS | pen::CBUFFER_BIND_VS);
    pen::renderer_set_constant_buffer(scene->cbuffer[cube_entity], 1, pen::CBUFFER_BIND_PS | pen::CBUFFER_BIND_VS);
    pen::renderer_set_constant_buffer(scene->forward_light_buffer, 3, pen::CBUFFER_BIND_PS);
    pen::renderer_set_vertex_buffer(gr->vertex_buffer, 0, gr->vertex_size, 0);
    pen::renderer_set_index_buffer(gr->index_buffer, gr->index_type, 0);
    pen::renderer_draw_indexed(gr->num_indices, 0, 0, PEN_PT_TRIANGLELIST);
}

void generate_edge_mesh(geometry_resource* gr, shadow_volume_edge** sve_out, geometry_resource* gr_out)
{
    vertex_model* vm = (vertex_model*)gr->cpu_vertex_buffer;
    u16* ib = (u16*)gr->cpu_index_buffer;
    
    static u32 k[] = {
        1, 2, 0
    };
    
    shadow_volume_edge* sve = nullptr;
    
    static const f32 epsilon = 0.1f;
    for(u32 i = 0; i < gr->num_indices; i+=3)
    {
        shadow_volume_edge e[3];
        for(u32 j = 0; j < 3; ++j)
        {
            e[j].pos_0 = vm[ib[i+j]].pos;
            e[j].pos_1 = vm[ib[i+k[j]]].pos;
        }
        
        vec3f fn = maths::get_normal(e[0].pos_0.xyz, e[1].pos_0.xyz, e[2].pos_0.xyz);
        vec4f fn4 = vec4f(-fn, 1.0f);
        
        for(u32 j = 0; j < 3; ++j)
        {
            e[j].pos_0 = vm[ib[i+j]].pos;
            e[j].pos_1 = vm[ib[i+k[j]]].pos;
            
            s32 found = -1;
            u32 ne = sb_count(sve);
            for(u32 x = 0; x < ne; ++x)
            {
                if(almost_equal(sve[x].pos_0, e[j].pos_0, epsilon) && almost_equal(sve[x].pos_1, e[j].pos_1, epsilon)) {
                    found = x;
                    break;
                }
                
                if(almost_equal(sve[x].pos_1, e[j].pos_0, epsilon) && almost_equal(sve[x].pos_0, e[j].pos_1, epsilon)) {
                    found = x;
                    break;
                }
            }
            
            if(found == -1)
            {
                e[j].face_normal_0 = fn4;
                sb_push(sve, e[j]);
            }
            else
            {
                sve[found].face_normal_1 = fn4;
            }
        }
    }
    
    // for each edge add 4 vertices and 6 indices to make an extrable edge
    // the normals for each pair of verts are swapped to differentiate between them when extruding

    //     |
    // 0 ----- 1
    //
    // 2 ----- 3
    //     |

    shadow_volume_vertex* svv = nullptr;
    u16* sib = nullptr;
    
    u32 ne = sb_count(sve);
    u16 base_index = 0;
    for(u32 e = 0; e < ne; ++e)
    {
        shadow_volume_vertex v0;
        v0.pos = sve[e].pos_0;
        v0.face_normal_0 = sve[e].face_normal_0;
        v0.face_normal_1 = sve[e].face_normal_1;
        
        shadow_volume_vertex v1;
        v1.pos = sve[e].pos_1;
        v1.face_normal_0 = sve[e].face_normal_0;
        v1.face_normal_1 = sve[e].face_normal_1;
        
        shadow_volume_vertex v2;
        v2.pos = sve[e].pos_0;
        v2.face_normal_0 = sve[e].face_normal_1;
        v2.face_normal_1 = sve[e].face_normal_0;
        
        shadow_volume_vertex v3;
        v3.pos = sve[e].pos_1;
        v3.face_normal_0 = sve[e].face_normal_1;
        v3.face_normal_1 = sve[e].face_normal_0;
        
        sb_push(svv, v0);
        sb_push(svv, v1);
        sb_push(svv, v2);
        sb_push(svv, v3);
        
        sb_push(sib, base_index + 2);
        sb_push(sib, base_index + 1);
        sb_push(sib, base_index + 0);
        
        sb_push(sib, base_index + 2);
        sb_push(sib, base_index + 3);
        sb_push(sib, base_index + 1);
        
        base_index += 4;
    }
    
    // vb
    u32 num_verts = sb_count(svv);
    pen::buffer_creation_params bcp;
    bcp.usage_flags = PEN_USAGE_DEFAULT;
    bcp.bind_flags = PEN_BIND_VERTEX_BUFFER;
    bcp.cpu_access_flags = 0;
    bcp.buffer_size = sizeof(shadow_volume_vertex) * num_verts;
    bcp.data = (void*)svv;
    gr_out->vertex_buffer = pen::renderer_create_buffer(bcp);
    
    // ib
    u32 num_indices = sb_count(sib);
    bcp.usage_flags = PEN_USAGE_DEFAULT;
    bcp.bind_flags = PEN_BIND_INDEX_BUFFER;
    bcp.cpu_access_flags = 0;
    bcp.buffer_size = 2 * num_indices;
    bcp.data = (void*)sib;
    gr_out->index_buffer = pen::renderer_create_buffer(bcp);
    
    // info
    gr_out->num_indices = num_indices;
    gr_out->num_vertices = num_verts;
    gr_out->vertex_size = sizeof(shadow_volume_vertex);
    gr_out->index_type = PEN_FORMAT_R16_UINT;
    gr_out->min_extents = -vec3f::flt_max();
    gr_out->max_extents = vec3f::flt_max();
    gr_out->geometry_name = "shadow_mesh";
    gr_out->hash = PEN_HASH("shadow_mesh");
    gr_out->file_hash = PEN_HASH("shadow_mesh");
    gr_out->filename = "shadow_mesh";
    gr_out->p_skin = nullptr;
    
    // for debug purposes
    *sve_out = sve;
}

void example_setup(ecs::ecs_scene* scene, camera& cam)
{
    put::scene_view_renderer svr_stencil_shadow_volumes;
    svr_stencil_shadow_volumes.name = "ces_stencil_shadow_volumes";
    svr_stencil_shadow_volumes.id_name = PEN_HASH(svr_stencil_shadow_volumes.name.c_str());
    svr_stencil_shadow_volumes.render_function = &render_stencil_shadows;
    pmfx::register_scene_view_renderer(svr_stencil_shadow_volumes);
    
    pmfx::init("data/configs/stencil_shadows.jsn");
    
    clear_scene(scene);
    
    material_resource* default_material = get_material_resource(PEN_HASH("default_material"));
    
    geometry_resource* box = get_geometry_resource(PEN_HASH("cube"));
    
    // add light
    u32 light = get_new_entity(scene);
    scene->names[light] = "front_light";
    scene->id_name[light] = PEN_HASH("front_light");
    scene->lights[light].colour = vec3f::one();
    scene->lights[light].direction = vec3f::one();
    scene->lights[light].type = LIGHT_TYPE_DIR;
    scene->transforms[light].translation = vec3f::zero();
    scene->transforms[light].rotation = quat();
    scene->transforms[light].scale = vec3f::one();
    scene->entities[light] |= CMP_LIGHT;
    scene->entities[light] |= CMP_TRANSFORM;
    light_0 = light;
    
    // ground
    u32 ground = get_new_entity(scene);
    scene->names[ground] = "ground";
    scene->transforms[ground].translation = vec3f::zero();
    scene->transforms[ground].rotation = quat();
    scene->transforms[ground].scale = vec3f(50.0f, 1.0f, 50.0f);
    scene->entities[ground] |= CMP_TRANSFORM;
    scene->parents[ground] = ground;
    instantiate_geometry(box, scene, ground);
    instantiate_material(default_material, scene, ground);
    instantiate_model_cbuffer(scene, ground);
    
    // cube
    cube_entity = get_new_entity(scene);
    scene->names[cube_entity] = "cube";
    scene->transforms[cube_entity].translation = vec3f(0.0f, 11.0f, 0.0f);
    scene->transforms[cube_entity].rotation = quat();
    scene->transforms[cube_entity].scale = vec3f(10.0f, 10.0f, 10.0f);
    scene->entities[cube_entity] |= CMP_TRANSFORM;
    scene->parents[cube_entity] = cube_entity;
    instantiate_geometry(box, scene, cube_entity);
    instantiate_material(default_material, scene, cube_entity);
    instantiate_model_cbuffer(scene, cube_entity);
    
    generate_edge_mesh(box, &s_sve, &s_sgr);
}

void example_update(ecs::ecs_scene* scene, camera& cam, f32 dt)
{
    geometry_resource* box = get_geometry_resource(PEN_HASH("cube"));
    
    vertex_model* vm = (vertex_model*)box->cpu_vertex_buffer;
    mat4 wm = scene->world_matrices[cube_entity];
    
    for(u32 i = 0; i < box->num_vertices; ++i)
    {
        vec4f tp = wm.transform_vector(vm[i].pos);
        dbg::add_point(tp.xyz, 1.0f);
    }
    
    static s32 edge = 0;
    static bool isolate = false;
    ImGui::Checkbox("Isolate", &isolate);
    ImGui::InputInt("Edge", &edge);
    
    u32 ne = sb_count(s_sve);
    for(u32 j = 0; j < ne; ++j)
    {
        if(isolate)
            if(j != edge)
                continue;
        
        vec4f p0 = wm.transform_vector(s_sve[j].pos_0);
        vec4f p1 = wm.transform_vector(s_sve[j].pos_1);
        
        vec3f c = p0.xyz + (p1.xyz - p0.xyz) * 0.5f;
        
        dbg::add_line(c, c + s_sve[j].face_normal_0.xyz, vec4f::red());
        dbg::add_line(c, c + s_sve[j].face_normal_1.xyz, vec4f::magenta());
        
        vec3f ld = -vec3f::one();
        f32 d0 = dot(ld, s_sve[j].face_normal_0.xyz);
        f32 d1 = dot(ld, s_sve[j].face_normal_1.xyz);
        
        if((d0 > 0.0f && d1 < 0.0f) || (d1 > 0.0f && d0 < 0.0f))
        {
            dbg::add_line(p0.xyz, p0.xyz + ld * 100.0f);
            dbg::add_line(p1.xyz, p1.xyz + ld * 100.0f);
            dbg::add_line(p0.xyz, p1.xyz, vec4f::white());
        }
        else
        {
            dbg::add_line(p0.xyz, p1.xyz, vec4f::green());
        }
    }
}
