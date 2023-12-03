#pragma once

#include "utils/compiler.h"

GODOT_WARNING_DISABLE()
#include <godot_cpp/classes/canvas_layer.hpp>
GODOT_WARNING_RESTORE()
using namespace godot;

class DebugDrawManager : public CanvasLayer {
	GDCLASS(DebugDrawManager, CanvasLayer)
protected:
	static DebugDrawManager *singleton;

	String root_settings_section;
	const static char *s_initial_state;
	const static char *s_manager_aliases;
	const static char *s_dd2d_aliases;
	const static char *s_dd3d_aliases;

	static void _bind_methods();

	double log_flush_time = 0;
	bool debug_enabled = true;
	bool is_closing = false;
	bool is_current_scene_is_null = true;

	class DebugDraw3D *debug_draw_3d_singleton = nullptr;
	class DebugDraw2D *debug_draw_2d_singleton = nullptr;

	TypedArray<StringName> manager_aliases;
	TypedArray<StringName> dd2d_aliases;
	TypedArray<StringName> dd3d_aliases;

	Node *_get_current_scene();
	void _connect_scene_changed();
	void _on_scene_changed(bool _is_scene_null);

	void _integrate_into_engine();

	void _register_singleton_aliases(const TypedArray<StringName> &names, Object *instance);
	void _unregister_singleton_aliases(const TypedArray<StringName> &names);

#ifdef TOOLS_ENABLED
	void _try_to_update_cs_bindings();
#endif

public:
	static const char *s_extension_unloading;

	DebugDrawManager();
	~DebugDrawManager();

	static DebugDrawManager *get_singleton() {
		return singleton;
	};

	void clear_all();
	void set_debug_enabled(bool value);
	bool is_debug_enabled() const;

	void init();
	void deinit();
	virtual void _process(double delta) override;
};