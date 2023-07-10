/*************************************************************************/
/*  scene_synchronizer.cpp                                               */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                      https://godotengine.org                          */
/*************************************************************************/
/* Copyright (c) 2007-2021 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2021 Godot Engine contributors (cf. AUTHORS.md).   */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/

/**
	@author AndreaCatania
*/

#include "scene_synchronizer.h"

#include "client_synchronizer.h"
#include "core/method_bind_ext.gen.inc"
#include "core/os/os.h"
#include "input_network_encoder.h"
#include "networked_controller.h"
#include "no_net_synchronizer.h"
#include "scene_diff.h"
#include "scene_synchronizer_debugger.h"
#include "server_synchronizer.h"

#include "godot_backward_utility_cpp.h"

void SceneSynchronizer::_bind_methods() {
	BIND_ENUM_CONSTANT(CHANGE)
	BIND_ENUM_CONSTANT(SYNC_RECOVER)
	BIND_ENUM_CONSTANT(SYNC_RESET)
	BIND_ENUM_CONSTANT(SYNC_REWIND)
	BIND_ENUM_CONSTANT(END_SYNC)
	BIND_ENUM_CONSTANT(DEFAULT)
	BIND_ENUM_CONSTANT(SYNC)
	BIND_ENUM_CONSTANT(ALWAYS)

	ClassDB::bind_method(D_METHOD("reset_synchronizer_mode"), &SceneSynchronizer::reset_synchronizer_mode);
	ClassDB::bind_method(D_METHOD("clear"), &SceneSynchronizer::clear);

	ClassDB::bind_method(D_METHOD("set_server_notify_state_interval", "interval"), &SceneSynchronizer::set_server_notify_state_interval);
	ClassDB::bind_method(D_METHOD("get_server_notify_state_interval"), &SceneSynchronizer::get_server_notify_state_interval);

	ClassDB::bind_method(D_METHOD("set_comparison_float_tolerance", "tolerance"), &SceneSynchronizer::set_comparison_float_tolerance);
	ClassDB::bind_method(D_METHOD("get_comparison_float_tolerance"), &SceneSynchronizer::get_comparison_float_tolerance);

	ClassDB::bind_method(D_METHOD("set_actions_redundancy", "redundancy"), &SceneSynchronizer::set_actions_redundancy);
	ClassDB::bind_method(D_METHOD("get_actions_redundancy"), &SceneSynchronizer::get_actions_redundancy);

	ClassDB::bind_method(D_METHOD("set_actions_resend_time", "time"), &SceneSynchronizer::set_actions_resend_time);
	ClassDB::bind_method(D_METHOD("get_actions_resend_time"), &SceneSynchronizer::get_actions_resend_time);

	ClassDB::bind_method(D_METHOD("register_node", "node"), &SceneSynchronizer::register_node_gdscript);
	ClassDB::bind_method(D_METHOD("unregister_node", "node"), &SceneSynchronizer::unregister_node);
	ClassDB::bind_method(D_METHOD("get_node_id", "node"), &SceneSynchronizer::get_node_id);
	ClassDB::bind_method(D_METHOD("get_node_from_id", "id"), &SceneSynchronizer::get_node_from_id);

	ClassDB::bind_method(D_METHOD("register_variable", "node", "variable", "on_change_notify", "flags"), &SceneSynchronizer::register_variable, DEFVAL(StringName()), DEFVAL(NetEventFlag::DEFAULT));
	ClassDB::bind_method(D_METHOD("unregister_variable", "node", "variable"), &SceneSynchronizer::unregister_variable);
	ClassDB::bind_method(D_METHOD("get_variable_id", "node", "variable"), &SceneSynchronizer::get_variable_id);

	ClassDB::bind_method(D_METHOD("start_node_sync", "node"), &SceneSynchronizer::start_node_sync);
	ClassDB::bind_method(D_METHOD("stop_node_sync", "node"), &SceneSynchronizer::stop_node_sync);
	ClassDB::bind_method(D_METHOD("is_node_sync", "node"), &SceneSynchronizer::is_node_sync);

	ClassDB::bind_method(D_METHOD("register_action", "node", "action_func", "action_encoding_func", "can_client_trigger", "wait_server_validation", "server_action_validation_func"), &SceneSynchronizer::register_action, DEFVAL(false), DEFVAL(false), DEFVAL(StringName()));
	ClassDB::bind_method(D_METHOD("find_action_id", "node", "event_name"), &SceneSynchronizer::find_action_id);
	ClassDB::bind_method(D_METHOD("trigger_action_by_name", "node", "event_name", "arguments", "recipients_peers"), &SceneSynchronizer::trigger_action_by_name, DEFVAL(Array()), DEFVAL(Vector<int>()));
	ClassDB::bind_method(D_METHOD("trigger_action", "node", "action_id", "arguments", "recipients_peers"), &SceneSynchronizer::trigger_action, DEFVAL(Array()), DEFVAL(Vector<int>()));

	ClassDB::bind_method(D_METHOD("set_skip_rewinding", "node", "variable", "skip_rewinding"), &SceneSynchronizer::set_skip_rewinding);

	ClassDB::bind_method(D_METHOD("track_variable_changes", "node", "variable", "object", "method", "flags"), &SceneSynchronizer::track_variable_changes, DEFVAL(NetEventFlag::DEFAULT));
	ClassDB::bind_method(D_METHOD("untrack_variable_changes", "node", "variable", "object", "method"), &SceneSynchronizer::untrack_variable_changes);

	ClassDB::bind_method(D_METHOD("set_node_as_controlled_by", "node", "controller"), &SceneSynchronizer::set_node_as_controlled_by);

	ClassDB::bind_method(D_METHOD("controller_add_dependency", "controller", "node"), &SceneSynchronizer::controller_add_dependency);
	ClassDB::bind_method(D_METHOD("controller_remove_dependency", "controller", "node"), &SceneSynchronizer::controller_remove_dependency);
	ClassDB::bind_method(D_METHOD("controller_get_dependency_count", "controller"), &SceneSynchronizer::controller_get_dependency_count);
	ClassDB::bind_method(D_METHOD("controller_get_dependency", "controller", "index"), &SceneSynchronizer::controller_get_dependency);

	ClassDB::bind_method(D_METHOD("register_process", "node", "function"), &SceneSynchronizer::register_process);
	ClassDB::bind_method(D_METHOD("unregister_process", "node", "function"), &SceneSynchronizer::unregister_process);

	ClassDB::bind_method(D_METHOD("start_tracking_scene_changes", "diff_handle"), &SceneSynchronizer::start_tracking_scene_changes);
	ClassDB::bind_method(D_METHOD("stop_tracking_scene_changes", "diff_handle"), &SceneSynchronizer::stop_tracking_scene_changes);
	ClassDB::bind_method(D_METHOD("pop_scene_changes", "diff_handle"), &SceneSynchronizer::pop_scene_changes);
	ClassDB::bind_method(D_METHOD("apply_scene_changes", "sync_data"), &SceneSynchronizer::apply_scene_changes);

	ClassDB::bind_method(D_METHOD("is_recovered"), &SceneSynchronizer::is_recovered);
	ClassDB::bind_method(D_METHOD("is_resetted"), &SceneSynchronizer::is_resetted);
	ClassDB::bind_method(D_METHOD("is_rewinding"), &SceneSynchronizer::is_rewinding);
	ClassDB::bind_method(D_METHOD("is_end_sync"), &SceneSynchronizer::is_end_sync);

	ClassDB::bind_method(D_METHOD("force_state_notify"), &SceneSynchronizer::force_state_notify);

	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &SceneSynchronizer::set_enabled);
	ClassDB::bind_method(D_METHOD("set_peer_networking_enable", "peer", "enabled"), &SceneSynchronizer::set_peer_networking_enable);
	ClassDB::bind_method(D_METHOD("get_peer_networking_enable", "peer"), &SceneSynchronizer::is_peer_networking_enable);

	ClassDB::bind_method(D_METHOD("is_server"), &SceneSynchronizer::is_server);
	ClassDB::bind_method(D_METHOD("is_client"), &SceneSynchronizer::is_client);
	ClassDB::bind_method(D_METHOD("is_networked"), &SceneSynchronizer::is_networked);

	ClassDB::bind_method(D_METHOD("_on_peer_connected"), &SceneSynchronizer::_on_peer_connected);
	ClassDB::bind_method(D_METHOD("_on_peer_disconnected"), &SceneSynchronizer::_on_peer_disconnected);

	ClassDB::bind_method(D_METHOD("_on_node_removed"), &SceneSynchronizer::_on_node_removed);

	ClassDB::bind_method(D_METHOD("_rpc_send_state"), &SceneSynchronizer::_rpc_send_state);
	ClassDB::bind_method(D_METHOD("_rpc_notify_need_full_snapshot"), &SceneSynchronizer::_rpc_notify_need_full_snapshot);
	ClassDB::bind_method(D_METHOD("_rpc_set_network_enabled", "enabled"), &SceneSynchronizer::_rpc_set_network_enabled);
	ClassDB::bind_method(D_METHOD("_rpc_notify_peer_status", "enabled"), &SceneSynchronizer::_rpc_notify_peer_status);
	ClassDB::bind_method(D_METHOD("_rpc_send_actions", "enabled"), &SceneSynchronizer::_rpc_send_actions);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "server_notify_state_interval", PROPERTY_HINT_RANGE, "0.001,10.0,0.0001"), "set_server_notify_state_interval", "get_server_notify_state_interval");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "comparison_float_tolerance", PROPERTY_HINT_RANGE, "0.000001,0.01,0.000001"), "set_comparison_float_tolerance", "get_comparison_float_tolerance");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "actions_redundancy", PROPERTY_HINT_RANGE, "1,10,1"), "set_actions_redundancy", "get_actions_redundancy");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "actions_resend_time", PROPERTY_HINT_RANGE, "0.000001,0.5,0.000001"), "set_actions_resend_time", "get_actions_resend_time");

	ADD_SIGNAL(MethodInfo("sync_started"));
	ADD_SIGNAL(MethodInfo("sync_paused"));

	ADD_SIGNAL(MethodInfo("desync_detected", PropertyInfo(Variant::INT, "input_id"), PropertyInfo(Variant::OBJECT, "node"), PropertyInfo(Variant::ARRAY, "var_names"), PropertyInfo(Variant::ARRAY, "client_values"), PropertyInfo(Variant::ARRAY, "server_values")));
}

