#include "debug_draw_manager.h"

#include "2d/debug_draw_2d.h"
#include "2d/grouped_text.h"
#include "3d/debug_draw_3d.h"
#include "utils/utils.h"

#ifdef TOOLS_ENABLED
#include "editor/generate_csharp_bindings.h"
#endif

GODOT_WARNING_DISABLE()
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/file_access.hpp>
GODOT_WARNING_RESTORE()
using namespace godot;

DebugDrawManager *DebugDrawManager::singleton = nullptr;

const char *DebugDrawManager::s_initial_state = "initial_debug_state";
const char *DebugDrawManager::s_manager_aliases = NAMEOF(DebugDrawManager) "_singleton_aliases ";
const char *DebugDrawManager::s_dd2d_aliases = NAMEOF(DebugDraw2D) "_singleton_aliases";
const char *DebugDrawManager::s_dd3d_aliases = NAMEOF(DebugDraw3D) "_singleton_aliases";

const char *DebugDrawManager::s_extension_unloading = "extension_unloading";

void DebugDrawManager::_bind_methods() {
#define REG_CLASS_NAME DebugDrawManager

	// TODO use CALLABLE_MP in 4.2!
	ClassDB::bind_method(D_METHOD(NAMEOF(_integrate_into_engine)), &DebugDrawManager::_integrate_into_engine);
	ClassDB::bind_method(D_METHOD(NAMEOF(_on_scene_changed)), &DebugDrawManager::_on_scene_changed);

	ClassDB::bind_method(D_METHOD(NAMEOF(clear_all)), &DebugDrawManager::clear_all);

	REG_PROP_BOOL(debug_enabled);

	ADD_SIGNAL(MethodInfo(s_extension_unloading));

#undef REG_CLASS_NAME
}

Node *DebugDrawManager::_get_current_scene() {
#ifndef DISABLE_DEBUG_RENDERING
	auto ST = SCENE_TREE();
	if (IS_EDITOR_HINT()) {
		return ST->get_edited_scene_root();
	}
	return ST->get_current_scene();
#else
	return nullptr;
#endif
}

void DebugDrawManager::_connect_scene_changed() {
	ZoneScoped;
#ifndef DISABLE_DEBUG_RENDERING
	// Skip when exiting the tree and finish this loop
	if (is_closing) {
		return;
	}

	Node *scene = _get_current_scene();
	if (scene && UtilityFunctions::is_instance_valid(scene)) {
		scene->connect(StringName("tree_exiting"), Callable(this, NAMEOF(_on_scene_changed)).bindv(Array::make(false)), CONNECT_ONE_SHOT | CONNECT_DEFERRED);
		return;
	}

	SCENE_TREE()->connect(StringName("tree_changed"), Callable(this, NAMEOF(_on_scene_changed)).bindv(Array::make(true)), CONNECT_ONE_SHOT | CONNECT_DEFERRED);
#endif
}

void DebugDrawManager::_on_scene_changed(bool _is_scene_null) {
	ZoneScoped;
#ifndef DISABLE_DEBUG_RENDERING
	if (!is_current_scene_is_null || is_current_scene_is_null != _is_scene_null) {
		DEV_PRINT("Scene changed! clear_all()");
		debug_draw_3d_singleton->clear_all();
		debug_draw_2d_singleton->clear_all();
	}

	is_current_scene_is_null = _is_scene_null;
	_connect_scene_changed();
#endif
}

