#include "pen.h"
#include "renderer.h"
#include "timer.h"
#include "file_system.h"
#include "pen_string.h"
#include "loader.h"
#include "dev_ui.h"
#include "camera.h"
#include "debug_render.h"
#include "pmfx_controller.h"
#include "pmfx.h"
#include "pen_json.h"
#include "hash.h"
#include "str_utilities.h"
#include "input.h"
#include "ces/ces_scene.h"
#include "ces/ces_resources.h"
#include "ces/ces_editor.h"
#include "ces/ces_utilities.h"

using namespace put;
using namespace ces;

pen::window_creation_params pen_window
{
    1280,					    //width
    720,					    //height
    4,						    //MSAA samples
    "shadows"		            //window title / process name
};

namespace physics
{
    extern PEN_THREAD_RETURN physics_thread_main( void* params );
}

void create_scene_objects( ces::entity_scene* scene )
{
    clear_scene( scene );

    material_resource* default_material = get_material_resource( PEN_HASH( "default_material" ) );
    geometry_resource* box_resource = get_geometry_resource(PEN_HASH("cube"));
    
    //add light
    u32 light = get_new_node( scene );
    scene->names[light] = "front_light";
    scene->id_name[light] = PEN_HASH( "front_light" );
    scene->lights[light].colour = vec3f::one();
    scene->transforms[light].translation = vec3f( 10000.0f, 10000.0f, 10000.0f );
    scene->transforms[light].rotation = quat();
    scene->transforms[light].scale = vec3f::one();
    scene->entities[light] |= CMP_LIGHT;
    scene->entities[light] |= CMP_TRANSFORM;

    //add ground
    f32 ground_size = 100.0f;
    u32 ground = get_new_node( scene );
    scene->transforms[ground].rotation = quat();
    scene->transforms[ground].scale = vec3f(ground_size, 1.0f, ground_size);
    scene->transforms[ground].translation = vec3f::zero();
    scene->parents[ground] = ground;
    scene->entities[ground] |= CMP_TRANSFORM;
    
    instantiate_geometry( box_resource, scene, ground );
    instantiate_material( default_material, scene, ground );
    instantiate_model_cbuffer( scene, ground );
    
    //add some pillars for shadow casters
    f32 num_pillar_rows = 5;
    f32 pillar_size = 20.0f;
    f32 d = ground_size * 0.5f;
    vec3f start_pos = vec3f( -d, pillar_size, -d );
    vec3f pos = start_pos;
    for( s32 i = 0; i < num_pillar_rows; ++i )
    {
        pos.z = start_pos.z;
        
        for( s32 j = 0; j < num_pillar_rows; ++j )
        {
            u32 pillar = get_new_node( scene );
            scene->transforms[pillar].rotation = quat();
            scene->transforms[pillar].scale = vec3f(2.0f, pillar_size, 2.0f);
            scene->transforms[pillar].translation = pos;
            scene->parents[pillar] = pillar;
            scene->entities[pillar] |= CMP_TRANSFORM;
            
            instantiate_geometry( box_resource, scene, pillar );
            instantiate_material( default_material, scene, pillar );
            instantiate_model_cbuffer( scene, pillar );
            
            pos.z += d / 2;
        }
        
        pos.x += d / 2;
    }
}

//
mat4 g_shadow_view;
mat4 g_shadow_proj;
u32  cbuffer_shadow = 0;

void debug_render_frustum( const scene_view& view )
{
    put::dbg::add_aabb(view.scene->renderable_extents.min, view.scene->renderable_extents.max, vec4f::magenta());
    put::dbg::render_3d(view.cb_view);
    
    dbg::add_coord_space(g_shadow_view, 10.0f);
}