void SceneSynchronizer::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			if (Engine::get_singleton()->is_editor_hint())
				return;

			// TODO add a signal that allows to not check this each frame.
			if (unlikely(peer_ptr != get_multiplayer()->get_network_peer().ptr())) {
				reset_synchronizer_mode();
			}

			const int lowest_priority_number = INT32_MAX;
			ERR_FAIL_COND_MSG(get_process_priority() != lowest_priority_number, "The process priority MUST not be changed, it's likely there is a better way of doing what you are trying to do, if you really need it please open an issue.");

			process();
		} break;
		case NOTIFICATION_ENTER_TREE: {
			if (Engine::get_singleton()->is_editor_hint())
				return;

			clear();
			reset_synchronizer_mode();

			get_multiplayer()->connect(SNAME("network_peer_connected"), Callable(this, SNAME("_on_peer_connected")));
			get_multiplayer()->connect(SNAME("network_peer_disconnected"), Callable(this, SNAME("_on_peer_disconnected")));

			get_tree()->connect(SNAME("node_removed"), Callable(this, SNAME("_on_node_removed")));

			// Make sure to reset all the assigned controllers.
			reset_controllers();

			// Init the peers already connected.
			if (get_tree()->get_multiplayer()->get_network_peer().is_valid()) {
				const Vector<int> peer_ids = get_tree()->get_multiplayer()->get_network_connected_peers();
				const int *peer_ids_ptr = peer_ids.ptr();
				for (int i = 0; i < peer_ids.size(); i += 1) {
					_on_peer_connected(peer_ids_ptr[i]);
				}
			}

		} break;
		case NOTIFICATION_EXIT_TREE: {
			if (Engine::get_singleton()->is_editor_hint())
				return;

			clear_peers();

			get_multiplayer()->disconnect(SNAME("network_peer_connected"), Callable(this, SNAME("_on_peer_connected")));
			get_multiplayer()->disconnect(SNAME("network_peer_disconnected"), Callable(this, SNAME("_on_peer_disconnected")));

			get_tree()->disconnect(SNAME("node_removed"), Callable(this, SNAME("_on_node_removed")));

			clear();

			if (synchronizer) {
				memdelete(synchronizer);
				synchronizer = nullptr;
				synchronizer_type = SYNCHRONIZER_TYPE_NULL;
			}

			set_physics_process_internal(false);

			// Make sure to reset all the assigned controllers.
			reset_controllers();
		}
	}
}

SceneSynchronizer::SceneSynchronizer() {
	rpc_config(SNAME("_rpc_send_state"), MultiplayerAPI::RPC_MODE_REMOTE);
	rpc_config(SNAME("_rpc_notify_need_full_snapshot"), MultiplayerAPI::RPC_MODE_REMOTE);
	rpc_config(SNAME("_rpc_set_network_enabled"), MultiplayerAPI::RPC_MODE_REMOTE);
	rpc_config(SNAME("_rpc_notify_peer_status"), MultiplayerAPI::RPC_MODE_REMOTE);
	rpc_config(SNAME("_rpc_send_actions"), MultiplayerAPI::RPC_MODE_REMOTE);

	// Avoid too much useless re-allocations
	event_listener.reserve(100);
}

SceneSynchronizer::~SceneSynchronizer() {
	clear();
	if (synchronizer) {
		memdelete(synchronizer);
		synchronizer = nullptr;
		synchronizer_type = SYNCHRONIZER_TYPE_NULL;
	}
}

void SceneSynchronizer::set_server_notify_state_interval(real_t p_interval) {
	server_notify_state_interval = p_interval;
}

real_t SceneSynchronizer::get_server_notify_state_interval() const {
	return server_notify_state_interval;
}

void SceneSynchronizer::set_comparison_float_tolerance(real_t p_tolerance) {
	comparison_float_tolerance = p_tolerance;
}

real_t SceneSynchronizer::get_comparison_float_tolerance() const {
	return comparison_float_tolerance;
}

void SceneSynchronizer::set_actions_redundancy(int p_redundancy) {
	actions_redundancy = p_redundancy;
}

int SceneSynchronizer::get_actions_redundancy() const {
	return actions_redundancy;
}

void SceneSynchronizer::set_actions_resend_time(real_t p_time) {
	actions_resend_time = p_time;
}

real_t SceneSynchronizer::get_actions_resend_time() const {
	return actions_resend_time;
}

bool SceneSynchronizer::is_variable_registered(Node *p_node, const StringName &p_variable) const {
	const NetUtility::NodeData *nd = find_node_data(p_node);
	if (nd != nullptr) {
		return nd->vars.find(p_variable) >= 0;
	}
	return false;
}

NetUtility::NodeData *SceneSynchronizer::register_node(Node *p_node) {
	ERR_FAIL_COND_V(p_node == nullptr, nullptr);

	NetUtility::NodeData *nd = find_node_data(p_node);
	if (unlikely(nd == nullptr)) {
		nd = memnew(NetUtility::NodeData);
		nd->id = UINT32_MAX;
		nd->instance_id = p_node->get_instance_id();
		nd->node = p_node;

		NetworkedController *controller = Object::cast_to<NetworkedController>(p_node);
		if (controller) {
			if (unlikely(controller->has_scene_synchronizer())) {
				ERR_FAIL_V_MSG(nullptr, "This controller already has a synchronizer. This is a bug!");
			}

			nd->is_controller = true;
			controller->set_scene_synchronizer(this);
			dirty_peers();
		}

		add_node_data(nd);

		SceneSynchronizerDebugger::singleton()->debug_print(this, "New node registered" + (generate_id ? String(" #ID: ") + itos(nd->id) : "") + " : " + p_node->get_path());
	}

	SceneSynchronizerDebugger::singleton()->register_class_for_node_to_dump(p_node);

	return nd;
}

uint32_t SceneSynchronizer::register_node_gdscript(Node *p_node) {
	NetUtility::NodeData *nd = register_node(p_node);
	if (unlikely(nd == nullptr)) {
		return UINT32_MAX;
	}
	return nd->id;
}

void SceneSynchronizer::unregister_node(Node *p_node) {
	ERR_FAIL_COND(p_node == nullptr);

	NetUtility::NodeData *nd = find_node_data(p_node);
	if (unlikely(nd == nullptr)) {
		// Nothing to do.
		return;
	}

	drop_node_data(nd);
}

uint32_t SceneSynchronizer::get_node_id(Node *p_node) {
	ERR_FAIL_COND_V(p_node == nullptr, UINT32_MAX);
	NetUtility::NodeData *nd = find_node_data(p_node);
	ERR_FAIL_COND_V_MSG(nd == nullptr, UINT32_MAX, "This node " + p_node->get_path() + " is not yet registered, so there is not an available ID.");
	return nd->id;
}

Node *SceneSynchronizer::get_node_from_id(uint32_t p_id) {
	NetUtility::NodeData *nd = get_node_data(p_id);
	ERR_FAIL_COND_V_MSG(nd == nullptr, nullptr, "The ID " + itos(p_id) + " is not assigned to any node.");
	return nd->node;
}

void SceneSynchronizer::register_variable(Node *p_node, const StringName &p_variable, const StringName &p_on_change_notify, NetEventFlag p_flags) {
	ERR_FAIL_COND(p_node == nullptr);
	ERR_FAIL_COND(p_variable == StringName());

	NetUtility::NodeData *node_data = register_node(p_node);
	ERR_FAIL_COND(node_data == nullptr);

	const int index = node_data->vars.find(p_variable);
	if (index == -1) {
		// The variable is not yet registered.
		bool valid = false;
		const Variant old_val = p_node->get(p_variable, &valid);
		if (valid == false) {
			SceneSynchronizerDebugger::singleton()->debug_error(this, "The variable `" + p_variable + "` on the node `" + p_node->get_path() + "` was not found, make sure the variable exist.");
		}
		const int var_id = generate_id ? node_data->vars.size() : UINT32_MAX;
		node_data->vars.push_back(
				NetUtility::VarData(
						var_id,
						p_variable,
						old_val,
						false,
						true));
	} else {
		// Make sure the var is active.
		node_data->vars[index].enabled = true;
	}

#ifdef DEBUG_ENABLED
	for (uint32_t v = 0; v < node_data->vars.size(); v += 1) {
		// This can't happen, because the ID is always consecutive, or UINT32_MAX.
		CRASH_COND(node_data->vars[v].id != v && node_data->vars[v].id != UINT32_MAX);
	}
#endif

	if (p_on_change_notify != StringName()) {
		track_variable_changes(p_node, p_variable, p_node, p_on_change_notify, p_flags);
	}

	if (synchronizer) {
		synchronizer->on_variable_added(node_data, p_variable);
	}
}

