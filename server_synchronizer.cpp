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

#include "server_synchronizer.h"

#include "client_synchronizer.h"
#include "core/method_bind_ext.gen.inc"
#include "core/os/os.h"
#include "input_network_encoder.h"
#include "networked_controller.h"
#include "scene_diff.h"
#include "scene_synchronizer.h"
#include "scene_synchronizer_debugger.h"

#include "godot_backward_utility_cpp.h"

ServerSynchronizer::ServerSynchronizer(SceneSynchronizer *p_node) :
		Synchronizer(p_node) {
	SceneSynchronizerDebugger::singleton()->setup_debugger("server", 0, scene_synchronizer->get_tree());
}

void ServerSynchronizer::clear() {
	state_notifier_timer = 0.0;
	// Release the internal memory.
	changes.reset();
}

void ServerSynchronizer::process() {
	SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, "ServerSynchronizer::process", true);

	scene_synchronizer->update_peers();

	const double physics_ticks_per_second = Engine::get_singleton()->get_iterations_per_second();
	const double delta = 1.0 / physics_ticks_per_second;

	SceneSynchronizerDebugger::singleton()->scene_sync_process_start(scene_synchronizer);

	// Process the scene
	for (uint32_t i = 0; i < scene_synchronizer->node_data.size(); i += 1) {
		NetUtility::NodeData *nd = scene_synchronizer->node_data[i];
		nd->process(delta);
	}

	// Process the controllers_node_data
	for (uint32_t i = 0; i < scene_synchronizer->node_data_controllers.size(); i += 1) {
		NetUtility::NodeData *nd = scene_synchronizer->node_data_controllers[i];
		static_cast<NetworkedController *>(nd->node)->get_server_controller()->process(delta);
	}

	// Process the actions
	execute_actions();

	// Pull the changes.
	scene_synchronizer->change_events_begin(NetEventFlag::CHANGE);
	for (uint32_t i = 0; i < scene_synchronizer->node_data.size(); i += 1) {
		NetUtility::NodeData *nd = scene_synchronizer->node_data[i];
		scene_synchronizer->pull_node_changes(nd);
	}
	scene_synchronizer->change_events_flush();

	process_snapshot_notificator(delta);

	SceneSynchronizerDebugger::singleton()->scene_sync_process_end(scene_synchronizer);

	clean_pending_actions();
	check_missing_actions();

#if DEBUG_ENABLED
	// Write the debug dump for each peer.
	for (
			OAHashMap<int, NetUtility::PeerData>::Iterator peer_it = scene_synchronizer->peer_data.iter();
			peer_it.valid;
			peer_it = scene_synchronizer->peer_data.next_iter(peer_it)) {
		if (unlikely(peer_it.value->controller_id == UINT32_MAX)) {
			continue;
		}

		const NetUtility::NodeData *nd = scene_synchronizer->get_node_data(peer_it.value->controller_id);
		const uint32_t current_input_id = static_cast<const NetworkedController *>(nd->node)->get_server_controller()->get_current_input_id();
		SceneSynchronizerDebugger::singleton()->write_dump(*(peer_it.key), current_input_id);
	}
	SceneSynchronizerDebugger::singleton()->start_new_frame();
#endif
}

void ServerSynchronizer::on_node_added(NetUtility::NodeData *p_node_data) {
#ifdef DEBUG_ENABLED
	// Can't happen on server
	CRASH_COND(scene_synchronizer->is_recovered());
	// On server the ID is always known.
	CRASH_COND(p_node_data->id == UINT32_MAX);
#endif

	if (changes.size() <= p_node_data->id) {
		changes.resize(p_node_data->id + 1);
	}

	changes[p_node_data->id].not_known_before = true;
}

void ServerSynchronizer::on_node_removed(NetUtility::NodeData *p_node_data) {
	// Remove the actions as the `NodeData` is gone.
	for (int64_t i = int64_t(server_actions.size()) - 1; i >= 0; i -= 1) {
		if (server_actions[i].action_processor.nd == p_node_data) {
			server_actions.remove_unordered(i);
		}
	}
}

void ServerSynchronizer::on_variable_added(NetUtility::NodeData *p_node_data, const StringName &p_var_name) {
#ifdef DEBUG_ENABLED
	// Can't happen on server
	CRASH_COND(scene_synchronizer->is_recovered());
	// On server the ID is always known.
	CRASH_COND(p_node_data->id == UINT32_MAX);
#endif

	if (changes.size() <= p_node_data->id) {
		changes.resize(p_node_data->id + 1);
	}

	changes[p_node_data->id].vars.insert(p_var_name);
	changes[p_node_data->id].uknown_vars.insert(p_var_name);
}