void DebugDrawManager::_integrate_into_engine() {
	ZoneScoped;
	// Assigned here because it is created inside the `GDExtension Initialize` function.
	// 1. Create in Initialize and assign Singleton
	// 2. Call class constructor after Initialize to get default values of properties
	// but singleton was already assigned..
	// Other classes will not face similar problems
	ASSIGN_SINGLETON(DebugDrawManager);

	SCENE_ROOT()->add_child(this);
	SCENE_ROOT()->move_child(this, 0);

	set_process(true);

	debug_draw_3d_singleton->init(this);
	debug_draw_2d_singleton->init(this);

#ifndef DISABLE_DEBUG_RENDERING
	// Useful nodes and names:
	// CanvasItemEditor - probably 2D editor viewport
	// Node3DEditorViewportContainer - base 3D viewport
	// Node3DEditorViewportContainer/Node3DEditorViewport - base of 1 of 4 viewports
	// Node3DEditorViewportContainer/Node3DEditorViewport/SubViewportContainer/SubViewport/Camera3D

	// Funny hack to get default font
	{
		Control *c = memnew(Control);
		SCENE_ROOT()->add_child(c);
		debug_draw_2d_singleton->_font = c->get_theme_default_font();
		c->queue_free();
	}

	// Setup default text group
	debug_draw_2d_singleton->grouped_text->end_text_group();

	if (IS_EDITOR_HINT()) {
		String editor3d = "Node3DEditorViewportContainer";
		String subviewport = SubViewport::get_class_static();
		Node *res = Utils::find_node_by_class(SCENE_ROOT(), editor3d);

		Node *n = res->get_child(0)->get_child(0);
		n->set_meta("UseParentSize", true);
		debug_draw_2d_singleton->default_canvas = (Control *)n;

		// actual tree for godot 4.0 beta 14
		//
		//	0. VSplitContainer
		//		0. Node3DEditorViewportContainer >>> res
		//			0. Node3DEditorViewport
		//				0. SubViewportContainer >>> res->get_child(0)->get_child(0)
		//					0. SubViewport
		//						0. Camera3D
		//				1. ...
		//			1. Node3DEditorViewport
		//				0. SubViewportContainer
		//					0. SubViewport
		//  ......

		std::vector<SubViewport *> editor_viewports;
		TypedArray<Node> viewports_root = res->get_children();
		for (int i = 0; i < viewports_root.size(); i++) {
			Node *node = cast_to<Node>(viewports_root[i]);
			if (node) {
				SubViewport *sub_view = cast_to<SubViewport>(Utils::find_node_by_class(node, subviewport));
				if (sub_view) {
					editor_viewports.push_back(sub_view);
				}
			}
		}
		debug_draw_3d_singleton->set_custom_editor_viewport(editor_viewports);

		/*
		// Used to explore the editor tree.
		auto f = FileAccess::open("user://tree.txt", FileAccess::WRITE);
		f->store_string(Utils::get_scene_tree_as_string(res->get_parent()->get_parent()));
		*/
	} else {
		set_layer(64);

		auto default_canvas = memnew(Control);
		default_canvas->set_name("DebugDrawDefaultCanvas");
		((Control *)default_canvas)->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
		((Control *)default_canvas)->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		debug_draw_2d_singleton->default_canvas = default_canvas;

		add_child(default_canvas);
	}

	debug_draw_2d_singleton->set_custom_canvas(debug_draw_2d_singleton->custom_canvas);
	debug_draw_3d_singleton->_set_base_world_node(SCENE_ROOT());
	_connect_scene_changed();
#endif

#ifdef TOOLS_ENABLED
	if (IS_EDITOR_HINT())
		_try_to_update_cs_bindings();
#endif
}

DebugDrawManager::DebugDrawManager() {
}

DebugDrawManager::~DebugDrawManager() {
	UNASSIGN_SINGLETON(DebugDrawManager);
	deinit();
}

void DebugDrawManager::clear_all() {
	ZoneScoped;
	if (debug_draw_2d_singleton)
		debug_draw_2d_singleton->clear_all();
	if (debug_draw_3d_singleton)
		debug_draw_3d_singleton->clear_all();
}

void DebugDrawManager::set_debug_enabled(bool value) {
	debug_enabled = value;
	if (!value) {
		clear_all();
	}
}

bool DebugDrawManager::is_debug_enabled() const {
	return debug_enabled;
}