void SceneSynchronizer::unregister_variable(Node *p_node, const StringName &p_variable) {
	ERR_FAIL_COND(p_node == nullptr);
	ERR_FAIL_COND(p_variable == StringName());

	NetUtility::NodeData *nd = find_node_data(p_node);
	ERR_FAIL_COND(nd == nullptr);

	const int64_t index = nd->vars.find(p_variable);
	ERR_FAIL_COND(index == -1);

	const NetVarId var_id = index;

	// Never remove the variable values, because the order of the vars matters.
	nd->vars[index].enabled = false;

	for (int i = 0; i < nd->vars[var_id].change_listeners.size(); i += 1) {
		const uint32_t event_index = nd->vars[var_id].change_listeners[i];
		// Just erase the tracked variables without removing the listener to
		// keep the order.
		NetUtility::NodeChangeListener ncl;
		ncl.node_data = nd;
		ncl.var_id = var_id;
		event_listener[event_index].watching_vars.erase(ncl);
	}

	nd->vars[index].change_listeners.clear();
}

void SceneSynchronizer::start_node_sync(const Node *p_node) {
	ERR_FAIL_COND(p_node == nullptr);

	NetUtility::NodeData *nd = find_node_data(p_node);
	ERR_FAIL_COND(nd == nullptr);

	nd->sync_enabled = true;
}

void SceneSynchronizer::stop_node_sync(const Node *p_node) {
	ERR_FAIL_COND(p_node == nullptr);

	NetUtility::NodeData *nd = find_node_data(p_node);
	ERR_FAIL_COND(nd == nullptr);

	nd->sync_enabled = false;
}

bool SceneSynchronizer::is_node_sync(const Node *p_node) const {
	ERR_FAIL_COND_V(p_node == nullptr, false);

	const NetUtility::NodeData *nd = find_node_data(p_node);
	if (nd == nullptr) {
		return false;
	}

	return nd->sync_enabled;
}

NetActionId SceneSynchronizer::register_action(
		Node *p_node,
		const StringName &p_action_func,
		const StringName &p_action_encoding_func,
		bool p_can_client_trigger,
		bool p_wait_server_validation,
		const StringName &p_server_action_validation_func) {
	ERR_FAIL_COND_V(p_node == nullptr, UINT32_MAX);

	// Validate the functions.
	List<MethodInfo> methods;
	p_node->get_method_list(&methods);

	MethodInfo *act_func_info = nullptr;
	MethodInfo *act_encoding_func_info = nullptr;
	MethodInfo *server_event_validation_info = nullptr;

	for (List<MethodInfo>::Element *e = methods.front(); e; e = e->next()) {
		if (e->get().name == p_action_func) {
			act_func_info = &e->get();
		} else if (e->get().name == p_action_encoding_func) {
			act_encoding_func_info = &e->get();
		} else if (p_server_action_validation_func != StringName() && e->get().name == p_server_action_validation_func) {
			server_event_validation_info = &e->get();
		}
	}

	ERR_FAIL_COND_V_MSG(act_func_info == nullptr, UINT32_MAX, "The passed `" + p_node->get_path() + "` doesn't have the event function `" + p_action_func + "`");
	ERR_FAIL_COND_V_MSG(act_encoding_func_info == nullptr, UINT32_MAX, "The passed `" + p_node->get_path() + "` doesn't have the event_encoding function `" + p_action_encoding_func + "`");

	ERR_FAIL_COND_V_MSG(act_encoding_func_info->arguments.size() != 1, UINT32_MAX, "`" + p_node->get_path() + "` - The passed event_encoding function `" + p_action_encoding_func + "` should have 1 argument with type `InputNetworkEncoder`.");
	if (act_encoding_func_info->arguments[0].type != Variant::NIL) {
		// If the paramter is typed, make sure it's the correct type.
		ERR_FAIL_COND_V_MSG(act_encoding_func_info->arguments[0].type != Variant::OBJECT, UINT32_MAX, "`" + p_node->get_path() + "` - The passed event_encoding function `" + p_action_encoding_func + "` should have 1 argument with type `InputNetworkEncoder`.");
		ERR_FAIL_COND_V_MSG(act_encoding_func_info->arguments[0].hint != PropertyHint::PROPERTY_HINT_RESOURCE_TYPE, UINT32_MAX, "`" + p_node->get_path() + "` - The passed event_encoding function `" + p_action_encoding_func + "` should have 1 argument with type `InputNetworkEncoder`.");
		ERR_FAIL_COND_V_MSG(act_encoding_func_info->arguments[0].hint_string != "InputNetworkEncoder", UINT32_MAX, "`" + p_node->get_path() + "` - The passed event_encoding function `" + p_action_encoding_func + "` should have 1 argument with type `InputNetworkEncoder`.");
	}

	if (server_event_validation_info) {
		if (server_event_validation_info->return_val.type != Variant::NIL) {
			ERR_FAIL_COND_V_MSG(server_event_validation_info->return_val.type != Variant::BOOL, UINT32_MAX, "`" + p_node->get_path() + "` - The passed server_action_validation_func `" + p_server_action_validation_func + "` should return a boolean.");
		}

		// Validate the arguments count.
		ERR_FAIL_COND_V_MSG(server_event_validation_info->arguments.size() != act_func_info->arguments.size(), UINT32_MAX, "`" + p_node->get_path() + "` - The function `" + p_server_action_validation_func + "` and `" + p_action_func + "` should have the same arguments.");

		// Validate the argument types.
		List<PropertyInfo>::Element *e_e = act_func_info->arguments.front();
		List<PropertyInfo>::Element *sevi_e = server_event_validation_info->arguments.front();
		for (; e_e; e_e = e_e->next(), sevi_e = sevi_e->next()) {
			ERR_FAIL_COND_V_MSG(sevi_e->get().type != e_e->get().type, UINT32_MAX, "`" + p_node->get_path() + "` - The function `" + p_server_action_validation_func + "` and `" + p_action_func + "` should have the same arguments.");
		}
	}

	// Fetch the function encoder and verify it can property encode the act_func arguments.
	Ref<InputNetworkEncoder> network_encoder;
	network_encoder.instance();
	p_node->call(p_action_encoding_func, network_encoder);
	const LocalVector<NetworkedInputInfo> &encoding_info = network_encoder->get_input_info();

	ERR_FAIL_COND_V_MSG(encoding_info.size() != (uint32_t)act_func_info->arguments.size(), UINT32_MAX, "`" + p_node->get_path() + "` - The encoding function should provide an encoding for each argument of `" + p_action_func + "` function (Note the order matters).");
	int i = 0;
	for (List<PropertyInfo>::Element *e = act_func_info->arguments.front(); e; e = e->next()) {
		if (e->get().type != Variant::NIL) {
			ERR_FAIL_COND_V_MSG(encoding_info[i].default_value.get_type() != e->get().type, UINT32_MAX, "`" + p_node->get_path() + "` - The encoding function " + itos(i) + " parameter is providing a wrong encoding for `" + e->get().name + "`.");
		}
		i++;
	}

	// At this point the validation passed. Just register the event.
	NetUtility::NodeData *node_data = register_node(p_node);
	ERR_FAIL_COND_V(node_data == nullptr, UINT32_MAX);

	NetActionId action_id = find_action_id(p_node, p_action_func);
	ERR_FAIL_COND_V_MSG(action_id != UINT32_MAX, UINT32_MAX, "`" + p_node->get_path() + "` The event `" + p_action_func + "` is already registered, this should never happen.");

	action_id = node_data->net_actions.size();
	node_data->net_actions.resize(action_id + 1);
	node_data->net_actions[action_id].id = action_id;
	node_data->net_actions[action_id].act_func = p_action_func;
	node_data->net_actions[action_id].act_encoding_func = p_action_encoding_func;
	node_data->net_actions[action_id].can_client_trigger = p_can_client_trigger;
	node_data->net_actions[action_id].wait_server_validation = p_wait_server_validation;
	node_data->net_actions[action_id].server_action_validation_func = p_server_action_validation_func;
	node_data->net_actions[action_id].network_encoder = network_encoder;

	SceneSynchronizerDebugger::singleton()->debug_print(this, "The event `" + p_action_func + "` on the node `" + p_node->get_path() + "` registered (act_encoding_func: `" + p_action_encoding_func + "`, wait_server_validation: `" + (p_server_action_validation_func ? "true" : "false") + "`, server_action_validation_func: `" + p_server_action_validation_func + "`).");
	return action_id;
}

NetActionId SceneSynchronizer::find_action_id(Node *p_node, const StringName &p_action_func) const {
	const NetUtility::NodeData *nd = find_node_data(p_node);
	if (nd) {
		NetActionInfo e;
		e.act_func = p_action_func;
		const int64_t i = nd->net_actions.find(e);
		return i == -1 ? UINT32_MAX : NetActionId(i);
	}
	return UINT32_MAX;
}

void SceneSynchronizer::trigger_action_by_name(
		Node *p_node,
		const StringName &p_action_func,
		const Array &p_arguments,
		const Vector<int> &p_recipients) {
	const NetActionId id = find_action_id(p_node, p_action_func);
	trigger_action(p_node, id, p_arguments, p_recipients);
}

void SceneSynchronizer::trigger_action(
		Node *p_node,
		NetActionId p_action_id,
		const Array &p_arguments,
		const Vector<int> &p_recipients) {
	ERR_FAIL_COND(p_node == nullptr);

	NetUtility::NodeData *nd = find_node_data(p_node);
	ERR_FAIL_COND_MSG(nd == nullptr, "The event was not found.");
	ERR_FAIL_COND_MSG(p_action_id >= nd->net_actions.size(), "The event was not found.");
	ERR_FAIL_COND_MSG(nd->net_actions[p_action_id].network_encoder->get_input_info().size() != uint32_t(p_arguments.size()), "The event `" + p_node->get_path() + "::" + nd->net_actions[p_action_id].act_func + "` was called with the wrong amount of arguments.");
	ERR_FAIL_COND_MSG(nd->net_actions[p_action_id].can_client_trigger == false && is_client(), "The client is not allowed to trigger this action `" + nd->net_actions[p_action_id].act_func + "`.");

	synchronizer->on_action_triggered(nd, p_action_id, p_arguments, p_recipients);
}