void ServerSynchronizer::on_variable_changed(NetUtility::NodeData *p_node_data, NetVarId p_var_id, const Variant &p_old_value, int p_flag) {
#ifdef DEBUG_ENABLED
	// Can't happen on server
	CRASH_COND(scene_synchronizer->is_recovered());
	// On server the ID is always known.
	CRASH_COND(p_node_data->id == UINT32_MAX);
#endif

	if (changes.size() <= p_node_data->id) {
		changes.resize(p_node_data->id + 1);
	}

	changes[p_node_data->id].vars.insert(p_node_data->vars[p_var_id].var.name);
}

void ServerSynchronizer::on_action_triggered(
		NetUtility::NodeData *p_node_data,
		NetActionId p_id,
		const Array &p_arguments,
		const Vector<int> &p_recipients) {
	// The server can just trigger the action.

	// Definte the action index.
	const uint32_t server_action_token = server_actions_count;
	server_actions_count += 1;

	const uint32_t index = server_actions.size();
	server_actions.resize(index + 1);

	server_actions[index].prepare_processor(p_node_data, p_id, p_arguments);
	server_actions[index].sender_peer = 1;
	server_actions[index].triggerer_action_token = server_action_token;
	server_actions[index].action_token = server_action_token;

	// Trigger this action on the next tick.
	for (
			OAHashMap<int, uint32_t>::Iterator it = peers_next_action_trigger_input_id.iter();
			it.valid;
			it = peers_next_action_trigger_input_id.next_iter(it)) {
		server_actions[index].peers_executed_input_id[*it.key] = *it.value;
	}
	server_actions[index].recipients = p_recipients;

	// Now the server can propagate the actions to the clients.
	send_actions_to_clients();
}

void ServerSynchronizer::on_actions_received(
		int p_sender_peer,
		const LocalVector<SenderNetAction> &p_actions) {
	NetActionSenderInfo *sender = senders_info.lookup_ptr(p_sender_peer);
	if (sender == nullptr) {
		senders_info.set(p_sender_peer, NetActionSenderInfo());
		sender = senders_info.lookup_ptr(p_sender_peer);
	}

	NetworkedController *controller = scene_synchronizer->fetch_controller_by_peer(p_sender_peer);
	ERR_FAIL_COND_MSG(controller == nullptr, "[FATAL] The peer `" + itos(p_sender_peer) + "` is not associated to any controller, though an Action was generated by the client.");

	for (uint32_t g = 0; g < p_actions.size(); g += 1) {
		const SenderNetAction &action = p_actions[g];

		ERR_CONTINUE_MSG(action.get_action_info().can_client_trigger, "[CHEATER WARNING] This action `" + action.get_action_info().act_func + "` is not supposed to be triggered by the client. Is the client cheating? (Normally the NetSync aborts this kind of request on client side).");

		const bool already_received = sender->process_received_action(action.action_token);
		if (already_received) {
			// Already received: nothing to do.
			continue;
		}

		// Validate the action.
		if (!action.action_processor.server_validate()) {
			// The action is not valid just discard it.
			SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, "The `" + action.action_processor + "` action validation returned `false`. The action is discarded. SenderPeer: `" + itos(p_sender_peer) + "`.");
			continue;
		}

		// The server assigns a new action index, so we can globally reference the actions using a
		// single number.
		const uint32_t server_action_token = server_actions_count;
		server_actions_count += 1;

		const uint32_t index = server_actions.size();
		server_actions.push_back(action);
		server_actions[index].triggerer_action_token = server_actions[index].action_token;
		server_actions[index].action_token = server_action_token;

		// Set the action execution so for all peer based on when it was executed on the client.
		const uint32_t action_executed_input_id = action.peer_get_executed_input_id(p_sender_peer);
		if (action.get_action_info().wait_server_validation ||
				controller->get_current_input_id() >= action_executed_input_id) {
			// The action will be executed by the sxerver ASAP:
			// - This can happen if `wait_server_validation` is used
			// - The action was received too late. It's necessary to re-schedule the action and notify the
			//   client.

			for (
					OAHashMap<int, uint32_t>::Iterator it = peers_next_action_trigger_input_id.iter();
					it.valid;
					it = peers_next_action_trigger_input_id.next_iter(it)) {
				// This code set the `executed_input_id` to the next frame: similarly as done when the
				// `Action is triggered on the server (check `ServerSynchronizer::on_action_triggered`).
				server_actions[index].peers_executed_input_id[*it.key] = *it.value;
			}

			// Notify the sender client to adjust its snapshots.
			server_actions[index].sender_executed_time_changed = true;

		} else {
			ERR_CONTINUE_MSG(action_executed_input_id == UINT32_MAX, "[FATAL] The `executed_input_id` is missing on the received action: The action is not `wait_server_validation` so the `input_id` should be available.");

			// All is looking good. Set the action input_id to other peers relative to the sender_peer:
			// so to execute the Action in Sync.
			const uint32_t delta_actions = action_executed_input_id - controller->get_current_input_id();

			for (
					OAHashMap<int, uint32_t>::Iterator it = peers_next_action_trigger_input_id.iter();
					it.valid;
					it = peers_next_action_trigger_input_id.next_iter(it)) {
				if ((*it.key) == p_sender_peer) {
					// Already set.
					continue;
				} else {
					// Each controller has its own `input_id`: This code calculates and set the `input_id`
					// relative to the specific peer, so that all will execute the action at the right time.
					NetworkedController *peer_controller = scene_synchronizer->fetch_controller_by_peer(*it.key);
					ERR_CONTINUE(peer_controller == nullptr);
					server_actions[index].peers_executed_input_id[*it.key] = peer_controller->get_current_input_id() + delta_actions;
				}
			}
		}
	}

	// Now the server can propagate the actions to the clients.
	send_actions_to_clients();
}