void get_aabb_corners( vec3f* corners, vec3f min, vec3f max )
{
    static const vec3f offsets[8] =
    {
        vec3f::zero(),
        vec3f::one(),
        
        vec3f::unit_x(),
        vec3f::unit_y(),
        vec3f::unit_z(),
        
        vec3f( 1.0f, 0.0f, 1.0f ),
        vec3f( 1.0f, 1.0f, 0.0f ),
        vec3f( 0.0f, 1.0f, 1.0f )
    };
    
    vec3f size = max - min;
    for( s32 i = 0; i < 8; ++i )
    {
        corners[i] = min + offsets[i] * size;
    }
}

void fit_directional_shadow_to_aabb( put::camera* shadow_cam, vec3f light_dir, vec3f min, vec3f max )
{
    //create view matrix
    vec3f right = maths::cross(light_dir, vec3f::unit_y());
    vec3f up = maths::cross(right, light_dir);
    
    mat4 shadow_view;
    shadow_view.set_vectors(right, up, light_dir, vec3f::zero());
    
    //get corners
    vec3f corners[8];
    get_aabb_corners(&corners[0], min, max);
    
    //calculate extents in shadow space
    vec3f cmin = vec3f::flt_max();
    vec3f cmax = -vec3f::flt_max();
    for( s32 i = 0; i < 8; ++i )
    {
        vec3f p = shadow_view.transform_vector(corners[i]);
        
        cmin = vec3f::vmin(cmin, p);
        cmax = vec3f::vmax(cmax, p);
    }
    
    //shadow_view.set_vectors(right, up, light_dir, -light_dir * (cmax.z - cmin.z));
    g_shadow_view = shadow_view;
    
    //create ortho mat and set view matrix
    shadow_cam->view = shadow_view;
    shadow_cam->proj = mat4::create_orthographic_projection
    (
        cmin.x, cmax.x,
        cmin.y, cmax.y,
        300, -100
    );
    shadow_cam->flags |= CF_INVALIDATED;
}

void update_shadow_frustum( ces::entity_scene* scene, put::camera* shadow_cam )
{
    static hash_id id_shadow_map = PEN_HASH("shadow_map");
    static hash_id id_wrap_linear = PEN_HASH("wrap_linear");
    
    //static hash_id id_main_cam = PEN_HASH("model_viewer_camera");
    //const camera* main_cam = pmfx::get_camera(id_main_cam);
    
    vec3f light_dir = maths::normalise(-scene->transforms[0].translation);
    
    fit_directional_shadow_to_aabb
    (
        shadow_cam,
        light_dir,
        scene->renderable_extents.min,
        scene->renderable_extents.max
    );
    
    if(!cbuffer_shadow)
    {
        pen::buffer_creation_params bcp;
        bcp.usage_flags = PEN_USAGE_DYNAMIC;
        bcp.bind_flags = PEN_BIND_CONSTANT_BUFFER;
        bcp.cpu_access_flags = PEN_CPU_ACCESS_WRITE;
        bcp.buffer_size = sizeof(mat4);
        bcp.data = nullptr;
        
        cbuffer_shadow = pen::renderer_create_buffer(bcp);
    }
    
    mat4 shadow_vp = shadow_cam->proj * shadow_cam->view;
    
    pen::renderer_update_buffer(cbuffer_shadow, &shadow_vp, sizeof(mat4));
    
    u32 shadow_map = pmfx::get_render_target(id_shadow_map)->handle;
    u32 ss = pmfx::get_render_state_by_name(id_wrap_linear);
    
    pen::renderer_set_texture(shadow_map, ss, 15, PEN_SHADER_TYPE_PS);
    pen::renderer_set_constant_buffer(cbuffer_shadow, 4, PEN_SHADER_TYPE_PS);
}

void uu(put::camera_controller* cc)
{
    
}