uint32_t SceneSynchronizer::get_variable_id(Node *p_node, const StringName &p_variable) {
	ERR_FAIL_COND_V(p_node == nullptr, UINT32_MAX);
	ERR_FAIL_COND_V(p_variable == StringName(), UINT32_MAX);

	NetUtility::NodeData *nd = find_node_data(p_node);
	ERR_FAIL_COND_V_MSG(nd == nullptr, UINT32_MAX, "This node " + p_node->get_path() + "is not registered.");

	const int64_t index = nd->vars.find(p_variable);
	ERR_FAIL_COND_V_MSG(index == -1, UINT32_MAX, "This variable " + p_node->get_path() + ":" + p_variable + " is not registered.");

	return uint32_t(index);
}

void SceneSynchronizer::set_skip_rewinding(Node *p_node, const StringName &p_variable, bool p_skip_rewinding) {
	ERR_FAIL_COND(p_node == nullptr);
	ERR_FAIL_COND(p_variable == StringName());

	NetUtility::NodeData *nd = find_node_data(p_node);
	ERR_FAIL_COND(nd == nullptr);

	const int64_t index = nd->vars.find(p_variable);
	ERR_FAIL_COND(index == -1);

	nd->vars[index].skip_rewinding = p_skip_rewinding;
}

void SceneSynchronizer::track_variable_changes(Node *p_node, const StringName &p_variable, Object *p_object, const StringName &p_method, NetEventFlag p_flags) {
	ERR_FAIL_COND(p_node == nullptr);
	ERR_FAIL_COND(p_variable == StringName());
	ERR_FAIL_COND(p_method == StringName());

	NetUtility::NodeData *nd = find_node_data(p_node);
	ERR_FAIL_COND_MSG(nd == nullptr, "You need to register the variable to track its changes.");

	const int64_t v = nd->vars.find(p_variable);
	ERR_FAIL_COND_MSG(v == -1, "You need to register the variable to track its changes.");

	const NetVarId var_id = v;

	int64_t index;

	{
		NetUtility::ChangeListener listener;
		listener.object_id = p_object->get_instance_id();
		listener.method = p_method;

		index = event_listener.find(listener);

		if (-1 == index) {
			// Add it.
			listener.flag = p_flags;
			listener.method_argument_count = UINT32_MAX;

			// Search the method and get the argument count.
			List<MethodInfo> methods;
			p_object->get_method_list(&methods);
			for (List<MethodInfo>::Element *e = methods.front(); e != nullptr; e = e->next()) {
				if (e->get().name != p_method) {
					continue;
				}

				listener.method_argument_count = e->get().arguments.size();

				break;
			}
			ERR_FAIL_COND_MSG(listener.method_argument_count == UINT32_MAX, "The method " + p_method + " doesn't exist in this node: " + p_node->get_path());

			index = event_listener.size();
			event_listener.push_back(listener);
		} else {
			ERR_FAIL_COND_MSG(event_listener[index].flag != p_flags, "The event listener is already registered with the flag: " + itos(event_listener[index].flag) + ". You can't specify a different one.");
		}
	}

	NetUtility::NodeChangeListener ncl;
	ncl.node_data = nd;
	ncl.var_id = var_id;

	if (event_listener[index].watching_vars.find(ncl) != -1) {
		return;
	}

	event_listener[index].watching_vars.push_back(ncl);
	nd->vars[var_id].change_listeners.push_back(index);
}

void SceneSynchronizer::untrack_variable_changes(Node *p_node, const StringName &p_variable, Object *p_object, const StringName &p_method) {
	ERR_FAIL_COND(p_node == nullptr);
	ERR_FAIL_COND(p_variable == StringName());
	ERR_FAIL_COND(p_method == StringName());

	NetUtility::NodeData *nd = find_node_data(p_node);
	ERR_FAIL_COND_MSG(nd == nullptr, "This not is not registered.");

	const int64_t v = nd->vars.find(p_variable);
	ERR_FAIL_COND_MSG(v == -1, "This variable is not registered.");

	const NetVarId var_id = v;

	NetUtility::ChangeListener listener;
	listener.object_id = p_object->get_instance_id();
	listener.method = p_method;

	const int64_t index = event_listener.find(listener);

	ERR_FAIL_COND_MSG(index == -1, "The variable is not know.");

	NetUtility::NodeChangeListener ncl;
	ncl.node_data = nd;
	ncl.var_id = var_id;

	event_listener[index].watching_vars.erase(ncl);
	nd->vars[var_id].change_listeners.erase(index);

	// Don't remove the listener to preserve the order.
}

void SceneSynchronizer::set_node_as_controlled_by(Node *p_node, Node *p_controller) {
	NetUtility::NodeData *nd = register_node(p_node);
	ERR_FAIL_COND(nd == nullptr);
	ERR_FAIL_COND_MSG(nd->is_controller, "A controller can't be controlled by another controller.");

	if (nd->controlled_by) {
		// Put the node back into global.
		nd->controlled_by->controlled_nodes.erase(nd);
		nd->controlled_by = nullptr;
	}

	if (p_controller) {
		NetworkedController *c = Object::cast_to<NetworkedController>(p_controller);
		ERR_FAIL_COND_MSG(c == nullptr, "The controller must be a node of type: NetworkedController.");

		NetUtility::NodeData *controller_node_data = register_node(p_controller);
		ERR_FAIL_COND(controller_node_data == nullptr);
		ERR_FAIL_COND_MSG(controller_node_data->is_controller == false, "The node can be only controlled by a controller.");

#ifdef DEBUG_ENABLED
		CRASH_COND_MSG(controller_node_data->controlled_nodes.find(nd) != -1, "There is a bug the same node is added twice into the controlled_nodes.");
#endif
		controller_node_data->controlled_nodes.push_back(nd);
		nd->controlled_by = controller_node_data;
	}

#ifdef DEBUG_ENABLED
	// Make sure that all controlled nodes are into the proper controller.
	for (uint32_t i = 0; i < node_data_controllers.size(); i += 1) {
		for (uint32_t y = 0; y < node_data_controllers[i]->controlled_nodes.size(); y += 1) {
			CRASH_COND(node_data_controllers[i]->controlled_nodes[y]->controlled_by != node_data_controllers[i]);
		}
	}
#endif
}

void SceneSynchronizer::controller_add_dependency(Node *p_controller, Node *p_node) {
	if (is_client() == false) {
		// Nothing to do.
		return;
	}

	NetUtility::NodeData *controller_nd = find_node_data(p_controller);
	ERR_FAIL_COND_MSG(controller_nd == nullptr, "The passed controller (" + p_controller->get_path() + ") is not registered.");
	ERR_FAIL_COND_MSG(controller_nd->is_controller == false, "The node passed as controller (" + p_controller->get_path() + ") is not a controller.");

	NetUtility::NodeData *node_nd = find_node_data(p_node);
	ERR_FAIL_COND_MSG(node_nd == nullptr, "The passed node (" + p_node->get_path() + ") is not registered.");
	ERR_FAIL_COND_MSG(node_nd->is_controller, "The node (" + p_node->get_path() + ") set as dependency is supposed to be just a node.");
	ERR_FAIL_COND_MSG(node_nd->controlled_by != nullptr, "The node (" + p_node->get_path() + ") set as dependency is supposed to be just a node.");

	const int64_t index = controller_nd->dependency_nodes.find(node_nd);
	if (index == -1) {
		controller_nd->dependency_nodes.push_back(node_nd);
		controller_nd->dependency_nodes_end.push_back(UINT32_MAX);
	} else {
		// We already have this dependency, just make sure we don't delete it.
		controller_nd->dependency_nodes_end[index] = UINT32_MAX;
	}
}

void SceneSynchronizer::controller_remove_dependency(Node *p_controller, Node *p_node) {
	if (is_client() == false) {
		// Nothing to do.
		return;
	}

	NetUtility::NodeData *controller_nd = find_node_data(p_controller);
	ERR_FAIL_COND_MSG(controller_nd == nullptr, "The passed controller (" + p_controller->get_path() + ") is not registered.");
	ERR_FAIL_COND_MSG(controller_nd->is_controller == false, "The node passed as controller (" + p_controller->get_path() + ") is not a controller.");

	NetUtility::NodeData *node_nd = find_node_data(p_node);
	ERR_FAIL_COND_MSG(node_nd == nullptr, "The passed node (" + p_node->get_path() + ") is not registered.");
	ERR_FAIL_COND_MSG(node_nd->is_controller, "The node (" + p_node->get_path() + ") set as dependency is supposed to be just a node.");
	ERR_FAIL_COND_MSG(node_nd->controlled_by != nullptr, "The node (" + p_node->get_path() + ") set as dependency is supposed to be just a node.");

	const int64_t index = controller_nd->dependency_nodes.find(node_nd);
	if (index == -1) {
		// Nothing to do, this node is not a dependency.
		return;
	}

	// Instead to remove the dependency immeditaly we have to postpone it till
	// the server confirms the valitity via state.
	// This operation is required otherwise the dependency is remvoved too early,
	// and an eventual rewind may miss it.
	// The actual removal is performed at the end of the sync.
	controller_nd->dependency_nodes_end[index] =
			static_cast<NetworkedController *>(controller_nd->node)->get_current_input_id();
}

int SceneSynchronizer::controller_get_dependency_count(Node *p_controller) const {
	if (is_client() == false) {
		// Nothing to do.
		return 0;
	}

	const NetUtility::NodeData *controller_nd = find_node_data(p_controller);
	ERR_FAIL_COND_V_MSG(controller_nd == nullptr, 0, "The passed controller (" + p_controller->get_path() + ") is not registered.");
	ERR_FAIL_COND_V_MSG(controller_nd->is_controller == false, 0, "The node passed as controller (" + p_controller->get_path() + ") is not a controller.");
	return controller_nd->dependency_nodes.size();
}

