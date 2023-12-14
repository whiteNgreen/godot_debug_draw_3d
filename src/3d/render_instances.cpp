#include "render_instances.h"

#ifndef DISABLE_DEBUG_RENDERING

#include "stats_3d.h"

GODOT_WARNING_DISABLE()
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/time.hpp>
GODOT_WARNING_RESTORE()

// TODO too big buffer constantly shrinking
template <class T, size_t _TStepSize>
struct TempBigBuffer {
private:
	T m_buffer;
	int m_accesses_before_shrink = (int)InstanceType::MAX * 60; // try to shrink after 60 frames
	int m_shrink_timer;

public:
	TempBigBuffer() {
		m_buffer.resize(_TStepSize);
		m_shrink_timer = 0;
	}

	void prepare_buffer(size_t t_expected_size) {
		ZoneScoped;
		if ((size_t)m_buffer.size() < t_expected_size) {
			size_t new_size = (size_t)Math::ceil(t_expected_size / (double)_TStepSize) * _TStepSize;
			DEV_PRINT_STD(NAMEOF(TempBigBuffer) ": extending from %d to %d\n", m_buffer.size(), new_size);
			m_buffer.resize(new_size);
			m_shrink_timer = 0;
		} else {
			if (t_expected_size < m_buffer.size() - _TStepSize) {
				m_shrink_timer++;
				if (m_shrink_timer >= m_accesses_before_shrink) {
					size_t new_size = (size_t)m_buffer.size() - _TStepSize;
					DEV_PRINT_STD(NAMEOF(TempBigBuffer) ": shrinking from %d to %d\n", m_buffer.size(), new_size);
					m_buffer.resize(new_size);
					m_shrink_timer = 0;
				}
			} else {
				m_shrink_timer = 0;
			}
		}
	}

	inline auto ptrw() {
		return m_buffer.ptrw();
	}

	// TODO stupid and slow as it makes a COPY of all memory..
	inline auto slice(int64_t begin, int64_t end = 2147483647) {
		return m_buffer.slice(begin, end);
	}
};

DelayedRendererInstance::DelayedRendererInstance() :
		DelayedRenderer() {
	DEV_PRINT_STD("New " NAMEOF(DelayedRendererInstance) " created\n");
}

void DelayedRendererInstance::update(const real_t &_exp_time, const Transform3D &_transform, const Color &_col, const Color &_custom_col, const SphereBounds &_bounds) {
	ZoneScoped;
	_update(_exp_time, true);

	bounds = _bounds;
	data = InstanceTransformAndData3D(_transform, _col, _custom_col);
}

DelayedRendererLine::DelayedRendererLine() :
		DelayedRenderer() {
	DEV_PRINT_STD("New " NAMEOF(DelayedRendererLine) " created\n");
}

void DelayedRendererLine::update(const real_t &_exp_time, const std::vector<Vector3> &_lines, const Color &_col) {
	ZoneScoped;
	_update(_exp_time, true);

	set_lines(_lines);
	color = _col;
}

void DelayedRendererLine::set_lines(const std::vector<Vector3> &_lines) {
	lines = _lines;
	bounds = calculate_bounds_based_on_lines(lines);
}

const std::vector<Vector3> &DelayedRendererLine::get_lines() const {
	return lines;
}

AABB DelayedRendererLine::calculate_bounds_based_on_lines(const std::vector<Vector3> &_lines) {
	ZoneScoped;
	if (_lines.size() > 0) {
		// Using the original Godot expand_to code to avoid creating new AABB instances
		Vector3 begin = _lines[0];
		Vector3 end = _lines[0] + Vector3_ZERO;

		for (Vector3 v : _lines) {
			if (v.x < begin.x) {
				begin.x = v.x;
			}
			if (v.y < begin.y) {
				begin.y = v.y;
			}
			if (v.z < begin.z) {
				begin.z = v.z;
			}

			if (v.x > end.x) {
				end.x = v.x;
			}
			if (v.y > end.y) {
				end.y = v.y;
			}
			if (v.z > end.z) {
				end.z = v.z;
			}
		}

		return AABB(begin, end - begin);
	} else {
		return AABB();
	}
}

void GeometryPool::fill_instance_data(const std::array<Ref<MultiMesh> *, (int)InstanceType::MAX> &t_meshes) {
	ZoneScoped;
	time_spent_to_fill_buffers_of_instances = Time::get_singleton()->get_ticks_usec();

	for (size_t i = 0; i < t_meshes.size(); i++) {
		auto &mesh = *t_meshes[i];

		PackedFloat32Array a = get_raw_data((InstanceType)i);

		int new_size = (int)(a.size() / INSTANCE_DATA_FLOAT_COUNT);
		if (mesh->get_instance_count() != new_size) {
			mesh->set_instance_count(new_size);
			mesh->set_visible_instance_count(-1);
		}

		if (a.size()) {
			// TODO PR for native pointer!
			mesh->set_buffer(a);
		}
	}

	time_spent_to_fill_buffers_of_instances = Time::get_singleton()->get_ticks_usec() - time_spent_to_fill_buffers_of_instances;
}