void ServerSynchronizer::process_snapshot_notificator(real_t p_delta) {
	if (scene_synchronizer->peer_data.empty()) {
		// No one is listening.
		return;
	}

	// Notify the state if needed
	state_notifier_timer += p_delta;
	const bool notify_state = state_notifier_timer >= scene_synchronizer->get_server_notify_state_interval();

	if (notify_state) {
		state_notifier_timer = 0.0;
	}

	Vector<Variant> full_global_nodes_snapshot;
	Vector<Variant> delta_global_nodes_snapshot;
	for (
			OAHashMap<int, NetUtility::PeerData>::Iterator peer_it = scene_synchronizer->peer_data.iter();
			peer_it.valid;
			peer_it = scene_synchronizer->peer_data.next_iter(peer_it)) {
		if (unlikely(peer_it.value->enabled == false)) {
			// This peer is disabled.
			continue;
		}
		if (peer_it.value->force_notify_snapshot == false && notify_state == false) {
			// Nothing to do.
			continue;
		}

		peer_it.value->force_notify_snapshot = false;

		Vector<Variant> snap;

		NetUtility::NodeData *nd = peer_it.value->controller_id == UINT32_MAX ? nullptr : scene_synchronizer->get_node_data(peer_it.value->controller_id);
		if (nd) {
			// Add the controller input id at the beginning of the frame.
			snap.push_back(true);
			NetworkedController *controller = static_cast<NetworkedController *>(nd->node);
			snap.push_back(controller->get_current_input_id());

			ERR_CONTINUE_MSG(nd->is_controller == false, "[BUG] The NodeData fetched is not a controller: `" + nd->node->get_path() + "`.");
			controller_generate_snapshot(nd, peer_it.value->need_full_snapshot, snap);
		} else {
			snap.push_back(false);
		}

		if (peer_it.value->need_full_snapshot) {
			peer_it.value->need_full_snapshot = false;
			if (full_global_nodes_snapshot.size() == 0) {
				full_global_nodes_snapshot = global_nodes_generate_snapshot(true);
			}
			snap.append_array(full_global_nodes_snapshot);

		} else {
			if (delta_global_nodes_snapshot.size() == 0) {
				delta_global_nodes_snapshot = global_nodes_generate_snapshot(false);
			}
			snap.append_array(delta_global_nodes_snapshot);
		}

		scene_synchronizer->rpc_id(*peer_it.key, SNAME("_rpc_send_state"), snap);

		if (nd) {
			NetworkedController *controller = static_cast<NetworkedController *>(nd->node);
			controller->get_server_controller()->notify_send_state();
		}
	}

	if (notify_state) {
		// The state got notified, mark this as checkpoint so the next state
		// will contains only the changed things.
		changes.clear();
	}
}