Node *SceneSynchronizer::controller_get_dependency(Node *p_controller, int p_index) {
	if (is_client() == false) {
		// Nothing to do.
		return nullptr;
	}

	NetUtility::NodeData *controller_nd = find_node_data(p_controller);
	ERR_FAIL_COND_V_MSG(controller_nd == nullptr, nullptr, "The passed controller (" + p_controller->get_path() + ") is not registered.");
	ERR_FAIL_COND_V_MSG(controller_nd->is_controller == false, nullptr, "The node passed as controller (" + p_controller->get_path() + ") is not a controller.");
	ERR_FAIL_INDEX_V(p_index, int(controller_nd->dependency_nodes.size()), nullptr);

	return controller_nd->dependency_nodes[p_index]->node;
}

void SceneSynchronizer::register_process(Node *p_node, const StringName &p_function) {
	ERR_FAIL_COND(p_node == nullptr);
	ERR_FAIL_COND(p_function == StringName());
	NetUtility::NodeData *node_data = register_node(p_node);
	ERR_FAIL_COND(node_data == nullptr);

	if (node_data->functions.find(p_function) == -1) {
		node_data->functions.push_back(p_function);
	}
}

void SceneSynchronizer::unregister_process(Node *p_node, const StringName &p_function) {
	ERR_FAIL_COND(p_node == nullptr);
	ERR_FAIL_COND(p_function == StringName());
	NetUtility::NodeData *node_data = register_node(p_node);
	ERR_FAIL_COND(node_data == nullptr);
	node_data->functions.erase(p_function);
}

void SceneSynchronizer::start_tracking_scene_changes(Object *p_diff_handle) const {
	ERR_FAIL_COND_MSG(get_tree()->get_multiplayer()->is_network_server() == false, "This function is supposed to be called only on server.");
	SceneDiff *diff = Object::cast_to<SceneDiff>(p_diff_handle);
	ERR_FAIL_COND_MSG(diff == nullptr, "The object is not a SceneDiff class.");

	diff->start_tracking_scene_changes(organized_node_data);
}

void SceneSynchronizer::stop_tracking_scene_changes(Object *p_diff_handle) const {
	ERR_FAIL_COND_MSG(get_tree()->get_multiplayer()->is_network_server() == false, "This function is supposed to be called only on server.");
	SceneDiff *diff = Object::cast_to<SceneDiff>(p_diff_handle);
	ERR_FAIL_COND_MSG(diff == nullptr, "The object is not a SceneDiff class.");

	diff->stop_tracking_scene_changes(this);
}

Variant SceneSynchronizer::pop_scene_changes(Object *p_diff_handle) const {
	ERR_FAIL_COND_V_MSG(
			synchronizer_type != SYNCHRONIZER_TYPE_SERVER,
			Variant(),
			"This function is supposed to be called only on server.");

	SceneDiff *diff = Object::cast_to<SceneDiff>(p_diff_handle);
	ERR_FAIL_COND_V_MSG(
			diff == nullptr,
			Variant(),
			"The object is not a SceneDiff class.");

	ERR_FAIL_COND_V_MSG(
			diff->is_tracking_in_progress(),
			Variant(),
			"You can't pop the changes while the tracking is still in progress.");

	// Generates a sync_data and returns it.
	Vector<Variant> ret;
	for (NetNodeId node_id = 0; node_id < diff->diff.size(); node_id += 1) {
		if (diff->diff[node_id].size() == 0) {
			// Nothing to do.
			continue;
		}

		bool node_id_in_ret = false;
		for (NetVarId var_id = 0; var_id < diff->diff[node_id].size(); var_id += 1) {
			if (diff->diff[node_id][var_id].is_different == false) {
				continue;
			}
			if (node_id_in_ret == false) {
				node_id_in_ret = true;
				// Set the node id.
				ret.push_back(node_id);
			}
			ret.push_back(var_id);
			ret.push_back(diff->diff[node_id][var_id].value);
		}
		if (node_id_in_ret) {
			// Close the Node data.
			ret.push_back(Variant());
		}
	}

	// Clear the diff data.
	diff->diff.clear();

	return ret.size() > 0 ? Variant(ret) : Variant();
}

void SceneSynchronizer::apply_scene_changes(const Variant &p_sync_data) {
	ERR_FAIL_COND_MSG(is_client() == false, "This function is not supposed to be called on server.");

	ClientSynchronizer *client_sync = static_cast<ClientSynchronizer *>(synchronizer);

	change_events_begin(NetEventFlag::CHANGE);

	const bool success = client_sync->parse_sync_data(
			p_sync_data,
			this,

			// Parse the Node:
			[](void *p_user_pointer, NetUtility::NodeData *p_node_data) {},

			// Parse InputID:
			[](void *p_user_pointer, uint32_t p_input_id) {},

			// Parse controller:
			[](void *p_user_pointer, NetUtility::NodeData *p_node_data) {},

			// Parse variable:
			[](void *p_user_pointer, NetUtility::NodeData *p_node_data, uint32_t p_var_id, const Variant &p_value) {
				SceneSynchronizer *scene_sync = static_cast<SceneSynchronizer *>(p_user_pointer);

				const Variant current_val = p_node_data->vars[p_var_id].var.value;

				if (scene_sync->compare(current_val, p_value) == false) {
					// There is a difference.
					// Set the new value.
					p_node_data->vars[p_var_id].var.value = p_value;
					p_node_data->node->set(
							p_node_data->vars[p_var_id].var.name,
							p_value);

					// Add an event.
					scene_sync->change_event_add(
							p_node_data,
							p_var_id,
							current_val);
				}
			});

	if (success == false) {
		SceneSynchronizerDebugger::singleton()->debug_error(this, "Scene changes:");
		SceneSynchronizerDebugger::singleton()->debug_error(this, p_sync_data);
	}

	change_events_flush();
}

bool SceneSynchronizer::is_recovered() const {
	return recover_in_progress;
}

bool SceneSynchronizer::is_resetted() const {
	return reset_in_progress;
}

bool SceneSynchronizer::is_rewinding() const {
	return rewinding_in_progress;
}

bool SceneSynchronizer::is_end_sync() const {
	return end_sync;
}

void SceneSynchronizer::force_state_notify() {
	ERR_FAIL_COND(is_server() == false);
	ServerSynchronizer *r = static_cast<ServerSynchronizer *>(synchronizer);
	// + 1.0 is just a ridiculous high number to be sure to avoid float
	// precision error.
	r->state_notifier_timer = get_server_notify_state_interval() + 1.0;
}

void SceneSynchronizer::dirty_peers() {
	peer_dirty = true;
}

void SceneSynchronizer::set_enabled(bool p_enable) {
	ERR_FAIL_COND_MSG(synchronizer_type == SYNCHRONIZER_TYPE_SERVER, "The server is always enabled.");
	if (synchronizer_type == SYNCHRONIZER_TYPE_CLIENT) {
		rpc_id(1, SNAME("_rpc_set_network_enabled"), p_enable);
		if (p_enable == false) {
			// If the peer want to disable, we can disable it locally
			// immediately. When it wants to enable the networking, the server
			// must be notified so it decides when to start the networking
			// again.
			static_cast<ClientSynchronizer *>(synchronizer)->set_enabled(p_enable);
		}
	} else if (synchronizer_type == SYNCHRONIZER_TYPE_NONETWORK) {
		set_peer_networking_enable(0, p_enable);
	}
}

bool SceneSynchronizer::is_enabled() const {
	ERR_FAIL_COND_V_MSG(synchronizer_type == SYNCHRONIZER_TYPE_SERVER, false, "The server is always enabled.");
	if (likely(synchronizer_type == SYNCHRONIZER_TYPE_CLIENT)) {
		return static_cast<ClientSynchronizer *>(synchronizer)->enabled;
	} else if (synchronizer_type == SYNCHRONIZER_TYPE_NONETWORK) {
		return static_cast<NoNetSynchronizer *>(synchronizer)->enabled;
	} else {
		return true;
	}
}

void SceneSynchronizer::set_peer_networking_enable(int p_peer, bool p_enable) {
	if (synchronizer_type == SYNCHRONIZER_TYPE_SERVER) {
		ERR_FAIL_COND_MSG(p_peer == 1, "Disable the server is not possible.");

		NetUtility::PeerData *pd = peer_data.lookup_ptr(p_peer);
		ERR_FAIL_COND_MSG(pd == nullptr, "The peer: " + itos(p_peer) + " is not know. [bug]");

		if (pd->enabled == p_enable) {
			// Nothing to do.
			return;
		}

		pd->enabled = p_enable;
		// Set to true, so next time this peer connects a full snapshot is sent.
		pd->force_notify_snapshot = true;
		pd->need_full_snapshot = true;

		dirty_peers();

		// Just notify the peer status.
		rpc_id(p_peer, SNAME("_rpc_notify_peer_status"), p_enable);
	} else {
		ERR_FAIL_COND_MSG(synchronizer_type != SYNCHRONIZER_TYPE_NONETWORK, "At this point no network is expected.");
		static_cast<NoNetSynchronizer *>(synchronizer)->set_enabled(p_enable);
	}
}

bool SceneSynchronizer::is_peer_networking_enable(int p_peer) const {
	if (synchronizer_type == SYNCHRONIZER_TYPE_SERVER) {
		if (p_peer == 1) {
			// Server is always enabled.
			return true;
		}

		const NetUtility::PeerData *pd = peer_data.lookup_ptr(p_peer);
		ERR_FAIL_COND_V_MSG(pd == nullptr, false, "The peer: " + itos(p_peer) + " is not know. [bug]");
		return pd->enabled;
	} else {
		ERR_FAIL_COND_V_MSG(synchronizer_type != SYNCHRONIZER_TYPE_NONETWORK, false, "At this point no network is expected.");
		return static_cast<NoNetSynchronizer *>(synchronizer)->is_enabled();
	}
}