PackedFloat32Array GeometryPool::get_raw_data(InstanceType _type) {
	ZoneScoped;
	thread_local static auto temp_buffer = TempBigBuffer<PackedFloat32Array, 128 * 1024>();
	size_t last_added = 0;
	size_t buffer_size = 0;

	{
		ZoneScopedN("Prepare buffer");
		for (auto &proc : pools) {
			auto &inst = proc.instances[(int)_type];
			buffer_size += (inst.used_instant + inst.delayed.size()) * INSTANCE_DATA_FLOAT_COUNT;
		}
		temp_buffer.prepare_buffer(buffer_size);
	}

	if (buffer_size > 0) {
		ZoneScopedN("Fill buffer");
		auto w = temp_buffer.ptrw();

		for (auto &proc : pools) {
			auto &inst = proc.instances[(int)_type];

			auto write_instance_data = [&last_added, &w](DelayedRendererInstance &o) {
				size_t id = last_added * INSTANCE_DATA_FLOAT_COUNT;
				last_added++;

				// 7500 instances. 1.2-1.3ms with the old approach and 0.8-0.9ms with the current approach
				memcpy(w + id, reinterpret_cast<real_t *>(&o.data), INSTANCE_DATA_FLOAT_COUNT * sizeof(real_t));
			};

			for (size_t i = 0; i < inst.used_instant; i++) {
				auto &o = inst.instant[i];
				o.is_used_one_time = true;
				if (o.is_visible) {
					write_instance_data(o);
				}
			}
			inst._prev_used_instant = inst.used_instant;

			inst.used_delayed = 0;
			for (size_t i = 0; i < inst.delayed.size(); i++) {
				auto &o = inst.delayed[i];
				if (!o.is_expired()) {
					inst.used_delayed++;
					o.is_used_one_time = true;
					if (o.is_visible) {
						write_instance_data(o);
					}
				}
			}
		}
	}

	return temp_buffer.slice(0, last_added * INSTANCE_DATA_FLOAT_COUNT);
}

void GeometryPool::fill_lines_data(Ref<ArrayMesh> _ig) {
	ZoneScoped;
	time_spent_to_fill_buffers_of_lines = 0;

	uint64_t used_lines = 0;
	for (auto &proc : pools) {
		used_lines += proc.lines.used_instant;
		used_lines += proc.lines.delayed.size();
	}
	if (used_lines == 0) {
		return;
	}

	size_t spent_timer = Time::get_singleton()->get_ticks_usec();

	// Avoiding a large number of resizes increased the speed from 1.9-2.0ms to 1.4-1.5ms
	size_t used_vertices = 0;

	// pre calculate buffer size
	for (auto &proc : pools) {
		for (size_t i = 0; i < proc.lines.used_instant; i++) {
			auto &o = proc.lines.instant[i];
			if (o.is_visible) {
				used_vertices += o.get_lines().size();
			}
		}
		for (size_t i = 0; i < proc.lines.delayed.size(); i++) {
			auto &o = proc.lines.delayed[i];
			if (o.is_visible && !o.is_expired()) {
				used_vertices += o.get_lines().size();
			}
		}
	}

	thread_local static auto verices_buffer = TempBigBuffer<PackedVector3Array, 64 * 1024>();
	thread_local static auto colors_buffer = TempBigBuffer<PackedColorArray, 64 * 1024>();
	verices_buffer.prepare_buffer(used_vertices);
	colors_buffer.prepare_buffer(used_vertices);

	size_t prev_pos = 0;
	auto vertices_write = verices_buffer.ptrw();
	auto colors_write = colors_buffer.ptrw();

	auto append_lines = [&vertices_write, &colors_write, &prev_pos](const DelayedRendererLine &o) {
		size_t lines_size = o.get_lines().size();

		std::copy(o.get_lines().begin(), o.get_lines().end(), vertices_write + prev_pos);
		std::fill(colors_write + prev_pos, colors_write + prev_pos + lines_size, o.color);

		prev_pos += lines_size;
	};

	for (auto &proc : pools) {
		for (size_t i = 0; i < proc.lines.used_instant; i++) {
			auto &o = proc.lines.instant[i];
			if (o.is_visible) {
				append_lines(o);
			}
			o.is_used_one_time = true;
		}
		proc.lines._prev_used_instant = proc.lines.used_instant;

		proc.lines.used_delayed = 0;
		for (size_t i = 0; i < proc.lines.delayed.size(); i++) {
			auto &o = proc.lines.delayed[i];
			if (!o.is_expired()) {
				proc.lines.used_delayed++;
				if (o.is_visible) {
					append_lines(o);
				}
			}
			o.is_used_one_time = true;
		}
	}

	if (used_vertices > 1) {
		Array mesh = Array();
		mesh.resize(ArrayMesh::ArrayType::ARRAY_MAX);
		mesh[ArrayMesh::ArrayType::ARRAY_VERTEX] = verices_buffer.slice(0, used_vertices);
		mesh[ArrayMesh::ArrayType::ARRAY_COLOR] = colors_buffer.slice(0, used_vertices);

		_ig->add_surface_from_arrays(Mesh::PrimitiveType::PRIMITIVE_LINES, mesh);
	}

	time_spent_to_fill_buffers_of_lines = Time::get_singleton()->get_ticks_usec() - spent_timer;
}