Vector<Variant> ServerSynchronizer::global_nodes_generate_snapshot(bool p_force_full_snapshot) const {
	Vector<Variant> snapshot_data;

	for (uint32_t i = 0; i < scene_synchronizer->node_data.size(); i += 1) {
		const NetUtility::NodeData *node_data = scene_synchronizer->node_data[i];

		if (node_data == nullptr) {
			continue;

		} else if (node_data->is_controller || node_data->controlled_by != nullptr) {
			// Stkip any controller.
			continue;

		} else {
			generate_snapshot_node_data(
					node_data,
					p_force_full_snapshot ? SNAPSHOT_GENERATION_MODE_FORCE_FULL : SNAPSHOT_GENERATION_MODE_NORMAL,
					snapshot_data);
		}
	}

	return snapshot_data;
}

void ServerSynchronizer::controller_generate_snapshot(
		const NetUtility::NodeData *p_node_data,
		bool p_force_full_snapshot,
		Vector<Variant> &r_snapshot_result) const {
	CRASH_COND(p_node_data->is_controller == false);

	// Add the Controller and Controlled node `NodePath`: if unknown.
	for (uint32_t i = 0; i < scene_synchronizer->node_data.size(); i += 1) {
		const NetUtility::NodeData *node_data = scene_synchronizer->node_data[i];

		if (node_data == nullptr) {
			continue;

		} else if (node_data->is_controller == false && node_data->controlled_by == nullptr) {
			// This is not a controller, skip.
			continue;

		} else if (node_data == p_node_data || node_data->controlled_by == p_node_data) {
			// Skip this node because we will collect those info just after this loop.
			// Here we want to collect only the other controllers.
			continue;
		}

		// This is a controller, network only the `NodePath` if it`s unkwnown.
		generate_snapshot_node_data(
				node_data,
				p_force_full_snapshot ? SNAPSHOT_GENERATION_MODE_FORCE_NODE_PATH_ONLY : SNAPSHOT_GENERATION_MODE_NODE_PATH_ONLY,
				r_snapshot_result);
	}

	generate_snapshot_node_data(
			p_node_data,
			p_force_full_snapshot ? SNAPSHOT_GENERATION_MODE_FORCE_FULL : SNAPSHOT_GENERATION_MODE_NORMAL,
			r_snapshot_result);

	for (uint32_t i = 0; i < p_node_data->controlled_nodes.size(); i += 1) {
		generate_snapshot_node_data(
				p_node_data->controlled_nodes[i],
				p_force_full_snapshot ? SNAPSHOT_GENERATION_MODE_FORCE_FULL : SNAPSHOT_GENERATION_MODE_NORMAL,
				r_snapshot_result);
	}
}