void SceneSynchronizer::_on_peer_connected(int p_peer) {
	peer_data.insert(p_peer, NetUtility::PeerData());
	dirty_peers();
}

void SceneSynchronizer::_on_peer_disconnected(int p_peer) {
	peer_data.remove(p_peer);

	// Notify all controllers that this peer is gone.
	for (uint32_t i = 0; i < node_data_controllers.size(); i += 1) {
		NetworkedController *c = static_cast<NetworkedController *>(node_data_controllers[i]->node);
		c->controller->deactivate_peer(p_peer);
	}
}

void SceneSynchronizer::_on_node_removed(Node *p_node) {
	unregister_node(p_node);
}

void SceneSynchronizer::reset_synchronizer_mode() {
	set_physics_process_internal(false);
	const bool was_generating_ids = generate_id;
	generate_id = false;

	if (synchronizer) {
		memdelete(synchronizer);
		synchronizer = nullptr;
		synchronizer_type = SYNCHRONIZER_TYPE_NULL;
	}

	peer_ptr = get_multiplayer() == nullptr ? nullptr : get_multiplayer()->get_network_peer().ptr();

	if (get_tree() == nullptr || get_tree()->get_multiplayer()->get_network_peer().is_null()) {
		synchronizer_type = SYNCHRONIZER_TYPE_NONETWORK;
		synchronizer = memnew(NoNetSynchronizer(this));
		generate_id = true;

	} else if (get_tree()->get_multiplayer()->is_network_server()) {
		synchronizer_type = SYNCHRONIZER_TYPE_SERVER;
		synchronizer = memnew(ServerSynchronizer(this));
		generate_id = true;
	} else {
		synchronizer_type = SYNCHRONIZER_TYPE_CLIENT;
		synchronizer = memnew(ClientSynchronizer(this));
	}

	// Always runs the SceneSynchronizer last.
	const int lowest_priority_number = INT32_MAX;
	set_process_priority(lowest_priority_number);
	set_physics_process_internal(true);

	if (was_generating_ids != generate_id) {
		organized_node_data.resize(node_data.size());
		for (uint32_t i = 0; i < node_data.size(); i += 1) {
			if (node_data[i] == nullptr) {
				continue;
			}

			// Handle the node ID.
			if (generate_id) {
				node_data[i]->id = i;
				organized_node_data[i] = node_data[i];
			} else {
				node_data[i]->id = UINT32_MAX;
				organized_node_data[i] = nullptr;
			}

			// Handle the variables ID.
			for (uint32_t v = 0; v < node_data[i]->vars.size(); v += 1) {
				if (generate_id) {
					node_data[i]->vars[v].id = v;
				} else {
					node_data[i]->vars[v].id = UINT32_MAX;
				}
			}
		}
	}

	// Notify the presence all available nodes and its variables to the synchronizer.
	for (uint32_t i = 0; i < node_data.size(); i += 1) {
		synchronizer->on_node_added(node_data[i]);
		for (uint32_t y = 0; y < node_data[i]->vars.size(); y += 1) {
			synchronizer->on_variable_added(node_data[i], node_data[i]->vars[y].var.name);
		}
	}

	// Reset the controllers.
	reset_controllers();
}

void SceneSynchronizer::clear() {
	// Drop the node_data.
	for (uint32_t i = 0; i < node_data.size(); i += 1) {
		if (node_data[i] != nullptr) {
			drop_node_data(node_data[i]);
		}
	}

	node_data.reset();
	organized_node_data.reset();
	node_data_controllers.reset();
	event_listener.reset();

	// Avoid too much useless re-allocations.
	event_listener.reserve(100);

	if (synchronizer) {
		synchronizer->clear();
	}
}

void SceneSynchronizer::notify_controller_control_mode_changed(NetworkedController *controller) {
	reset_controller(find_node_data(controller));
}

void SceneSynchronizer::_rpc_send_state(const Variant &p_snapshot) {
	ERR_FAIL_COND_MSG(is_client() == false, "Only clients are suposed to receive the server snapshot.");
	static_cast<ClientSynchronizer *>(synchronizer)->receive_snapshot(p_snapshot);
}

void SceneSynchronizer::_rpc_notify_need_full_snapshot() {
	ERR_FAIL_COND_MSG(is_server() == false, "Only the server can receive the request to send a full snapshot.");

	const int sender_peer = get_tree()->get_multiplayer()->get_rpc_sender_id();
	NetUtility::PeerData *pd = peer_data.lookup_ptr(sender_peer);
	ERR_FAIL_COND(pd == nullptr);
	pd->need_full_snapshot = true;
}

void SceneSynchronizer::_rpc_set_network_enabled(bool p_enabled) {
	ERR_FAIL_COND_MSG(is_server() == false, "The peer status is supposed to be received by the server.");
	set_peer_networking_enable(
			get_multiplayer()->get_rpc_sender_id(),
			p_enabled);
}

void SceneSynchronizer::_rpc_notify_peer_status(bool p_enabled) {
	ERR_FAIL_COND_MSG(is_client() == false, "The peer status is supposed to be received by the client.");
	static_cast<ClientSynchronizer *>(synchronizer)->set_enabled(p_enabled);
}

void SceneSynchronizer::_rpc_send_actions(const Vector<uint8_t> &p_data) {
	// Anyone can receive acts.
	DataBuffer db(p_data);
	db.begin_read();

	LocalVector<SenderNetAction> received_actions;

	const int sender_peer = get_tree()->get_multiplayer()->get_rpc_sender_id();

	net_action::decode_net_action(
			this,
			db,
			sender_peer,
			received_actions);

	synchronizer->on_actions_received(sender_peer, received_actions);
}

void SceneSynchronizer::update_peers() {
#ifdef DEBUG_ENABLED
	// This function is only called on server.
	CRASH_COND(synchronizer_type != SYNCHRONIZER_TYPE_SERVER);
#endif

	if (likely(peer_dirty == false)) {
		return;
	}

	peer_dirty = false;

	for (OAHashMap<int, NetUtility::PeerData>::Iterator it = peer_data.iter();
			it.valid;
			it = peer_data.next_iter(it)) {
		// Validate the peer.
		if (it.value->controller_id != UINT32_MAX) {
			NetUtility::NodeData *nd = get_node_data(it.value->controller_id);
			if (nd == nullptr ||
					nd->is_controller == false ||
					nd->node->get_network_master() != (*it.key)) {
				// Invalidate the controller id
				it.value->controller_id = UINT32_MAX;
			}
		}

		if (it.value->controller_id == UINT32_MAX) {
			// The controller_id is not assigned, search it.
			for (uint32_t i = 0; i < node_data_controllers.size(); i += 1) {
				if (node_data_controllers[i]->node->get_network_master() == (*it.key)) {
					// Controller found.
					it.value->controller_id = node_data_controllers[i]->id;
					break;
				}
			}
		}

		// Propagate the peer change to controllers.
		for (uint32_t i = 0; i < node_data_controllers.size(); i += 1) {
			NetworkedController *c = static_cast<NetworkedController *>(node_data_controllers[i]->node);

			if (it.value->controller_id == node_data_controllers[i]->id) {
				// This is the controller owned by this peer.
				c->get_server_controller()->set_enabled(it.value->enabled);
			} else {
				// This is a controller owned by another peer.
				if (it.value->enabled) {
					c->controller->activate_peer(*it.key);
				} else {
					c->controller->deactivate_peer(*it.key);
				}
			}
		}
	}
}

void SceneSynchronizer::clear_peers() {
	peer_data.clear();
	for (uint32_t i = 0; i < node_data_controllers.size(); i += 1) {
		NetworkedController *c = static_cast<NetworkedController *>(node_data_controllers[i]->node);
		c->controller->clear_peers();
	}
}

void SceneSynchronizer::change_events_begin(int p_flag) {
#ifdef DEBUG_ENABLED
	// This can't happen because at the end these are reset.
	CRASH_COND(recover_in_progress);
	CRASH_COND(reset_in_progress);
	CRASH_COND(rewinding_in_progress);
	CRASH_COND(end_sync);
#endif
	event_flag = p_flag;
	recover_in_progress = NetEventFlag::SYNC & p_flag;
	reset_in_progress = NetEventFlag::SYNC_RESET & p_flag;
	rewinding_in_progress = NetEventFlag::SYNC_REWIND & p_flag;
	end_sync = NetEventFlag::END_SYNC & p_flag;
}

void SceneSynchronizer::change_event_add(NetUtility::NodeData *p_node_data, NetVarId p_var_id, const Variant &p_old) {
	for (int i = 0; i < p_node_data->vars[p_var_id].change_listeners.size(); i += 1) {
		const uint32_t listener_index = p_node_data->vars[p_var_id].change_listeners[i];
		NetUtility::ChangeListener &listener = event_listener[listener_index];
		if ((listener.flag & event_flag) == 0) {
			// Not listening to this event.
			continue;
		}

		listener.emitted = false;

		NetUtility::NodeChangeListener ncl;
		ncl.node_data = p_node_data;
		ncl.var_id = p_var_id;

		const int64_t index = listener.watching_vars.find(ncl);
#ifdef DEBUG_ENABLED
		// This can't never happen because the `NodeData::change_listeners`
		// tracks the correct listener.
		CRASH_COND(index == -1);
#endif
		listener.watching_vars[index].old_value = p_old;
		listener.watching_vars[index].old_set = true;
	}

	// Notify the synchronizer.
	if (synchronizer) {
		synchronizer->on_variable_changed(
				p_node_data,
				p_var_id,
				p_old,
				event_flag);
	}
}