void DebugDrawManager::init() {
	ZoneScoped;
	DEFINE_SETTING_AND_GET(bool initial_debug_state, String(Utils::root_settings_section) + s_initial_state, true, Variant::BOOL);
	set_debug_enabled(initial_debug_state);

	manager_aliases.push_back(StringName("Dmgr"));
	dd2d_aliases.push_back(StringName("Dbg2"));
	dd3d_aliases.push_back(StringName("Dbg3"));

	root_settings_section = String(Utils::root_settings_section) + "common/";
	DEFINE_SETTING_AND_GET_HINT(Variant mgr_a, root_settings_section + s_manager_aliases, manager_aliases, Variant::ARRAY, PropertyHint::PROPERTY_HINT_TYPE_STRING, FMT_STR("{0}:", Variant::STRING_NAME));
	DEFINE_SETTING_AND_GET_HINT(Variant dd2d_a, root_settings_section + s_dd2d_aliases, dd2d_aliases, Variant::ARRAY, PropertyHint::PROPERTY_HINT_TYPE_STRING, FMT_STR("{0}:", Variant::STRING_NAME));
	DEFINE_SETTING_AND_GET_HINT(Variant dd3d_a, root_settings_section + s_dd3d_aliases, dd3d_aliases, Variant::ARRAY, PropertyHint::PROPERTY_HINT_TYPE_STRING, FMT_STR("{0}:", Variant::STRING_NAME));

	manager_aliases = TypedArray<StringName>(mgr_a);
	dd2d_aliases = TypedArray<StringName>(dd2d_a);
	dd3d_aliases = TypedArray<StringName>(dd3d_a);

	// Draw everything after calls from scripts to avoid lagging
	set_process_priority(INT32_MAX);

	Engine::get_singleton()->register_singleton(NAMEOF(DebugDrawManager), this);
	_register_singleton_aliases(manager_aliases, this);

	debug_draw_2d_singleton = memnew(DebugDraw2D);
	Engine::get_singleton()->register_singleton(NAMEOF(DebugDraw2D), debug_draw_2d_singleton);
	_register_singleton_aliases(dd2d_aliases, debug_draw_2d_singleton);

	debug_draw_3d_singleton = memnew(DebugDraw3D);
	Engine::get_singleton()->register_singleton(NAMEOF(DebugDraw3D), debug_draw_3d_singleton);
	_register_singleton_aliases(dd3d_aliases, debug_draw_3d_singleton);

	call_deferred(NAMEOF(_integrate_into_engine));
}

void DebugDrawManager::deinit() {
	ZoneScoped;
	is_closing = true;

	if (Engine::get_singleton()->has_singleton(NAMEOF(DebugDrawManager))) {
		Engine::get_singleton()->unregister_singleton(NAMEOF(DebugDrawManager));
		_unregister_singleton_aliases(manager_aliases);
	}

	if (debug_draw_2d_singleton) {
		Engine::get_singleton()->unregister_singleton(NAMEOF(DebugDraw2D));
		_unregister_singleton_aliases(dd2d_aliases);
		memdelete(debug_draw_2d_singleton);
		debug_draw_2d_singleton = nullptr;
	}

	if (debug_draw_3d_singleton) {
		Engine::get_singleton()->unregister_singleton(NAMEOF(DebugDraw3D));
		_unregister_singleton_aliases(dd3d_aliases);
		memdelete(debug_draw_3d_singleton);
		debug_draw_3d_singleton = nullptr;
	}

	emit_signal(s_extension_unloading);
}

void DebugDrawManager::_register_singleton_aliases(const TypedArray<StringName> &names, Object *instance) {
	for (int i = 0; i < names.size(); i++) {
		if (!names[i].operator godot::StringName().is_empty())
			Engine::get_singleton()->register_singleton(names[i], instance);
	}
}

void DebugDrawManager::_unregister_singleton_aliases(const TypedArray<StringName> &names) {
	for (int i = 0; i < names.size(); i++) {
		if (!names[i].operator godot::StringName().is_empty())
			Engine::get_singleton()->unregister_singleton(names[i]);
	}
}

void DebugDrawManager::_process(double delta) {
	// To discover what causes the constant look here:
	// https://github.com/godotengine/godot/blob/baf6b4634d08bc3e193a38b86e96945052002f64/servers/rendering/rendering_server_default.h#L104
	// Replace _changes_changed's body with:
	// __declspec(noinline) void RenderingServerDefault::_changes_changed() {
	//	#if 0
	//		auto *asd = memnew(Object);
	//		memdelete(asd);
	//	#endif
	//	}
#ifndef DISABLE_DEBUG_RENDERING
	if (debug_enabled) {
		DebugDraw3D::get_singleton()->process(get_process_delta_time());

		DebugDraw2D::get_singleton()->process(get_process_delta_time());

		if (!DebugDraw2D::get_singleton()->is_drawing_frame()) {
			FrameMark;
		}
	}
#endif

	log_flush_time += get_process_delta_time();
	if (log_flush_time > 0.25f) {
		log_flush_time -= 0.25f;
		Utils::print_logs();
	}

	if (!debug_enabled || !DebugDraw2D::get_singleton()->is_debug_enabled()) {
		FrameMark;
	}
}

#ifdef TOOLS_ENABLED
void DebugDrawManager::_try_to_update_cs_bindings() {
	ZoneScoped;
	auto g = GenerateCSharpBindingsPlugin();
	if (g.is_need_to_update()) {
		PRINT_WARNING("C# bindings for 'Debug Draw' were not found or are outdated!");
		g.generate();
	}
}
#endif