void ServerSynchronizer::generate_snapshot_node_data(
		const NetUtility::NodeData *p_node_data,
		SnapshotGenerationMode p_mode,
		Vector<Variant> &r_snapshot_data) const {
	// The packet data is an array that contains the informations to update the
	// client snapshot.
	//
	// It's composed as follows:
	//  [NODE, VARIABLE, Value, VARIABLE, Value, VARIABLE, value, NIL,
	//  NODE, INPUT ID, VARIABLE, Value, VARIABLE, Value, NIL,
	//  NODE, VARIABLE, Value, VARIABLE, Value, NIL]
	//
	// Each node ends with a NIL, and the NODE and the VARIABLE are special:
	// - NODE, can be an array of two variables [Net Node ID, NodePath] or directly
	//         a Node ID. Obviously the array is sent only the first time.
	// - INPUT ID, this is optional and is used only when the node is a controller.
	// - VARIABLE, can be an array with the ID and the variable name, or just
	//              the ID; similarly as is for the NODE the array is send only
	//              the first time.

	if (p_node_data->node == nullptr || p_node_data->node->is_inside_tree() == false) {
		return;
	}

	const bool force_using_node_path = p_mode == SNAPSHOT_GENERATION_MODE_FORCE_FULL || p_mode == SNAPSHOT_GENERATION_MODE_FORCE_NODE_PATH_ONLY || p_mode == SNAPSHOT_GENERATION_MODE_NODE_PATH_ONLY;
	const bool force_snapshot_node_path = p_mode == SNAPSHOT_GENERATION_MODE_FORCE_FULL || p_mode == SNAPSHOT_GENERATION_MODE_FORCE_NODE_PATH_ONLY;
	const bool force_snapshot_variables = p_mode == SNAPSHOT_GENERATION_MODE_FORCE_FULL;
	const bool skip_snapshot_variables = p_mode == SNAPSHOT_GENERATION_MODE_FORCE_NODE_PATH_ONLY || p_mode == SNAPSHOT_GENERATION_MODE_NODE_PATH_ONLY;
	const bool force_using_variable_name = p_mode == SNAPSHOT_GENERATION_MODE_FORCE_FULL;

	const Change *change = p_node_data->id >= changes.size() ? nullptr : changes.ptr() + p_node_data->id;

	const bool unknown = change != nullptr && change->not_known_before;
	const bool node_has_changes = change != nullptr && change->vars.empty() == false;

	// Insert NODE DATA.
	Variant snap_node_data;
	if (force_using_node_path || unknown) {
		Vector<Variant> _snap_node_data;
		_snap_node_data.resize(2);
		_snap_node_data.write[0] = p_node_data->id;
		_snap_node_data.write[1] = p_node_data->node->get_path();
		snap_node_data = _snap_node_data;
	} else {
		// This node is already known on clients, just set the node ID.
		snap_node_data = p_node_data->id;
	}

	if ((node_has_changes && skip_snapshot_variables == false) || force_snapshot_node_path || unknown) {
		r_snapshot_data.push_back(snap_node_data);
	} else {
		// It has no changes, skip this node.
		return;
	}

	if (force_snapshot_variables || (node_has_changes && skip_snapshot_variables == false)) {
		// Insert the node variables.
		for (uint32_t i = 0; i < p_node_data->vars.size(); i += 1) {
			const NetUtility::VarData &var = p_node_data->vars[i];
			if (var.enabled == false) {
				continue;
			}

			if (force_snapshot_variables == false && change->vars.has(var.var.name) == false) {
				// This is a delta snapshot and this variable is the same as
				// before. Skip it.
				continue;
			}

			Variant var_info;
			if (force_using_variable_name || change->uknown_vars.has(var.var.name)) {
				Vector<Variant> _var_info;
				_var_info.resize(2);
				_var_info.write[0] = var.id;
				_var_info.write[1] = var.var.name;
				var_info = _var_info;
			} else {
				var_info = var.id;
			}

			r_snapshot_data.push_back(var_info);
			r_snapshot_data.push_back(var.var.value);
		}
	}

	// Insert NIL.
	r_snapshot_data.push_back(Variant());
}

void ServerSynchronizer::execute_actions() {
	for (uint32_t i = 0; i < server_actions.size(); i += 1) {
		if (server_actions[i].locally_executed) {
			// Already executed.
			continue;
		}

		// Take the controller associated to the sender_peer, to extract the current `input_id`.
		int sender_peer = server_actions[i].sender_peer;
		uint32_t executed_input_id = UINT32_MAX;
		if (sender_peer == 1) {
			if (unlikely(scene_synchronizer->peer_data.iter().valid == false)) {
				// No peers to take as reference to execute this Action, so just execute it right away.
				server_actions[i].locally_executed = true;
				server_actions[i].action_processor.execute();
				continue;
			}

			// Since this action was triggered by the server, and the server specify as
			// execution_input_id the same delta for all the peers: in order to execute this action
			// on the server we can just use any peer as reference to know when it's the right time
			// to execute the Action.
			// So it uses the first available peer.
			sender_peer = *scene_synchronizer->peer_data.iter().key;
		}

		NetworkedController *controller = scene_synchronizer->fetch_controller_by_peer(sender_peer);

		if (unlikely(controller == nullptr)) {
			SceneSynchronizerDebugger::singleton()->debug_warning(scene_synchronizer, "ServerSnchronizer::execute_actions. The peer `" + itos(sender_peer) + "` doesn't have any controller associated, but the Action (`" + server_actions[i].action_processor + "`) was generated. Maybe the character disconnected?");
			server_actions[i].locally_executed = true;
			server_actions[i].action_processor.execute();
			continue;
		}

		executed_input_id = server_actions[i].peer_get_executed_input_id(sender_peer);
		if (unlikely(executed_input_id == UINT32_MAX)) {
			SceneSynchronizerDebugger::singleton()->debug_error(scene_synchronizer, "[FATAL] The `executed_input_id` is `UINT32_MAX` which means it was unable to fetch the `controller_input_id` from the peer `" + itos(executed_input_id) + "`. Action: `" + server_actions[i].action_processor + "`");
			// This is likely a bug, so do not even bother executing it.
			// Marking as executed so this action is dropped.
			server_actions[i].locally_executed = true;
			continue;
		}

		if (controller->get_current_input_id() >= executed_input_id) {
			if (unlikely(controller->get_current_input_id() > executed_input_id)) {
				SceneSynchronizerDebugger::singleton()->debug_warning(scene_synchronizer, "ServerSnchronizer::execute_actions. The action `" + server_actions[i].action_processor + "` was planned to be executed on the frame `" + itos(executed_input_id) + "` while the current controller (`" + controller->get_path() + "`) frame is `" + itos(controller->get_current_input_id()) + "`. Since the execution_frame is adjusted when the action is received on the server, this case is triggered when the client stop communicating for some time and some inputs are skipped.");
			}

			// It's time to execute the Action, Yey!
			server_actions[i].locally_executed = true;
			server_actions[i].action_processor.execute();
		}
	}

	// Advance the action `input_id` for each peer, so we know when the next action will be triggered.
	for (OAHashMap<int, NetUtility::PeerData>::Iterator it = scene_synchronizer->peer_data.iter();
			it.valid;
			it = scene_synchronizer->peer_data.next_iter(it)) {
		// The peer
		const int peer_id = *it.key;

		NetworkedController *controller = scene_synchronizer->fetch_controller_by_peer(peer_id);
		if (controller && controller->get_current_input_id() != UINT32_MAX) {
			peers_next_action_trigger_input_id.set(peer_id, controller->get_current_input_id() + 1);
		}
	}
}