void SceneSynchronizer::change_events_flush() {
	LocalVector<Variant> vars;
	LocalVector<const Variant *> vars_ptr;

	// TODO this can be optimized by storing the changed listener in a separate
	// vector. This change must be inserted into the `change_event_add`.
	for (uint32_t listener_i = 0; listener_i < event_listener.size(); listener_i += 1) {
		NetUtility::ChangeListener &listener = event_listener[listener_i];
		if (listener.emitted) {
			continue;
		}
		listener.emitted = true;

		Object *obj = ObjectDB::get_instance(listener.object_id);
		if (obj == nullptr) {
			// Setting the flag to 0 so no events trigger this anymore.
			listener.flag = NetEventFlag::EMPTY;
			listener.object_id = ObjectID();
			listener.method = StringName();

			// Make sure this listener is not tracking any variable.
			for (uint32_t wv = 0; wv < listener.watching_vars.size(); wv += 1) {
				NetUtility::NodeData *nd = listener.watching_vars[wv].node_data;
				uint32_t var_id = listener.watching_vars[wv].var_id;
				nd->vars[var_id].change_listeners.erase(listener_i);
			}
			listener.watching_vars.clear();
			continue;
		}

		// Initialize the arguments
		ERR_CONTINUE_MSG(listener.method_argument_count > listener.watching_vars.size(), "This method " + listener.method + " has more arguments than the watched variables. This listener is broken.");

		vars.resize(MIN(listener.watching_vars.size(), listener.method_argument_count));
		vars_ptr.resize(vars.size());
		for (uint32_t v = 0; v < MIN(listener.watching_vars.size(), listener.method_argument_count); v += 1) {
			if (listener.watching_vars[v].old_set) {
				vars[v] = listener.watching_vars[v].old_value;
				listener.watching_vars[v].old_set = false;
			} else {
				// This value is not changed, so just retrive the current one.
				vars[v] = listener.watching_vars[v].node_data->vars[listener.watching_vars[v].var_id].var.value;
			}
			vars_ptr[v] = vars.ptr() + v;
		}

		Variant::CallError e;
		obj->call(listener.method, vars_ptr.ptr(), vars_ptr.size(), e);
	}

	recover_in_progress = false;
	reset_in_progress = false;
	rewinding_in_progress = false;
	end_sync = false;
}

void SceneSynchronizer::add_node_data(NetUtility::NodeData *p_node_data) {
	if (generate_id) {
#ifdef DEBUG_ENABLED
		// When generate_id is true, the id must always be undefined.
		CRASH_COND(p_node_data->id != UINT32_MAX);
#endif
		p_node_data->id = organized_node_data.size();
	}

#ifdef DEBUG_ENABLED
	// Make sure the registered nodes have an unique ID.
	// Due to an engine bug, it's possible to have two different nodes with the
	// exact same path:
	//		- Create a scene.
	//		- Add a child with the name `BadChild`.
	//		- Instance the scene into another scene.
	//		- Add a child, under the instanced scene, with the name `BadChild`.
	//	Now you have the scene with two different nodes but same path.
	for (uint32_t i = 0; i < node_data.size(); i += 1) {
		if (node_data[i]->node->get_path() == p_node_data->node->get_path()) {
			SceneSynchronizerDebugger::singleton()->debug_error(this, "You have two different nodes with the same path: " + p_node_data->node->get_path() + ". This will cause troubles. Fix it.");
			break;
		}
	}
#endif

	node_data.push_back(p_node_data);

	if (generate_id) {
		organized_node_data.push_back(p_node_data);
	} else {
		if (p_node_data->id != UINT32_MAX) {
			// This node has an ID, make sure to organize it properly.

			if (organized_node_data.size() <= p_node_data->id) {
				expand_organized_node_data_vector((p_node_data->id + 1) - organized_node_data.size());
			}

			organized_node_data[p_node_data->id] = p_node_data;
		}
	}

	if (p_node_data->is_controller) {
		node_data_controllers.push_back(p_node_data);
		reset_controller(p_node_data);
	}

	if (synchronizer) {
		synchronizer->on_node_added(p_node_data);
	}
}

void SceneSynchronizer::drop_node_data(NetUtility::NodeData *p_node_data) {
	if (synchronizer) {
		synchronizer->on_node_removed(p_node_data);
	}

	if (p_node_data->controlled_by) {
		// This node is controlled by another one, remove from that node.
		p_node_data->controlled_by->controlled_nodes.erase(p_node_data);
		p_node_data->controlled_by = nullptr;
	}

	// Set all controlled nodes as not controlled by this.
	for (uint32_t i = 0; i < p_node_data->controlled_nodes.size(); i += 1) {
		p_node_data->controlled_nodes[i]->controlled_by = nullptr;
	}
	p_node_data->controlled_nodes.clear();

	if (p_node_data->is_controller) {
		// This is a controller, make sure to reset the peers.
		static_cast<NetworkedController *>(p_node_data->node)->set_scene_synchronizer(nullptr);
		dirty_peers();
		node_data_controllers.erase(p_node_data);
	}

	node_data.erase(p_node_data);

	if (p_node_data->id < organized_node_data.size()) {
		// Never resize this vector to keep it sort.
		organized_node_data[p_node_data->id] = nullptr;
	}

	for (uint32_t i = 0; i < node_data_controllers.size(); i += 1) {
		const int64_t index = node_data_controllers[i]->dependency_nodes.find(p_node_data);
		if (index != -1) {
			node_data_controllers[i]->dependency_nodes.remove_unordered(index);
			node_data_controllers[i]->dependency_nodes_end.remove_unordered(index);
		}
	}

	// Remove this `NodeData` from any event listener.
	for (uint32_t i = 0; i < event_listener.size(); i += 1) {
		while (true) {
			uint32_t index_to_remove = UINT32_MAX;

			// Search.
			for (uint32_t v = 0; v < event_listener[i].watching_vars.size(); v += 1) {
				if (event_listener[i].watching_vars[v].node_data == p_node_data) {
					index_to_remove = v;
					break;
				}
			}

			if (index_to_remove == UINT32_MAX) {
				// Nothing more to do.
				break;
			} else {
				event_listener[i].watching_vars.remove_unordered(index_to_remove);
			}
		}
	}

	memdelete(p_node_data);
}

void SceneSynchronizer::set_node_data_id(NetUtility::NodeData *p_node_data, NetNodeId p_id) {
#ifdef DEBUG_ENABLED
	CRASH_COND_MSG(generate_id, "This function is not supposed to be called, because this instance is generating the IDs");
#endif
	if (organized_node_data.size() <= p_id) {
		expand_organized_node_data_vector((p_id + 1) - organized_node_data.size());
	}
	p_node_data->id = p_id;
	organized_node_data[p_id] = p_node_data;
	SceneSynchronizerDebugger::singleton()->debug_print(this, "NetNodeId: " + itos(p_id) + " just assigned to: " + p_node_data->node->get_path());
}

NetworkedController *SceneSynchronizer::fetch_controller_by_peer(int peer) {
	NetUtility::PeerData *data = peer_data.lookup_ptr(peer);
	if (data && data->controller_id != UINT32_MAX) {
		NetUtility::NodeData *nd = get_node_data(data->controller_id);
		if (nd) {
			if (nd->is_controller) {
				return static_cast<NetworkedController *>(nd->node);
			}
		}
	}
	return nullptr;
}

bool SceneSynchronizer::compare(const Vector2 &p_first, const Vector2 &p_second) const {
	return compare(p_first, p_second, comparison_float_tolerance);
}

bool SceneSynchronizer::compare(const Vector3 &p_first, const Vector3 &p_second) const {
	return compare(p_first, p_second, comparison_float_tolerance);
}

bool SceneSynchronizer::compare(const Variant &p_first, const Variant &p_second) const {
	return compare(p_first, p_second, comparison_float_tolerance);
}

bool SceneSynchronizer::compare(const Vector2 &p_first, const Vector2 &p_second, real_t p_tolerance) {
	return Math::is_equal_approx(p_first.x, p_second.x, p_tolerance) &&
			Math::is_equal_approx(p_first.y, p_second.y, p_tolerance);
}

bool SceneSynchronizer::compare(const Vector3 &p_first, const Vector3 &p_second, real_t p_tolerance) {
	return Math::is_equal_approx(p_first.x, p_second.x, p_tolerance) &&
			Math::is_equal_approx(p_first.y, p_second.y, p_tolerance) &&
			Math::is_equal_approx(p_first.z, p_second.z, p_tolerance);
}