void GeometryPool::reset_counter(const double &_delta, const ProcessType &p_proc) {
	ZoneScoped;
	if (p_proc == ProcessType::MAX) {
		for (auto &proc : pools) {
			for (int i = 0; i < (int)InstanceType::MAX; i++) {
				proc.instances[i].reset_counter(_delta, i);
			}
			proc.lines.reset_counter(_delta);
		}
	} else {
		for (int i = 0; i < (int)InstanceType::MAX; i++) {
			pools[(int)p_proc].instances[i].reset_counter(_delta, i);
		}
		pools[(int)p_proc].lines.reset_counter(_delta);
	}
}

void GeometryPool::reset_visible_objects() {
	ZoneScoped;
	for (auto &proc : pools) {
		for (auto &i : proc.instances) {
			i.reset_visible_counter();
		}
		proc.lines.reset_visible_counter();
	}
}

void GeometryPool::update_stats(Ref<DebugDrawStats3D> &stats) const {
	ZoneScoped;

	size_t used_instances = 0;
	size_t used_lines = 0;
	size_t visible_instances = 0;
	size_t visible_lines = 0;

	for (auto &proc : pools) {
		for (auto &i : proc.instances) {
			used_instances += i._prev_used_instant;
			used_instances += i.used_delayed;
		}

		for (auto &i : proc.instances) {
			visible_instances += i.visible_objects;
		}

		used_lines += proc.lines._prev_used_instant + proc.lines.used_delayed;
		visible_lines += proc.lines.visible_objects;
	}

	stats->set_render_stats(
			/* t_instances */ used_instances,
			/* t_lines */ used_lines,

			/* t_visible_instances */ visible_instances,
			/* t_visible_lines */ visible_lines,

			/* t_time_filling_buffers_instances_usec */ time_spent_to_fill_buffers_of_instances,
			/* t_time_filling_buffers_lines_usec */ time_spent_to_fill_buffers_of_lines,

			/* t_time_culling_instant_usec */ time_spent_to_cull_instant,
			/* t_time_culling_delayed_usec */ time_spent_to_cull_delayed);
}

void GeometryPool::clear_pool() {
	ZoneScoped;
	for (auto &proc : pools) {
		for (auto &i : proc.instances) {
			i.clear_pools();
		}
		proc.lines.clear_pools();
	}
}

void GeometryPool::for_each_instance(const std::function<void(DelayedRendererInstance *)> &_func) {
	ZoneScoped;
	for (auto &proc : pools) {
		for (auto &inst : proc.instances) {
			for (size_t i = 0; i < inst.used_instant; i++) {
				_func(&inst.instant[i]);
			}
			for (size_t i = 0; i < inst.delayed.size(); i++) {
				if (!inst.delayed[i].is_expired())
					_func(&inst.delayed[i]);
			}
		}
	}
}

void GeometryPool::for_each_line(const std::function<void(DelayedRendererLine *)> &_func) {
	ZoneScoped;
	for (auto &proc : pools) {
		for (size_t i = 0; i < proc.lines.used_instant; i++) {
			_func(&proc.lines.instant[i]);
		}
		for (size_t i = 0; i < proc.lines.delayed.size(); i++) {
			if (!proc.lines.delayed[i].is_expired())
				_func(&proc.lines.delayed[i]);
		}
	}
}