PEN_THREAD_RETURN pen::game_entry( void* params )
{
    //unpack the params passed to the thread and signal to the engine it ok to proceed
    pen::job_thread_params* job_params = (pen::job_thread_params*)params;
    pen::job_thread* p_thread_info = job_params->job_thread_info;
    pen::threads_semaphore_signal(p_thread_info->p_sem_continue, 1);
    
    pen::threads_create_job( physics::physics_thread_main, 1024*10, nullptr, pen::THREAD_START_DETACHED );
    
	put::dev_ui::init();
	put::dbg::init();
    
	//create main camera and controller
	put::camera main_camera;
	put::camera_create_perspective( &main_camera, 60.0f, (f32)pen_window.width / (f32)pen_window.height, 0.1f, 1000.0f );
    
    put::camera_controller cc;
    cc.camera = &main_camera;
    cc.update_function = &ces::update_model_viewer_camera;
    cc.name = "model_viewer_camera";
    cc.id_name = PEN_HASH(cc.name.c_str());
    
    //create shadow camera and controller
    put::camera shadow_camera;
    
    put::camera_controller shadow_cc;
    shadow_cc.camera = &shadow_camera;
    shadow_cc.update_function = &uu;
    shadow_cc.name = "shadow_camera";
    shadow_cc.id_name = PEN_HASH(shadow_cc.name.c_str());
    
    //create the main scene and controller
    put::ces::entity_scene* main_scene = put::ces::create_scene("main_scene");
    put::ces::editor_init( main_scene );
    
    put::scene_controller sc;
    sc.scene = main_scene;
    sc.update_function = &ces::update_model_viewer_scene;
    sc.name = "main_scene";
	sc.camera = &main_camera;
    sc.id_name = PEN_HASH(sc.name.c_str());
    
    //create view renderers
    put::scene_view_renderer svr_main;
    svr_main.name = "ces_render_scene";
    svr_main.id_name = PEN_HASH(svr_main.name.c_str());
    svr_main.render_function = &ces::render_scene_view;
    
    put::scene_view_renderer svr_editor;
    svr_editor.name = "ces_render_editor";
    svr_editor.id_name = PEN_HASH(svr_editor.name.c_str());
    svr_editor.render_function = &ces::render_scene_editor;
    
    put::scene_view_renderer svr_debug;
    svr_debug.name = "shadows_debug";
    svr_debug.id_name = PEN_HASH(svr_editor.name.c_str());
    svr_debug.render_function = &debug_render_frustum;
    
    pmfx::register_scene_view_renderer(svr_main);
    pmfx::register_scene_view_renderer(svr_editor);
    pmfx::register_scene_view_renderer(svr_debug);

    pmfx::register_scene(sc);
    
    pmfx::register_camera(cc);
    pmfx::register_camera(shadow_cc);
    
    pmfx::init("data/configs/shadows.json");

    create_scene_objects( main_scene );
    
    bool enable_dev_ui = true;
    f32 frame_time = 0.0f;
    
    while( 1 )
    {
        f32 start = pen::timer_get_time();
        
		put::dev_ui::new_frame();
        
        pmfx::update();
        
        update_shadow_frustum(main_scene, &shadow_camera);
        
        pmfx::render();
        
        pmfx::show_dev_ui();
        
        if( enable_dev_ui )
        {
            put::dev_ui::console();
            put::dev_ui::render();
        }
        
        if( pen::input_is_key_held(PENK_MENU) && pen::input_is_key_pressed(PENK_D) )
            enable_dev_ui = !enable_dev_ui;
        
        frame_time = pen::timer_get_time() - start;
        
        pen::renderer_present();
        pen::renderer_consume_cmd_buffer();
        
		pmfx::poll_for_changes();
		put::poll_hot_loader();

        //msg from the engine we want to terminate
        if( pen::threads_semaphore_try_wait( p_thread_info->p_sem_exit ) )
            break;
    }

	ces::destroy_scene(main_scene);
	ces::editor_shutdown();
    
    //clean up mem here
	put::pmfx::shutdown();
	put::dbg::shutdown();
	put::dev_ui::shutdown();

    pen::renderer_consume_cmd_buffer();
    
    //signal to the engine the thread has finished
    pen::threads_semaphore_signal( p_thread_info->p_sem_terminated, 1);
    
    return PEN_THREAD_OK;
}