void ServerSynchronizer::send_actions_to_clients() {
	const uint64_t now = OS::get_singleton()->get_ticks_msec();

	LocalVector<SenderNetAction *> packet_actions;

	// First take the significant actions to network.
	for (uint32_t i = 0; i < server_actions.size(); i += 1) {
		if (int(server_actions[i].send_count) >= scene_synchronizer->get_actions_redundancy()) {
			// Nothing to do.
			continue;
		}

		if ((server_actions[i].send_timestamp + 2 /*ms - give some room*/) > now) {
			// Nothing to do.
			continue;
		}

		server_actions[i].send_timestamp = now;
		server_actions[i].send_count += 1;
		packet_actions.push_back(&server_actions[i]);
	}

	if (packet_actions.size() == 0) {
		// Nothing to send.
		return;
	}

	// For each peer
	for (OAHashMap<int, NetUtility::PeerData>::Iterator it = scene_synchronizer->peer_data.iter();
			it.valid;
			it = scene_synchronizer->peer_data.next_iter(it)) {
		// Send to peers.
		const int peer_id = *it.key;

		// Collects the actions importants for this peer.
		LocalVector<SenderNetAction *> peer_packet_actions;
		for (uint32_t i = 0; i < packet_actions.size(); i += 1) {
			if (
					(
							packet_actions[i]->sender_peer == peer_id &&
							packet_actions[i]->get_action_info().wait_server_validation == false &&
							packet_actions[i]->sender_executed_time_changed == false) ||
					(packet_actions[i]->recipients.size() > 0 &&
							packet_actions[i]->recipients.find(peer_id) == -1)) {
				// This peer must not receive the action.
				continue;
			} else {
				// This peer has to receive and execute this action.
				peer_packet_actions.push_back(packet_actions[i]);
			}
		}

		if (peer_packet_actions.size() == 0) {
			// Nothing to network for this peer.
			continue;
		}

		// Encode the actions.
		DataBuffer db;
		db.begin_write(0);
		net_action::encode_net_action(packet_actions, peer_id, db);
		db.dry();

		// Send the action to the peer.
		scene_synchronizer->rpc_unreliable_id(
				peer_id,
				"_rpc_send_actions",
				db.get_buffer().get_bytes());
	}
}

void ServerSynchronizer::clean_pending_actions() {
	// The packet will contains the most recent actions.
	for (int64_t i = int64_t(server_actions.size()) - 1; i >= 0; i -= 1) {
		if (
				server_actions[i].locally_executed == false ||
				int(server_actions[i].send_count) < scene_synchronizer->get_actions_redundancy()) {
			// Still somethin to do.
			continue;
		}

		server_actions.remove_unordered(i);
	}
}

void ServerSynchronizer::check_missing_actions() {
	for (
			OAHashMap<int, NetActionSenderInfo>::Iterator it = senders_info.iter();
			it.valid;
			it = senders_info.next_iter(it)) {
		it.value->check_missing_actions_and_clean_up(scene_synchronizer);
	}
}