bool SceneSynchronizer::compare(const Variant &p_first, const Variant &p_second, real_t p_tolerance) {
	if (p_first.get_type() != p_second.get_type()) {
		return false;
	}

	// Custom evaluation methods
	switch (p_first.get_type()) {
		case Variant::FLOAT: {
			return Math::is_equal_approx(p_first, p_second, p_tolerance);
		}
		case Variant::VECTOR2: {
			return compare(Vector2(p_first), Vector2(p_second), p_tolerance);
		}
		case Variant::RECT2: {
			const Rect2 a(p_first);
			const Rect2 b(p_second);
			if (compare(a.position, b.position, p_tolerance)) {
				if (compare(a.size, b.size, p_tolerance)) {
					return true;
				}
			}
			return false;
		}
		case Variant::TRANSFORM2D: {
			const Transform2D a(p_first);
			const Transform2D b(p_second);
			if (compare(a.elements[0], b.elements[0], p_tolerance)) {
				if (compare(a.elements[1], b.elements[1], p_tolerance)) {
					if (compare(a.elements[2], b.elements[2], p_tolerance)) {
						return true;
					}
				}
			}
			return false;
		}
		case Variant::VECTOR3:
			return compare(Vector3(p_first), Vector3(p_second), p_tolerance);

		case Variant::QUAT: {
			const Quat a = p_first;
			const Quat b = p_second;
			const Quat r(a - b); // Element wise subtraction.
			return (r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w) <= (p_tolerance * p_tolerance);
		}
		case Variant::PLANE: {
			const Plane a(p_first);
			const Plane b(p_second);
			if (Math::is_equal_approx(a.d, b.d, p_tolerance)) {
				if (compare(a.normal, b.normal, p_tolerance)) {
					return true;
				}
			}
			return false;
		}
		case Variant::AABB: {
			const AABB a(p_first);
			const AABB b(p_second);
			if (compare(a.position, b.position, p_tolerance)) {
				if (compare(a.size, b.size, p_tolerance)) {
					return true;
				}
			}
			return false;
		}
		case Variant::BASIS: {
			const Basis a = p_first;
			const Basis b = p_second;
			if (compare(a.elements[0], b.elements[0], p_tolerance)) {
				if (compare(a.elements[1], b.elements[1], p_tolerance)) {
					if (compare(a.elements[2], b.elements[2], p_tolerance)) {
						return true;
					}
				}
			}
			return false;
		}
		case Variant::TRANSFORM: {
			const Transform a = p_first;
			const Transform b = p_second;
			if (compare(a.origin, b.origin, p_tolerance)) {
				if (compare(a.basis.elements[0], b.basis.elements[0], p_tolerance)) {
					if (compare(a.basis.elements[1], b.basis.elements[1], p_tolerance)) {
						if (compare(a.basis.elements[2], b.basis.elements[2], p_tolerance)) {
							return true;
						}
					}
				}
			}
			return false;
		}
		case Variant::ARRAY: {
			const Array a = p_first;
			const Array b = p_second;
			if (a.size() != b.size()) {
				return false;
			}
			for (int i = 0; i < a.size(); i += 1) {
				if (compare(a[i], b[i], p_tolerance) == false) {
					return false;
				}
			}
			return true;
		}
		case Variant::DICTIONARY: {
			const Dictionary a = p_first;
			const Dictionary b = p_second;

			if (a.size() != b.size()) {
				return false;
			}

			List<Variant> l;
			a.get_key_list(&l);

			for (const List<Variant>::Element *key = l.front(); key; key = key->next()) {
				if (b.has(key->get()) == false) {
					return false;
				}

				if (compare(
							a.get(key->get(), Variant()),
							b.get(key->get(), Variant()),
							p_tolerance) == false) {
					return false;
				}
			}

			return true;
		}
		default:
			return p_first == p_second;
	}
}

bool SceneSynchronizer::is_server() const {
	return synchronizer_type == SYNCHRONIZER_TYPE_SERVER;
}

bool SceneSynchronizer::is_client() const {
	return synchronizer_type == SYNCHRONIZER_TYPE_CLIENT;
}

bool SceneSynchronizer::is_no_network() const {
	return synchronizer_type == SYNCHRONIZER_TYPE_NONETWORK;
}

bool SceneSynchronizer::is_networked() const {
	return is_client() || is_server();
}

#ifdef DEBUG_ENABLED
void SceneSynchronizer::validate_nodes() {
	LocalVector<NetUtility::NodeData *> null_objects;
	null_objects.reserve(node_data.size());

	for (uint32_t i = 0; i < node_data.size(); i += 1) {
		if (ObjectDB::get_instance(node_data[i]->instance_id) == nullptr) {
			// Mark for removal.
			null_objects.push_back(node_data[i]);
		}
	}

	// Removes the invalidated `NodeData`.
	if (null_objects.size()) {
		SceneSynchronizerDebugger::singleton()->debug_error(this, "At least one node has been removed from the tree without the SceneSynchronizer noticing. This shouldn't happen.");
		for (uint32_t i = 0; i < null_objects.size(); i += 1) {
			drop_node_data(null_objects[i]);
		}
	}
}
#endif

void SceneSynchronizer::purge_node_dependencies() {
	if (is_client() == false) {
		return;
	}

	// Clear the controller dependencies.
	ClientSynchronizer *client_sync = static_cast<ClientSynchronizer *>(synchronizer);

	for (uint32_t i = 0; i < node_data_controllers.size(); i += 1) {
		for (
				int d = 0;
				d < int(node_data_controllers[i]->dependency_nodes_end.size());
				d += 1) {
			if (node_data_controllers[i]->dependency_nodes_end[d] < client_sync->last_checked_input) {
				// This controller dependency can be cleared because the server
				// snapshot check has
				node_data_controllers[i]->dependency_nodes.remove_unordered(d);
				node_data_controllers[i]->dependency_nodes_end.remove_unordered(d);
				d -= 1;
			}
		}
	}
}

void SceneSynchronizer::expand_organized_node_data_vector(uint32_t p_size) {
	const uint32_t from = organized_node_data.size();
	organized_node_data.resize(from + p_size);
	memset(organized_node_data.ptr() + from, 0, sizeof(void *) * p_size);
}

NetUtility::NodeData *SceneSynchronizer::find_node_data(const Node *p_node) {
	for (uint32_t i = 0; i < node_data.size(); i += 1) {
		if (node_data[i] == nullptr) {
			continue;
		}
		if (node_data[i]->instance_id == p_node->get_instance_id()) {
			return node_data[i];
		}
	}
	return nullptr;
}

const NetUtility::NodeData *SceneSynchronizer::find_node_data(const Node *p_node) const {
	for (uint32_t i = 0; i < node_data.size(); i += 1) {
		if (node_data[i] == nullptr) {
			continue;
		}
		if (node_data[i]->instance_id == p_node->get_instance_id()) {
			return node_data[i];
		}
	}
	return nullptr;
}

NetUtility::NodeData *SceneSynchronizer::get_node_data(NetNodeId p_id) {
	ERR_FAIL_UNSIGNED_INDEX_V(p_id, organized_node_data.size(), nullptr);
	return organized_node_data[p_id];
}

const NetUtility::NodeData *SceneSynchronizer::get_node_data(NetNodeId p_id) const {
	ERR_FAIL_UNSIGNED_INDEX_V(p_id, organized_node_data.size(), nullptr);
	return organized_node_data[p_id];
}

NetNodeId SceneSynchronizer::get_biggest_node_id() const {
	return organized_node_data.size() == 0 ? UINT32_MAX : organized_node_data.size() - 1;
}

void SceneSynchronizer::reset_controllers() {
	for (uint32_t i = 0; i < node_data_controllers.size(); i += 1) {
		reset_controller(node_data_controllers[i]);
	}
}

void SceneSynchronizer::reset_controller(NetUtility::NodeData *p_controller_nd) {
#ifdef DEBUG_ENABLED
	// This can't happen because the callers make sure the `NodeData` is a
	// controller.
	CRASH_COND(p_controller_nd->is_controller == false);
#endif

	NetworkedController *controller = static_cast<NetworkedController *>(p_controller_nd->node);

	// Reset the controller type.
	if (controller->controller != nullptr) {
		memdelete(controller->controller);
		controller->controller = nullptr;
		controller->controller_type = NetworkedController::CONTROLLER_TYPE_NULL;
		controller->set_physics_process_internal(false);
	}

	if (get_tree() == nullptr) {
		if (synchronizer) {
			synchronizer->on_controller_reset(p_controller_nd);
		}

		// Nothing to do.
		return;
	}

	if (get_tree()->get_multiplayer()->get_network_peer().is_null()) {
		controller->controller_type = NetworkedController::CONTROLLER_TYPE_NONETWORK;
		controller->controller = memnew(NoNetController(controller));
	} else if (get_tree()->get_multiplayer()->is_network_server()) {
		if (controller->get_server_controlled()) {
			controller->controller_type = NetworkedController::CONTROLLER_TYPE_AUTONOMOUS_SERVER;
			controller->controller = memnew(AutonomousServerController(controller));
		} else {
			controller->controller_type = NetworkedController::CONTROLLER_TYPE_SERVER;
			controller->controller = memnew(ServerController(controller, controller->get_network_traced_frames()));
		}
	} else if (controller->is_network_master() && controller->get_server_controlled() == false) {
		controller->controller_type = NetworkedController::CONTROLLER_TYPE_PLAYER;
		controller->controller = memnew(PlayerController(controller));
	} else {
		controller->controller_type = NetworkedController::CONTROLLER_TYPE_DOLL;
		controller->controller = memnew(DollController(controller));
		controller->set_physics_process_internal(true);
	}

	dirty_peers();
	controller->controller->ready();
	controller->notify_controller_reset();

	if (synchronizer) {
		synchronizer->on_controller_reset(p_controller_nd);
	}
}

void SceneSynchronizer::process() {
	PROFILE_NODE

#ifdef DEBUG_ENABLED
	validate_nodes();
	// Never triggered because this function is called by `PHYSICS_PROCESS`,
	// notification that is emitted only when the node is in the tree.
	// When the node is in the tree, there is no way that the `synchronizer` is
	// null.
	CRASH_COND(synchronizer == nullptr);
#endif

	synchronizer->process();
	purge_node_dependencies();
}

void SceneSynchronizer::pull_node_changes(NetUtility::NodeData *p_node_data) {
	Node *node = p_node_data->node;

	for (NetVarId var_id = 0; var_id < p_node_data->vars.size(); var_id += 1) {
		if (p_node_data->vars[var_id].enabled == false) {
			continue;
		}

		const Variant old_val = p_node_data->vars[var_id].var.value;
		const Variant new_val = node->get(p_node_data->vars[var_id].var.name);

		if (!compare(old_val, new_val)) {
			p_node_data->vars[var_id].var.value = new_val.duplicate(true);
			change_event_add(
					p_node_data,
					var_id,
					old_val);
		}
	}
}

Synchronizer::Synchronizer(SceneSynchronizer *p_node) :
		scene_synchronizer(p_node) {
}