void GeometryPool::update_visibility(const std::vector<std::vector<Plane> > &t_frustums, const GeometryPoolDistanceCullingData &t_distance_data) {
	ZoneScoped;

	uint64_t dt_i = 0, dt_d = 0;
	for (auto &proc : pools) {
		uint64_t instant_time = 0;
		uint64_t delayed_time = 0;
		for (auto &t : proc.instances) { // loop over instance types
			uint64_t i_t = Time::get_singleton()->get_ticks_usec();

			for (size_t i = 0; i < t.used_instant; i++)
				t.instant[i].update_visibility(t_frustums, t_distance_data, true);

			instant_time += Time::get_singleton()->get_ticks_usec() - i_t;
			uint64_t d_t = Time::get_singleton()->get_ticks_usec();

			for (size_t i = 0; i < t.delayed.size(); i++)
				t.delayed[i].update_visibility(t_frustums, t_distance_data, false);

			delayed_time += Time::get_singleton()->get_ticks_usec() - d_t;
		}

		// loop over lines
		uint64_t instant_dt = Time::get_singleton()->get_ticks_usec();

		for (size_t i = 0; i < proc.lines.used_instant; i++)
			proc.lines.instant[i].update_visibility(t_frustums, t_distance_data, true);

		instant_dt = Time::get_singleton()->get_ticks_usec() - instant_dt + instant_time;
		uint64_t delayed_dt = Time::get_singleton()->get_ticks_usec();

		for (size_t i = 0; i < proc.lines.delayed.size(); i++)
			proc.lines.delayed[i].update_visibility(t_frustums, t_distance_data, false);

		delayed_dt = Time::get_singleton()->get_ticks_usec() - delayed_dt + delayed_time;
	}
	time_spent_to_cull_instant = dt_i;
	time_spent_to_cull_delayed = dt_d;
}

void GeometryPool::update_expiration(const double &_delta, const ProcessType &p_proc) {
	ZoneScoped;
	auto &proc = pools[(int)p_proc];

	if (p_proc == ProcessType::PHYSICS_PROCESS) {
		for (auto &t : proc.instances) {
			for (size_t i = 0; i < t.delayed.size(); i++) {
				auto &o = t.delayed[i];
				if (o.is_used_one_time) {
					o.update_expiration(_delta);
				}
			}
		}

		for (size_t i = 0; i < proc.lines.delayed.size(); i++) {
			auto &o = proc.lines.delayed[i];
			if (o.is_used_one_time) {
				o.update_expiration(_delta);
			}
		}
	} else {
		for (auto &t : proc.instances) {
			for (size_t i = 0; i < t.delayed.size(); i++) {
				t.delayed[i].update_expiration(_delta);
			}
		}

		for (size_t i = 0; i < proc.lines.delayed.size(); i++) {
			proc.lines.delayed[i].update_expiration(_delta);
		}
	}
}

void GeometryPool::scan_visible_instances() {
	ZoneScoped;
	reset_visible_objects();

	for (auto &proc : pools) {
		for (auto &t : proc.instances) {
			for (size_t i = 0; i < t.used_instant; i++)
				if (t.instant[i].is_visible)
					t.visible_objects++;
			for (size_t i = 0; i < t.delayed.size(); i++) {
				auto &o = t.delayed[i];
				if (o.is_visible && !o.is_expired())
					t.visible_objects++;
			}
		}

		for (size_t i = 0; i < proc.lines.used_instant; i++)
			if (proc.lines.instant[i].is_visible)
				proc.lines.visible_objects++;

		for (size_t i = 0; i < proc.lines.delayed.size(); i++) {
			auto &o = proc.lines.delayed[i];
			if (o.is_visible && !o.is_expired())
				proc.lines.visible_objects++;
		}
	}
}

void GeometryPool::add_or_update_instance(InstanceType _type, const real_t &_exp_time, const ProcessType &p_proc, const Transform3D &_transform, const Color &_col, const Color &_custom_col, const SphereBounds &_bounds, const std::function<void(DelayedRendererInstance *)> &_custom_upd) {
	ZoneScoped;
	DelayedRendererInstance *inst = pools[(int)p_proc].instances[(int)_type].get(_exp_time > 0);
	inst->update(_exp_time, _transform, _col, _custom_col, _bounds);
	if (_custom_upd)
		_custom_upd(inst);
}

void GeometryPool::add_or_update_line(const real_t &_exp_time, const ProcessType &p_proc, const std::vector<Vector3> &_lines, const Color &_col, const std::function<void(DelayedRendererLine *)> _custom_upd) {
	ZoneScoped;
	DelayedRendererLine *inst = pools[(int)p_proc].lines.get(_exp_time > 0);
	inst->update(_exp_time, _lines, _col);
	if (_custom_upd)
		_custom_upd(inst);
}

#endif