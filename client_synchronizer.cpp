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

#include "client_synchronizer.h"

#include "core/method_bind_ext.gen.inc"
#include "core/os/os.h"
#include "input_network_encoder.h"
#include "networked_controller.h"
#include "scene_diff.h"
#include "scene_synchronizer.h"
#include "scene_synchronizer_debugger.h"

#include "godot_backward_utility_cpp.h"

ClientSynchronizer::ClientSynchronizer(SceneSynchronizer *p_node) :
		Synchronizer(p_node) {
	clear();

	SceneSynchronizerDebugger::singleton()->setup_debugger("client", 0, scene_synchronizer->get_tree());
}

void ClientSynchronizer::clear() {
	player_controller_node_data = nullptr;
	node_paths.clear();
	last_received_snapshot.input_id = UINT32_MAX;
	last_received_snapshot.node_vars.clear();
	client_snapshots.clear();
	server_snapshots.clear();
	last_checked_input = 0;
	enabled = true;
	need_full_snapshot_notified = false;
}

void ClientSynchronizer::process() {
	SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, "ClientSynchronizer::process", true);

	if (unlikely(player_controller_node_data == nullptr || enabled == false)) {
		// No player controller or disabled so nothing to do.
		return;
	}

	const double physics_ticks_per_second = Engine::get_singleton()->get_iterations_per_second();
	const double delta = 1.0 / physics_ticks_per_second;

#ifdef DEBUG_ENABLED
	if (unlikely(Engine::get_singleton()->get_frames_per_second() < physics_ticks_per_second)) {
		const bool silent = !ProjectSettings::get_singleton()->get_setting("NetworkSynchronizer/debugger/log_debug_fps_warnings");
		SceneSynchronizerDebugger::singleton()->debug_warning(scene_synchronizer, "Current FPS is " + itos(Engine::get_singleton()->get_frames_per_second()) + ", but the minimum required FPS is " + itos(physics_ticks_per_second) + ", the client is unable to generate enough inputs for the server.", silent);
	}
#endif

	NetworkedController *controller = static_cast<NetworkedController *>(player_controller_node_data->node);
	PlayerController *player_controller = controller->get_player_controller();

	// Reset this here, so even when `sub_ticks` is zero (and it's not
	// updated due to process is not called), we can still have the corect
	// data.
	controller->player_set_has_new_input(false);

	// Due to some lag we may want to speed up the input_packet
	// generation, for this reason here I'm performing a sub tick.
	//
	// keep in mind that we are just pretending that the time
	// is advancing faster, for this reason we are still using
	// `delta` to step the controllers_node_data.
	//
	// The dolls may want to speed up too, so to consume the inputs faster
	// and get back in time with the server.
	int sub_ticks = player_controller->calculates_sub_ticks(delta, physics_ticks_per_second);

	if (sub_ticks == 0) {
		SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, "No sub ticks: this is not bu a bug; it's the lag compensation algorithm.", true);
	}

	while (sub_ticks > 0) {
		SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, "ClientSynchronizer::process::sub_process " + itos(sub_ticks), true);
		SceneSynchronizerDebugger::singleton()->scene_sync_process_start(scene_synchronizer);

		// Process the scene.
		for (uint32_t i = 0; i < scene_synchronizer->node_data.size(); i += 1) {
			NetUtility::NodeData *nd = scene_synchronizer->node_data[i];
			nd->process(delta);
		}

		// Process the player controllers_node_data.
		player_controller->process(delta);

		// Process the actions
		for (uint32_t i = 0; i < pending_actions.size(); i += 1) {
			if (pending_actions[i].locally_executed) {
				// Already executed.
				continue;
			}

			if (pending_actions[i].client_get_executed_input_id() > player_controller->get_current_input_id()) {
				// Not time yet.
				continue;
			}

#ifdef DEBUG_ENABLED
			if (pending_actions[i].sent_by_the_server == false) {
				// The executed_frame is set using `actions_input_id` which is correctly advanced: so itsn't
				// expected that this is different. Please make sure this never happens.
				CRASH_COND_MSG(pending_actions[i].client_get_executed_input_id() != player_controller->get_current_input_id(), "Action executed_input_id: `" + itos(pending_actions[i].client_get_executed_input_id()) + "` is different from current action `" + itos(player_controller->get_current_input_id()) + "`");
			}
#endif

			pending_actions[i].locally_executed = true;
			pending_actions[i].action_processor.execute();
		}

		actions_input_id = player_controller->get_current_input_id() + 1;

		// Pull the changes.
		scene_synchronizer->change_events_begin(NetEventFlag::CHANGE);
		for (NetNodeId i = 0; i < scene_synchronizer->node_data.size(); i += 1) {
			NetUtility::NodeData *nd = scene_synchronizer->node_data[i];
			scene_synchronizer->pull_node_changes(nd);
		}
		scene_synchronizer->change_events_flush();

		if (controller->player_has_new_input()) {
			store_snapshot();
		}

		sub_ticks -= 1;
		SceneSynchronizerDebugger::singleton()->scene_sync_process_end(scene_synchronizer);

#if DEBUG_ENABLED
		if (sub_ticks > 0) {
			// This is an intermediate sub tick, so store the dumping.
			// The last sub frame is not dumped, untile the end of the frame, so we can capture any subsequent message.
			const int client_peer = scene_synchronizer->get_multiplayer()->get_network_unique_id();
			SceneSynchronizerDebugger::singleton()->write_dump(client_peer, player_controller->get_current_input_id());
			SceneSynchronizerDebugger::singleton()->start_new_frame();
		}
#endif
	}

	process_controllers_recovery(delta);

	// Now trigger the END_SYNC event.
	scene_synchronizer->change_events_begin(NetEventFlag::END_SYNC);
	for (const Set<EndSyncEvent>::Element *e = sync_end_events.front();
			e != nullptr;
			e = e->next()) {
		// Check if the values between the variables before the sync and the
		// current one are different.
		if (scene_synchronizer->compare(
					e->get().node_data->vars[e->get().var_id].var.value,
					e->get().old_value) == false) {
			// Are different so we need to emit the `END_SYNC`.
			scene_synchronizer->change_event_add(
					e->get().node_data,
					e->get().var_id,
					e->get().old_value);
		}
	}
	sync_end_events.clear();

	scene_synchronizer->change_events_flush();

	send_actions_to_server();
	clean_pending_actions();
	check_missing_actions();

#if DEBUG_ENABLED
	const int client_peer = scene_synchronizer->get_multiplayer()->get_network_unique_id();
	SceneSynchronizerDebugger::singleton()->write_dump(client_peer, player_controller->get_current_input_id());
	SceneSynchronizerDebugger::singleton()->start_new_frame();
#endif
}

void ClientSynchronizer::receive_snapshot(Variant p_snapshot) {
	// The received snapshot is parsed and stored into the `last_received_snapshot`
	// that contains always the last received snapshot.
	// Later, the snapshot is stored into the server queue.
	// In this way, we are free to pop snapshot from the queue without wondering
	// about losing the data. Indeed the received snapshot is just and
	// incremental update so the last received data is always needed to fully
	// reconstruct it.

	SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, "The Client received the server snapshot.", true);

	// Parse server snapshot.
	const bool success = parse_snapshot(p_snapshot);

	if (success == false) {
		return;
	}

	// Finalize data.

	store_controllers_snapshot(
			last_received_snapshot,
			server_snapshots);
}

void ClientSynchronizer::on_node_added(NetUtility::NodeData *p_node_data) {
}

void ClientSynchronizer::on_node_removed(NetUtility::NodeData *p_node_data) {
	if (player_controller_node_data == p_node_data) {
		player_controller_node_data = nullptr;
		server_snapshots.clear();
		client_snapshots.clear();
	}

	// Remove the actions as the `NodeData` is gone.
	for (int64_t i = int64_t(pending_actions.size()) - 1; i >= 0; i -= 1) {
		if (pending_actions[i].action_processor.nd == p_node_data) {
			pending_actions.remove_unordered(i);
		}
	}

	if (p_node_data->id < uint32_t(last_received_snapshot.node_vars.size())) {
		last_received_snapshot.node_vars.ptrw()[p_node_data->id].clear();
	}
}

void ClientSynchronizer::on_variable_changed(NetUtility::NodeData *p_node_data, NetVarId p_var_id, const Variant &p_old_value, int p_flag) {
	if (p_flag & NetEventFlag::SYNC) {
		sync_end_events.insert(
				EndSyncEvent{
						p_node_data,
						p_var_id,
						p_old_value });
	}
}

void ClientSynchronizer::on_controller_reset(NetUtility::NodeData *p_node_data) {
#ifdef DEBUG_ENABLED
	CRASH_COND(p_node_data->is_controller == false);
#endif

	if (player_controller_node_data == p_node_data) {
		// Reset the node_data.
		player_controller_node_data = nullptr;
		server_snapshots.clear();
		client_snapshots.clear();
	}

	if (static_cast<NetworkedController *>(p_node_data->node)->is_player_controller()) {
		if (player_controller_node_data != nullptr) {
			SceneSynchronizerDebugger::singleton()->debug_error(scene_synchronizer, "Only one player controller is supported, at the moment. Make sure this is the case.");
		} else {
			// Set this player controller as active.
			player_controller_node_data = p_node_data;
			server_snapshots.clear();
			client_snapshots.clear();
		}
	}
}

void ClientSynchronizer::on_action_triggered(
		NetUtility::NodeData *p_node_data,
		NetActionId p_id,
		const Array &p_arguments,
		const Vector<int> &p_recipients) {
	ERR_FAIL_COND_MSG(p_recipients.size() > 0, "The client can't specify any peer, this feature is restricted to the server.");

	const uint32_t index = pending_actions.size();
	pending_actions.resize(index + 1);

	pending_actions[index].action_token = locally_triggered_actions_count;
	pending_actions[index].triggerer_action_token = locally_triggered_actions_count;
	locally_triggered_actions_count++;

	pending_actions[index].prepare_processor(p_node_data, p_id, p_arguments);

	if (!pending_actions[index].get_action_info().wait_server_validation) {
		// Will be immeditaly executed locally, set the execution frame now so we can network the
		// action right away before it's even executed locally: It's necessary to make this fast!
		pending_actions[index].client_set_executed_input_id(actions_input_id);
		pending_actions[index].locally_executed = false;
	} else {
		// Do not execute locally, the server will send it back once it's time.
		pending_actions[index].locally_executed = true;
	}

	pending_actions[index].sent_by_the_server = false;

	// Network the action.
	send_actions_to_server();
}

void ClientSynchronizer::on_actions_received(
		int sender_peer,
		const LocalVector<SenderNetAction> &p_actions) {
	ERR_FAIL_COND_MSG(sender_peer != 1, "[FATAL] Actions dropped becouse was not sent by the server!");

	for (uint32_t g = 0; g < p_actions.size(); g += 1) {
		const SenderNetAction &action = p_actions[g];

		const bool already_received = server_sender_info.process_received_action(action.action_token);
		if (already_received) {
			// Already known nothing to do.
			continue;
		}

		// Search the snapshot and add the Action to it.
		// NOTE: This is needed in case of rewind to take the action into account.
		NetworkedController *controller = static_cast<NetworkedController *>(player_controller_node_data->node);
		const uint32_t current_input_id = controller->get_current_input_id();

		if (action.client_get_executed_input_id() <= current_input_id) {
			// On the server this action was executed on an already passed frame, insert it inside the snapshot.
			// First search the snapshot:
			bool add_to_snapshots = false;
			for (uint32_t x = 0; x < client_snapshots.size(); x += 1) {
				if (client_snapshots[x].input_id == action.client_get_executed_input_id()) {
					// Insert the action inside the snapshot, so we can execute to reconcile the client and
					// the server: I'm using `UINT32_MAX` because we track only locally executed actions.
					client_snapshots[x].actions.push_back(TokenizedNetActionProcessor(UINT32_MAX, action.action_processor));
					add_to_snapshots = true;
					break;
				}
			}

			if (!add_to_snapshots) {
				SceneSynchronizerDebugger::singleton()->debug_warning(scene_synchronizer, "The Action `" + action.get_action_info().act_func + "` was not add to any snapshot as the snapshot was not found. The executed_input_id sent by the server is `" + itos(action.client_get_executed_input_id()) + "`. The actions is dropped.");
				continue;
			}
		}

		// Add the action to the pending actions so it's executed ASAP.
		const uint32_t index = pending_actions.size();
		pending_actions.push_back(action);
		// Never networkd this back to server: afterall the server just sent it!
		pending_actions[index].sent_by_the_server = true;

		if (action.sender_executed_time_changed) {
			// This action was generated by this peer, but it arrived too late to the server that
			// rescheduled it:
			// Remove the original Action from any stored snapshot to avoid executing it at the wrong time.
			ERR_CONTINUE_MSG(action.triggerer_action_token == UINT32_MAX, "[FATAL] The server sent an action marked with `sender_executed_time_changed` but the `truggerer_action_token` is not set, this is a bug. Report that.");

			for (uint32_t x = 0; x < client_snapshots.size(); x += 1) {
				const int64_t action_i = client_snapshots[x].actions.find(TokenizedNetActionProcessor(action.triggerer_action_token, NetActionProcessor()));
				if (action_i >= 0) {
					client_snapshots[x].actions.remove(action_i);
					break;
				}
			}

			if (pending_actions[index].get_action_info().wait_server_validation == false) {
				// Since it was already executed, no need to execute again, it's enough just put the Action
				// to the proper snapshot.
				pending_actions[index].locally_executed = true;
			} else {
				// Execute this locally.
				pending_actions[index].locally_executed = false;
			}
		} else {
			// Execute this locally.
			pending_actions[index].locally_executed = false;
		}
	}
}

void ClientSynchronizer::store_snapshot() {
	NetworkedController *controller = static_cast<NetworkedController *>(player_controller_node_data->node);

#ifdef DEBUG_ENABLED
	if (unlikely(client_snapshots.size() > 0 && controller->get_current_input_id() <= client_snapshots.back().input_id)) {
		CRASH_NOW_MSG("[FATAL] During snapshot creation, for controller " + controller->get_path() + ", was found an ID for an older snapshots. New input ID: " + itos(controller->get_current_input_id()) + " Last saved snapshot input ID: " + itos(client_snapshots.back().input_id) + ".");
	}
#endif

	client_snapshots.push_back(NetUtility::Snapshot());

	NetUtility::Snapshot &snap = client_snapshots.back();
	snap.input_id = controller->get_current_input_id();

	snap.node_vars.resize(scene_synchronizer->organized_node_data.size());

	// Store the nodes state and skip anything is related to the other
	// controllers.
	for (uint32_t i = 0; i < scene_synchronizer->organized_node_data.size(); i += 1) {
		const NetUtility::NodeData *node_data = scene_synchronizer->organized_node_data[i];

		if (node_data == nullptr) {
			// Nothing to do.
			continue;
		}

		if ((node_data->is_controller || node_data->controlled_by != nullptr) &&
				(node_data != player_controller_node_data && node_data->controlled_by != player_controller_node_data)) {
			// Ignore this controller.
			continue;
		}

		if (node_data->id >= uint32_t(snap.node_vars.size())) {
			// Make sure this ID is valid.
			ERR_FAIL_COND_MSG(node_data->id != UINT32_MAX, "[BUG] It's not expected that the client has a node with the NetNodeId (" + itos(node_data->id) + ") bigger than the registered node count: " + itos(snap.node_vars.size()));
			// Skip this node
			continue;
		}

		Vector<NetUtility::Var> *snap_node_vars = snap.node_vars.ptrw() + node_data->id;
		snap_node_vars->resize(node_data->vars.size());
		NetUtility::Var *vars = snap_node_vars->ptrw();
		for (uint32_t v = 0; v < node_data->vars.size(); v += 1) {
			if (node_data->vars[v].enabled) {
				vars[v] = node_data->vars[v].var;
			} else {
				vars[v].name = StringName();
			}
		}
	}

	// Store the actions
	for (uint32_t i = 0; i < pending_actions.size(); i += 1) {
		if (pending_actions[i].client_get_executed_input_id() != snap.input_id) {
			continue;
		}

		if (pending_actions[i].sent_by_the_server) {
			// Nothing to do because it was add on arrival into the correct snapshot.
		} else {
			snap.actions.push_back(TokenizedNetActionProcessor(pending_actions[i].action_token, pending_actions[i].action_processor));
		}
	}
}

void ClientSynchronizer::store_controllers_snapshot(
		const NetUtility::Snapshot &p_snapshot,
		std::deque<NetUtility::Snapshot> &r_snapshot_storage) {
	// Put the parsed snapshot into the queue.

	if (p_snapshot.input_id == UINT32_MAX && player_controller_node_data != nullptr) {
		// The snapshot doesn't have any info for this controller; Skip it.
		return;
	}

	if (p_snapshot.input_id == UINT32_MAX) {
		SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, "The Client received the server snapshot WITHOUT `input_id`.", true);
		// The controller node is not registered so just assume this snapshot is the most up-to-date.
		r_snapshot_storage.clear();
		r_snapshot_storage.push_back(p_snapshot);

	} else {
		SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, "The Client received the server snapshot: " + itos(p_snapshot.input_id), true);

		// Store the snapshot sorted by controller input ID.
		if (r_snapshot_storage.empty() == false) {
			// Make sure the snapshots are stored in order.
			const uint32_t last_stored_input_id = r_snapshot_storage.back().input_id;
			if (p_snapshot.input_id == last_stored_input_id) {
				// Update the snapshot.
				r_snapshot_storage.back() = p_snapshot;
				return;
			} else {
				ERR_FAIL_COND_MSG(p_snapshot.input_id < last_stored_input_id, "This snapshot (with ID: " + itos(p_snapshot.input_id) + ") is not expected because the last stored id is: " + itos(last_stored_input_id));
			}
		}

		r_snapshot_storage.push_back(p_snapshot);
	}
}

void ClientSynchronizer::apply_last_received_server_snapshot() {
	const Vector<NetUtility::Var> *vars = server_snapshots.back().node_vars.ptr();

	scene_synchronizer->change_events_begin(NetEventFlag::SYNC_RECOVER);
	for (int i = 0; i < server_snapshots.back().node_vars.size(); i += 1) {
		NetNodeId id = i;
		NetUtility::NodeData *nd = scene_synchronizer->get_node_data(id);
		for (int v = 0; v < vars[i].size(); v += 1) {
			const Variant current_val = nd->node->get(vars[i][v].name);
			if (scene_synchronizer->compare(current_val, vars[i][v].value)) {
				nd->node->set(vars[i][v].name, vars[i][v].value);
				scene_synchronizer->change_event_add(
						nd,
						v,
						current_val);
			}
		}
	}
	scene_synchronizer->change_events_flush();
}

void ClientSynchronizer::process_controllers_recovery(real_t p_delta) {
	// The client is responsible to recover only its local controller, while all
	// the other controllers_node_data (dolls) have their state interpolated. There is
	// no need to check the correctness of the doll state nor the needs to
	// rewind those.
	//
	// The scene, (global nodes), are always in sync with the reference frame
	// of the client.

	NetworkedController *controller = static_cast<NetworkedController *>(player_controller_node_data->node);
	PlayerController *player_controller = controller->get_player_controller();

	// --- Phase one: find the snapshot to check. ---
	if (server_snapshots.empty()) {
		// No snapshots to recover for this controller. Nothing to do.
		return;
	}

	if (server_snapshots.back().input_id == UINT32_MAX) {
		// The server last received snapshot is a no input snapshot. Just assume it's the most up-to-date.
		SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, "The client received a \"no input\" snapshot, so the client is setting it right away assuming is the most updated one.", true);

		apply_last_received_server_snapshot();

		server_snapshots.clear();
		client_snapshots.clear();
		return;
	}

#ifdef DEBUG_ENABLED
	if (client_snapshots.empty() == false) {
		// The SceneSynchronizer and the PlayerController are always in sync.
		CRASH_COND_MSG(client_snapshots.back().input_id != player_controller->last_known_input(), "This should not be possible: snapshot input: " + itos(client_snapshots.back().input_id) + " last_know_input: " + itos(player_controller->last_known_input()));
	}
#endif

	// Find the best recoverable input_id.
	uint32_t checkable_input_id = UINT32_MAX;
	// Find the best snapshot to recover from the one already
	// processed.
	if (client_snapshots.empty() == false) {
		for (
				auto s_snap = server_snapshots.rbegin();
				checkable_input_id == UINT32_MAX && s_snap != server_snapshots.rend();
				++s_snap) {
			for (auto c_snap = client_snapshots.begin(); c_snap != client_snapshots.end(); ++c_snap) {
				if (c_snap->input_id == s_snap->input_id) {
					// Server snapshot also found on client, can be checked.
					checkable_input_id = c_snap->input_id;
					break;
				}
			}
		}
	} else {
		// No client input, this happens when the stream is paused.
		process_paused_controller_recovery(p_delta);
		return;
	}

	if (checkable_input_id == UINT32_MAX) {
		// No snapshot found, nothing to do.
		return;
	}

#ifdef DEBUG_ENABLED
	// Unreachable cause the above check
	CRASH_COND(server_snapshots.empty());
	CRASH_COND(client_snapshots.empty());
#endif

	// Drop all the old server snapshots until the one that we need.
	while (server_snapshots.front().input_id < checkable_input_id) {
		server_snapshots.pop_front();
	}

	// Drop all the old client snapshots until the one that we need.
	while (client_snapshots.front().input_id < checkable_input_id) {
		client_snapshots.pop_front();
	}

#ifdef DEBUG_ENABLED
	// These are unreachable at this point.
	CRASH_COND(server_snapshots.empty());
	CRASH_COND(server_snapshots.front().input_id != checkable_input_id);

	// This is unreachable, because we store all the client shapshots
	// each time a new input is processed. Since the `checkable_input_id`
	// is taken by reading the processed doll inputs, it's guaranteed
	// that here the snapshot exists.
	CRASH_COND(client_snapshots.empty());
	CRASH_COND(client_snapshots.front().input_id != checkable_input_id);
#endif

	// --- Phase two: compare the server snapshot with the client snapshot. ---
	bool need_recover = false;
	bool recover_controller = false;
	LocalVector<NetUtility::NodeData *> nodes_to_recover;
	LocalVector<NetUtility::PostponedRecover> postponed_recover;

	__pcr__fetch_recovery_info(
			checkable_input_id,
			need_recover,
			recover_controller,
			nodes_to_recover,
			postponed_recover);

	// Popout the client snapshot.
	client_snapshots.pop_front();

	// --- Phase three: recover and reply. ---

	if (need_recover) {
		SceneSynchronizerDebugger::singleton()->notify_event(recover_controller ? SceneSynchronizerDebugger::FrameEvent::CLIENT_DESYNC_DETECTED : SceneSynchronizerDebugger::FrameEvent::CLIENT_DESYNC_DETECTED_SOFT);
		SceneSynchronizerDebugger::singleton()->add_node_message(scene_synchronizer, "Recover input: " + itos(checkable_input_id) + " - Last input: " + itos(player_controller->get_stored_input_id(-1)));

		// Add the postponed recover.
		for (uint32_t y = 0; y < postponed_recover.size(); ++y) {
			nodes_to_recover.push_back(postponed_recover[y].node_data);
		}

		if (recover_controller) {
			// Put the controlled and the controllers_node_data into the nodes to
			// rewind.
			// Note, the controller stuffs are added here to ensure that if the
			// controller need a recover, all its nodes are added; no matter
			// at which point the difference is found.
			nodes_to_recover.reserve(
					nodes_to_recover.size() +
					player_controller_node_data->controlled_nodes.size() +
					player_controller_node_data->dependency_nodes.size() +
					1);

			nodes_to_recover.push_back(player_controller_node_data);

			for (
					uint32_t y = 0;
					y < player_controller_node_data->controlled_nodes.size();
					y += 1) {
				nodes_to_recover.push_back(player_controller_node_data->controlled_nodes[y]);
			}

			for (
					uint32_t y = 0;
					y < player_controller_node_data->dependency_nodes.size();
					y += 1) {
				nodes_to_recover.push_back(player_controller_node_data->dependency_nodes[y]);
			}
		}

		__pcr__sync_pre_rewind(
				nodes_to_recover);

		// Rewind phase.
		__pcr__rewind(
				p_delta,
				checkable_input_id,
				controller,
				player_controller,
				recover_controller,
				nodes_to_recover);
	} else {
		__pcr__sync_no_rewind(postponed_recover);
		player_controller->notify_input_checked(checkable_input_id);
	}

	// Popout the server snapshot.
	server_snapshots.pop_front();

	last_checked_input = checkable_input_id;
}

void ClientSynchronizer::__pcr__fetch_recovery_info(
		const uint32_t p_input_id,
		bool &r_need_recover,
		bool &r_recover_controller,
		LocalVector<NetUtility::NodeData *> &r_nodes_to_recover,
		LocalVector<NetUtility::PostponedRecover> &r_postponed_recover) {
	r_need_recover = false;
	r_recover_controller = false;

	Vector<StringName> variable_names;
	Vector<Variant> server_values;
	Vector<Variant> client_values;

	r_nodes_to_recover.reserve(server_snapshots.front().node_vars.size());
	for (uint32_t net_node_id = 0; net_node_id < uint32_t(server_snapshots.front().node_vars.size()); net_node_id += 1) {
		NetUtility::NodeData *rew_node_data = scene_synchronizer->get_node_data(net_node_id);
		if (rew_node_data == nullptr || rew_node_data->sync_enabled == false) {
			continue;
		}

		bool recover_this_node = false;
		bool different = false;
		if (net_node_id >= uint32_t(client_snapshots.front().node_vars.size())) {
			SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, "Rewind is needed because the client snapshot doesn't contain this node: " + rew_node_data->node->get_path());
			recover_this_node = true;
			different = true;
		} else {
			NetUtility::PostponedRecover rec;

			different = compare_vars(
					rew_node_data,
					server_snapshots.front().node_vars[net_node_id],
					client_snapshots.front().node_vars[net_node_id],
					rec.vars);

			if (different) {
				SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, "Rewind on frame " + itos(p_input_id) + " is needed because the node on client is different: " + rew_node_data->node->get_path());
				recover_this_node = true;
			} else if (rec.vars.size() > 0) {
				rec.node_data = rew_node_data;
				r_postponed_recover.push_back(rec);
			}
		}

		if (recover_this_node) {
			r_need_recover = true;
			if (rew_node_data->controlled_by != nullptr ||
					rew_node_data->is_controller ||
					player_controller_node_data->dependency_nodes.find(rew_node_data) != -1) {
				// Controller node.
				r_recover_controller = true;
			} else {
				r_nodes_to_recover.push_back(rew_node_data);
			}
		}

#ifdef DEBUG_ENABLED
		if (different) {
			// Emit the de-sync detected signal.

			static const Vector<NetUtility::Var> const_empty_vector;
			const Vector<NetUtility::Var> &server_node_vars = uint32_t(server_snapshots.front().node_vars.size()) <= net_node_id ? const_empty_vector : server_snapshots.front().node_vars[net_node_id];
			const Vector<NetUtility::Var> &client_node_vars = uint32_t(client_snapshots.front().node_vars.size()) <= net_node_id ? const_empty_vector : client_snapshots.front().node_vars[net_node_id];

			const int count = MAX(server_node_vars.size(), client_node_vars.size());

			variable_names.resize(count);
			server_values.resize(count);
			client_values.resize(count);

			for (int g = 0; g < count; ++g) {
				if (g < server_node_vars.size()) {
					variable_names.ptrw()[g] = server_node_vars[g].name;
					server_values.ptrw()[g] = server_node_vars[g].value;
				} else {
					server_values.ptrw()[g] = Variant();
				}

				if (g < client_node_vars.size()) {
					variable_names.ptrw()[g] = client_node_vars[g].name;
					client_values.ptrw()[g] = client_node_vars[g].value;
				} else {
					client_values.ptrw()[g] = Variant();
				}
			}

			if (rew_node_data->node) {
				scene_synchronizer->emit_signal("desync_detected", p_input_id, rew_node_data->node, variable_names, client_values, server_values);
			} else {
				SceneSynchronizerDebugger::singleton()->debug_error(scene_synchronizer, "No node associated to `" + itos(net_node_id) + "`, was not possible to generate the event `desync_detected`.");
			}
		}
#endif
	}
}

void ClientSynchronizer::__pcr__sync_pre_rewind(
		const LocalVector<NetUtility::NodeData *> &p_nodes_to_recover) {
	// Apply the server snapshot so to go back in time till that moment,
	// so to be able to correctly reply the movements.
	scene_synchronizer->change_events_begin(NetEventFlag::SYNC_RECOVER | NetEventFlag::SYNC_RESET);
	for (uint32_t i = 0; i < p_nodes_to_recover.size(); i += 1) {
		if (p_nodes_to_recover[i]->id >= uint32_t(server_snapshots.front().node_vars.size())) {
			SceneSynchronizerDebugger::singleton()->debug_warning(scene_synchronizer, "The node: " + p_nodes_to_recover[i]->node->get_path() + " was not found on the server snapshot, this is not supposed to happen a lot.");
			continue;
		}
		if (p_nodes_to_recover[i]->sync_enabled == false) {
			// Don't sync this node.
			// This check is also here, because the `recover_controller`
			// mechanism, may have insert a no sync node.
			// The check is here because I feel it more clear, here.
			continue;
		}

#ifdef DEBUG_ENABLED
		// The parser make sure to properly initialize the snapshot variable
		// array size. So the following condition is always `false`.
		CRASH_COND(uint32_t(server_snapshots.front().node_vars[p_nodes_to_recover[i]->id].size()) != p_nodes_to_recover[i]->vars.size());
#endif

		Node *node = p_nodes_to_recover[i]->node;
		const Vector<NetUtility::Var> s_vars = server_snapshots.front().node_vars[p_nodes_to_recover[i]->id];
		const NetUtility::Var *s_vars_ptr = s_vars.ptr();

		SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, "Full reset node: " + node->get_path());

		for (int v = 0; v < s_vars.size(); v += 1) {
			if (s_vars_ptr[v].name == StringName()) {
				// This variable was not set, skip it.
				continue;
			}

			const Variant current_val = p_nodes_to_recover[i]->vars[v].var.value;
			p_nodes_to_recover[i]->vars[v].var.value = s_vars_ptr[v].value.duplicate(true);
			node->set(s_vars_ptr[v].name, s_vars_ptr[v].value);

			SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, " |- Variable: " + s_vars_ptr[v].name + " New value: " + s_vars_ptr[v].value);
			scene_synchronizer->change_event_add(
					p_nodes_to_recover[i],
					v,
					current_val);
		}
	}
	scene_synchronizer->change_events_flush();
}

void ClientSynchronizer::__pcr__rewind(
		real_t p_delta,
		const uint32_t p_checkable_input_id,
		NetworkedController *p_controller,
		PlayerController *p_player_controller,
		const bool p_recover_controller,
		const LocalVector<NetUtility::NodeData *> &p_nodes_to_recover) {
	const int remaining_inputs = p_player_controller->notify_input_checked(p_checkable_input_id);

#ifdef DEBUG_ENABLED
	// Unreachable because the SceneSynchronizer and the PlayerController
	// have the same stored data at this point.
	CRASH_COND(client_snapshots.size() != size_t(remaining_inputs));
#endif

#ifdef DEBUG_ENABLED
	// Used to double check all the instants have been processed.
	bool has_next = false;
#endif
	for (int i = 0; i < remaining_inputs; i += 1) {
		scene_synchronizer->change_events_begin(NetEventFlag::SYNC_RECOVER | NetEventFlag::SYNC_REWIND);

		// Step 1 -- Process the scene nodes.
		for (uint32_t r = 0; r < p_nodes_to_recover.size(); r += 1) {
			if (p_nodes_to_recover[r]->sync_enabled == false) {
				// This node is not sync.
				continue;
			}
			p_nodes_to_recover[r]->process(p_delta);
#ifdef DEBUG_ENABLED
			if (p_nodes_to_recover[r]->functions.size()) {
				SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, "Rewind, processed node: " + p_nodes_to_recover[r]->node->get_path());
			}
#endif
		}

		// Step 2 -- Process the controller.
		if (p_recover_controller && player_controller_node_data->sync_enabled) {
#ifdef DEBUG_ENABLED
			has_next =
#endif
					p_controller->process_instant(i, p_delta);
			SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, "Rewind, processed controller: " + p_controller->get_path());
		}

		// Step 3 -- Process the Actions.
#ifdef DEBUG_ENABLED
		// This can't happen because the client stores a snapshot for each frame.
		CRASH_COND(client_snapshots[i].input_id == p_checkable_input_id + i);
#endif
		for (int a_i = 0; a_i < client_snapshots[i].actions.size(); a_i += 1) {
			// Leave me alone, I don't want to make `execute()` const. 😫
			NetActionProcessor(client_snapshots[i].actions[a_i].processor).execute();
			SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, "Rewind, processed Action: " + String(client_snapshots[i].actions[a_i].processor));
		}

		// Step 4 -- Pull node changes and Update snapshots.
		for (uint32_t r = 0; r < p_nodes_to_recover.size(); r += 1) {
			if (p_nodes_to_recover[r]->sync_enabled == false) {
				// This node is not sync.
				continue;
			}
			// Pull changes
			scene_synchronizer->pull_node_changes(p_nodes_to_recover[r]);

			// Update client snapshot.
			if (uint32_t(client_snapshots[i].node_vars.size()) <= p_nodes_to_recover[r]->id) {
				client_snapshots[i].node_vars.resize(p_nodes_to_recover[r]->id + 1);
			}

			Vector<NetUtility::Var> *snap_node_vars = client_snapshots[i].node_vars.ptrw() + p_nodes_to_recover[r]->id;
			snap_node_vars->resize(p_nodes_to_recover[r]->vars.size());

			NetUtility::Var *vars = snap_node_vars->ptrw();
			for (uint32_t v = 0; v < p_nodes_to_recover[r]->vars.size(); v += 1) {
				vars[v] = p_nodes_to_recover[r]->vars[v].var;
			}
		}
		scene_synchronizer->change_events_flush();
	}

#ifdef DEBUG_ENABLED
	// Unreachable because the above loop consume all instants.
	CRASH_COND(has_next);
#endif
}

void ClientSynchronizer::__pcr__sync_no_rewind(const LocalVector<NetUtility::PostponedRecover> &p_postponed_recover) {
	// Apply found differences without rewind.
	scene_synchronizer->change_events_begin(NetEventFlag::SYNC_RECOVER);
	for (uint32_t i = 0; i < p_postponed_recover.size(); i += 1) {
		NetUtility::NodeData *rew_node_data = p_postponed_recover[i].node_data;
		if (rew_node_data->sync_enabled == false) {
			// This node sync is disabled.
			continue;
		}

		Node *node = rew_node_data->node;
		const NetUtility::Var *vars_ptr = p_postponed_recover[i].vars.ptr();

		SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, "[Snapshot partial reset] Node: " + node->get_path());

		// Set the value on the synchronizer too.
		for (int v = 0; v < p_postponed_recover[i].vars.size(); v += 1) {
			// We need to search it because the postponed recovered is not
			// aligned.
			// TODO This array is generated few lines above.
			// Can we store the ID too, so to avoid this search????
			const int rew_var_index = rew_node_data->vars.find(vars_ptr[v].name);
			// Unreachable, because when the snapshot is received the
			// algorithm make sure the `scene_synchronizer` is traking the
			// variable.
			CRASH_COND(rew_var_index <= -1);

			const Variant old_val = rew_node_data->vars[rew_var_index].var.value;
			rew_node_data->vars[rew_var_index].var.value = vars_ptr[v].value.duplicate(true);
			node->set(vars_ptr[v].name, vars_ptr[v].value);

			SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, " |- Variable: " + vars_ptr[v].name + "; old value: " + old_val + " new value: " + vars_ptr[v].value);
			scene_synchronizer->change_event_add(
					rew_node_data,
					rew_var_index,
					old_val);
		}

		// Update the last client snapshot.
		if (client_snapshots.empty() == false) {
			if (uint32_t(client_snapshots.back().node_vars.size()) <= rew_node_data->id) {
				client_snapshots.back().node_vars.resize(rew_node_data->id + 1);
			}

			Vector<NetUtility::Var> *snap_node_vars = client_snapshots.back().node_vars.ptrw() + rew_node_data->id;
			snap_node_vars->resize(rew_node_data->vars.size());

			NetUtility::Var *vars = snap_node_vars->ptrw();

			for (uint32_t v = 0; v < rew_node_data->vars.size(); v += 1) {
				vars[v] = rew_node_data->vars[v].var;
			}
		}
	}
	scene_synchronizer->change_events_flush();
}

void ClientSynchronizer::process_paused_controller_recovery(real_t p_delta) {
#ifdef DEBUG_ENABLED
	CRASH_COND(server_snapshots.empty());
	CRASH_COND(client_snapshots.empty() == false);
#endif

	// Drop the snapshots till the newest.
	while (server_snapshots.size() != 1) {
		server_snapshots.pop_front();
	}

#ifdef DEBUG_ENABLED
	CRASH_COND(server_snapshots.empty());
#endif
	scene_synchronizer->change_events_begin(NetEventFlag::SYNC_RECOVER);
	for (uint32_t net_node_id = 0; net_node_id < uint32_t(server_snapshots.front().node_vars.size()); net_node_id += 1) {
		NetUtility::NodeData *rew_node_data = scene_synchronizer->get_node_data(net_node_id);
		if (rew_node_data == nullptr || rew_node_data->sync_enabled == false) {
			continue;
		}

		Node *node = rew_node_data->node;

		const NetUtility::Var *snap_vars_ptr = server_snapshots.front().node_vars[net_node_id].ptr();
		for (int var_id = 0; var_id < server_snapshots.front().node_vars[net_node_id].size(); var_id += 1) {
			// Note: the snapshot variable array is ordered per var_id.
			const Variant old_val = rew_node_data->vars[var_id].var.value;
			if (!scene_synchronizer->compare(
						old_val,
						snap_vars_ptr[var_id].value)) {
				// Different
				rew_node_data->vars[var_id].var.value = snap_vars_ptr[var_id].value;
				node->set(snap_vars_ptr[var_id].name, snap_vars_ptr[var_id].value);
				SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, "[Snapshot paused controller] Node: " + node->get_path());
				SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, " |- Variable: " + snap_vars_ptr[var_id].name + "; value: " + snap_vars_ptr[var_id].value);
				scene_synchronizer->change_event_add(
						rew_node_data,
						var_id,
						old_val);
			}
		}
	}

	server_snapshots.pop_front();

	scene_synchronizer->change_events_flush();
}

bool ClientSynchronizer::parse_sync_data(
		Variant p_sync_data,
		void *p_user_pointer,
		void (*p_node_parse)(void *p_user_pointer, NetUtility::NodeData *p_node_data),
		void (*p_input_id_parse)(void *p_user_pointer, uint32_t p_input_id),
		void (*p_controller_parse)(void *p_user_pointer, NetUtility::NodeData *p_node_data),
		void (*p_variable_parse)(void *p_user_pointer, NetUtility::NodeData *p_node_data, uint32_t p_var_id, const Variant &p_value)) {
	// The sync data is an array that contains the scene informations.
	// It's used for several things, for this reason this function allows to
	// customize the parsing.
	//
	// The data is composed as follows:
	//  [TRUE/FALSE, InputID,
	//	NODE, VARIABLE, Value, VARIABLE, Value, VARIABLE, value, NIL,
	//  NODE, INPUT ID, VARIABLE, Value, VARIABLE, Value, NIL,
	//  NODE, VARIABLE, Value, VARIABLE, Value, NIL]
	//
	// Each node ends with a NIL, and the NODE and the VARIABLE are special:
	// - InputID: The first parameter is a boolean; when is true the following input is the `InputID`.
	// - NODE, can be an array of two variables [Node ID, NodePath] or directly
	//         a Node ID. Obviously the array is sent only the first time.
	// - INPUT ID, this is optional and is used only when the node is a controller.
	// - VARIABLE, can be an array with the ID and the variable name, or just
	//              the ID; similarly as is for the NODE the array is send only
	//              the first time.

	if (p_sync_data.get_type() == Variant::NIL) {
		// Nothing to do.
		return true;
	}

	ERR_FAIL_COND_V(!p_sync_data.is_array(), false);

	const Vector<Variant> raw_snapshot = p_sync_data;
	const Variant *raw_snapshot_ptr = raw_snapshot.ptr();

	int snap_data_index = 0;

	// Fetch the `InputID`.
	ERR_FAIL_COND_V_MSG(raw_snapshot.size() < 1, false, "This snapshot is corrupted as it doesn't even contains the first parameter used to specify the `InputID`.");
	ERR_FAIL_COND_V_MSG(raw_snapshot[0].get_type() != Variant::BOOL, false, "This snapshot is corrupted as the first parameter is not a boolean.");
	snap_data_index += 1;
	if (raw_snapshot[0].operator bool()) {
		// The InputId is set.
		ERR_FAIL_COND_V_MSG(raw_snapshot.size() < 2, false, "This snapshot is corrupted as the second parameter containing the `InputID` is not set.");
		ERR_FAIL_COND_V_MSG(raw_snapshot[1].get_type() != Variant::INT, false, "This snapshot is corrupted as the second parameter containing the `InputID` is not an INTEGER.");
		const uint32_t input_id = raw_snapshot[1];
		p_input_id_parse(p_user_pointer, input_id);
		snap_data_index += 1;
	} else {
		p_input_id_parse(p_user_pointer, UINT32_MAX);
	}

	NetUtility::NodeData *synchronizer_node_data = nullptr;
	uint32_t var_id = UINT32_MAX;

	for (; snap_data_index < raw_snapshot.size(); snap_data_index += 1) {
		const Variant v = raw_snapshot_ptr[snap_data_index];
		if (synchronizer_node_data == nullptr) {
			// Node is null so we expect `v` has the node info.

			bool skip_this_node = false;
			Node *node = nullptr;
			uint32_t net_node_id = UINT32_MAX;
			NodePath node_path;

			if (v.is_array()) {
				// Node info are in verbose form, extract it.

				const Vector<Variant> node_data = v;
				ERR_FAIL_COND_V(node_data.size() != 2, false);
				ERR_FAIL_COND_V_MSG(node_data[0].get_type() != Variant::INT, false, "This snapshot is corrupted.");
				ERR_FAIL_COND_V_MSG(node_data[1].get_type() != Variant::NODE_PATH, false, "This snapshot is corrupted.");

				net_node_id = node_data[0];
				node_path = node_data[1];

				// Associate the ID with the path.
				node_paths.set(net_node_id, node_path);

			} else if (v.get_type() == Variant::INT) {
				// Node info are in short form.
				net_node_id = v;
				NetUtility::NodeData *nd = scene_synchronizer->get_node_data(net_node_id);
				if (nd != nullptr) {
					synchronizer_node_data = nd;
					goto node_lookup_out;
				}
			} else {
				// The arrived snapshot does't seems to be in the expected form.
				ERR_FAIL_V_MSG(false, "This snapshot is corrupted. Now the node is expected, " + String(v) + " was submitted instead.");
			}

			if (synchronizer_node_data == nullptr) {
				if (node_path.is_empty()) {
					const NodePath *node_path_ptr = node_paths.lookup_ptr(net_node_id);

					if (node_path_ptr == nullptr) {
						// Was not possible lookup the node_path.
						SceneSynchronizerDebugger::singleton()->debug_warning(scene_synchronizer, "The node with ID `" + itos(net_node_id) + "` is not know by this peer, this is not supposed to happen.");
						notify_server_full_snapshot_is_needed();
						skip_this_node = true;
						goto node_lookup_check;
					} else {
						node_path = *node_path_ptr;
					}
				}

				node = scene_synchronizer->get_tree()->get_root()->get_node_or_null(node_path);
				if (node == nullptr) {
					// The node doesn't exists.
					SceneSynchronizerDebugger::singleton()->debug_warning(scene_synchronizer, "The node " + node_path + " still doesn't exist.");
					skip_this_node = true;
					goto node_lookup_check;
				}

				// Register this node, so to make sure the client is tracking it.
				NetUtility::NodeData *nd = scene_synchronizer->register_node(node);
				if (nd != nullptr) {
					// Set the node ID.
					scene_synchronizer->set_node_data_id(nd, net_node_id);
					synchronizer_node_data = nd;
				} else {
					SceneSynchronizerDebugger::singleton()->debug_error(scene_synchronizer, "[BUG] This node " + node->get_path() + " was not know on this client. Though, was not possible to register it.");
					skip_this_node = true;
				}
			}

		node_lookup_check:
			if (skip_this_node || synchronizer_node_data == nullptr) {
				synchronizer_node_data = nullptr;

				// This node does't exist; skip it entirely.
				for (snap_data_index += 1; snap_data_index < raw_snapshot.size(); snap_data_index += 1) {
					if (raw_snapshot_ptr[snap_data_index].get_type() == Variant::NIL) {
						break;
					}
				}

				if (!skip_this_node) {
					SceneSynchronizerDebugger::singleton()->debug_warning(scene_synchronizer, "This NetNodeId " + itos(net_node_id) + " doesn't exist on this client.");
				}
				continue;
			}

		node_lookup_out:

#ifdef DEBUG_ENABLED
			// At this point the ID is never UINT32_MAX thanks to the above
			// mechanism.
			CRASH_COND(synchronizer_node_data->id == UINT32_MAX);
#endif

			p_node_parse(p_user_pointer, synchronizer_node_data);

			if (synchronizer_node_data->is_controller) {
				if (synchronizer_node_data == player_controller_node_data) {
					// The current controller.
					p_controller_parse(p_user_pointer, synchronizer_node_data);
				} else {
					// This is just a remoote controller
					p_controller_parse(p_user_pointer, synchronizer_node_data);
				}
			}

		} else if (var_id == UINT32_MAX) {
			// When the node is known and the `var_id` not, we expect a
			// new variable or the end pf this node data.

			if (v.get_type() == Variant::NIL) {
				// NIL found, so this node is done.
				synchronizer_node_data = nullptr;
				continue;
			}

			// This is a new variable, so let's take the variable name.

			if (v.is_array()) {
				// The variable info are stored in verbose mode.

				const Vector<Variant> var_data = v;
				ERR_FAIL_COND_V(var_data.size() != 2, false);
				ERR_FAIL_COND_V(var_data[0].get_type() != Variant::INT, false);
				ERR_FAIL_COND_V(var_data[1].get_type() != Variant::STRING_NAME, false);

				var_id = var_data[0];
				StringName variable_name = var_data[1];

				{
					int64_t index = synchronizer_node_data->vars.find(variable_name);
					if (index == -1) {
						// The variable is not known locally, so just add it so
						// to store the variable ID.
						index = synchronizer_node_data->vars.size();

						const bool skip_rewinding = false;
						const bool enabled = false;
						synchronizer_node_data->vars
								.push_back(
										NetUtility::VarData(
												var_id,
												variable_name,
												Variant(),
												skip_rewinding,
												enabled));
						SceneSynchronizerDebugger::singleton()->debug_error(scene_synchronizer, "The variable " + variable_name + " for the node " + synchronizer_node_data->node->get_path() + " was not known on this client. This should never happen, make sure to register the same nodes on the client and server.");
					}

					if (index != var_id) {
						if (synchronizer_node_data[var_id].id != UINT32_MAX) {
							// It's not expected because if index is different to
							// var_id, var_id should have a not yet initialized
							// variable.
							SceneSynchronizerDebugger::singleton()->debug_error(scene_synchronizer, "This snapshot is corrupted. The var_id, at this point, must have a not yet init variable.");
							notify_server_full_snapshot_is_needed();
							return false;
						}

						// Make sure the variable is at the right index.
						SWAP(synchronizer_node_data->vars[index], synchronizer_node_data->vars[var_id]);
					}
				}

				// Make sure the ID is properly assigned.
				synchronizer_node_data->vars[var_id].id = var_id;

			} else if (v.get_type() == Variant::INT) {
				// The variable is stored in the compact form.

				var_id = v;

				if (var_id >= synchronizer_node_data->vars.size() ||
						synchronizer_node_data->vars[var_id].id == UINT32_MAX) {
					SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, "The var with ID `" + itos(var_id) + "` is not know by this peer, this is not supposed to happen.");

					notify_server_full_snapshot_is_needed();

					// Skip the next data since it's the value of this variable.
					snap_data_index += 1;
					var_id = UINT32_MAX;
					continue;
				}

			} else {
				ERR_FAIL_V_MSG(false, "The snapshot received seems corrupted. The variable is expected but " + String(v) + " received instead.");
			}

		} else {
			// The node is known, also the variable name is known, so the value
			// is expected.

			p_variable_parse(
					p_user_pointer,
					synchronizer_node_data,
					var_id,
					v);

			if (synchronizer_node_data->controlled_by == player_controller_node_data && ProjectSettings::get_singleton()->get_setting("NetworkSynchronizer/display_server_ghost")) {
				// This is the main controller, call func to access the server values
				NetworkedController *controller = static_cast<NetworkedController *>(player_controller_node_data->node);
				controller->call(
						SNAME("_receive_server_value"),
						synchronizer_node_data->vars[var_id].var.name,
						v.duplicate(true));
			}

			// Just reset the variable name so we can continue iterate.
			var_id = UINT32_MAX;
		}
	}

	return true;
}

void ClientSynchronizer::set_enabled(bool p_enabled) {
	if (enabled == p_enabled) {
		// Nothing to do.
		return;
	}

	if (p_enabled) {
		// Postpone enabling when the next server snapshot is received.
		want_to_enable = true;
	} else {
		// Disabling happens immediately.
		enabled = false;
		want_to_enable = false;
		scene_synchronizer->emit_signal("sync_paused");
	}
}

bool ClientSynchronizer::parse_snapshot(Variant p_snapshot) {
	if (want_to_enable) {
		if (enabled) {
			SceneSynchronizerDebugger::singleton()->debug_error(scene_synchronizer, "At this point the client is supposed to be disabled. This is a bug that must be solved.");
		}
		// The netwroking is disabled and we can re-enable it.
		enabled = true;
		want_to_enable = false;
		scene_synchronizer->emit_signal("sync_started");
	}

	need_full_snapshot_notified = false;

	NetUtility::Snapshot received_snapshot = last_received_snapshot;
	received_snapshot.input_id = UINT32_MAX;

	struct ParseData {
		NetUtility::Snapshot &snapshot;
		NetUtility::NodeData *player_controller_node_data;
	};

	ParseData parse_data{
		received_snapshot,
		player_controller_node_data
	};

	const bool success = parse_sync_data(
			p_snapshot,
			&parse_data,

			// Parse node:
			[](void *p_user_pointer, NetUtility::NodeData *p_node_data) {
				ParseData *pd = static_cast<ParseData *>(p_user_pointer);

				// Make sure this node is part of the server node too.
				if (uint32_t(pd->snapshot.node_vars.size()) <= p_node_data->id) {
					pd->snapshot.node_vars.resize(p_node_data->id + 1);
				}

				if (p_node_data->vars.size() != uint32_t(pd->snapshot.node_vars[p_node_data->id].size())) {
					// This mean the parser just added a new variable.
					// Already notified by the parser.
					pd->snapshot.node_vars.write[p_node_data->id].resize(p_node_data->vars.size());
				}
			},

			// Parse InputID:
			[](void *p_user_pointer, uint32_t p_input_id) {
				ParseData *pd = static_cast<ParseData *>(p_user_pointer);
				if (pd->player_controller_node_data != nullptr) {
					// This is the main controller, store the `InputID`.
					pd->snapshot.input_id = p_input_id;
				}
			},

			// Parse controller:
			[](void *p_user_pointer, NetUtility::NodeData *p_node_data) {},

			// Parse variable:
			[](void *p_user_pointer, NetUtility::NodeData *p_node_data, uint32_t p_var_id, const Variant &p_value) {
				ParseData *pd = static_cast<ParseData *>(p_user_pointer);

#ifdef DEBUG_ENABLED
				// This can't be triggered because the `Parse Node` function
				// above make sure to create room for this array.
				CRASH_COND(uint32_t(pd->snapshot.node_vars.size()) <= p_node_data->id);
				CRASH_COND(uint32_t(pd->snapshot.node_vars[p_node_data->id].size()) != p_node_data->vars.size());
#endif // ~DEBUG_ENABLED

				pd->snapshot.node_vars.write[p_node_data->id].write[p_var_id].name = p_node_data->vars[p_var_id].var.name;
				pd->snapshot.node_vars.write[p_node_data->id].write[p_var_id].value = p_value.duplicate(true);
			});

	if (success == false) {
		SceneSynchronizerDebugger::singleton()->debug_error(scene_synchronizer, "Snapshot:");
		SceneSynchronizerDebugger::singleton()->debug_error(scene_synchronizer, p_snapshot);
		return false;
	}

	if (unlikely(received_snapshot.input_id == UINT32_MAX && player_controller_node_data != nullptr)) {
		// We espect that the player_controller is updated by this new snapshot,
		// so make sure it's done so.
		SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, "[INFO] the player controller (" + player_controller_node_data->node->get_path() + ") was not part of the received snapshot, this happens when the server destroys the peer controller. NetUtility::Snapshot:");
		SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, p_snapshot);
	}

	last_received_snapshot = received_snapshot;

	// Success.
	return true;
}

bool ClientSynchronizer::compare_vars(
		const NetUtility::NodeData *p_synchronizer_node_data,
		const Vector<NetUtility::Var> &p_server_vars,
		const Vector<NetUtility::Var> &p_client_vars,
		Vector<NetUtility::Var> &r_postponed_recover) {
	const NetUtility::Var *s_vars = p_server_vars.ptr();
	const NetUtility::Var *c_vars = p_client_vars.ptr();

#ifdef DEBUG_ENABLED
	bool diff = false;
#endif

	for (uint32_t var_index = 0; var_index < uint32_t(p_client_vars.size()); var_index += 1) {
		if (uint32_t(p_server_vars.size()) <= var_index) {
			// This variable isn't defined into the server snapshot, so assuming it's correct.
			continue;
		}

		if (s_vars[var_index].name == StringName()) {
			// This variable was not set, skip the check.
			continue;
		}

		// Compare.
		const bool different =
				// Make sure this variable is set.
				c_vars[var_index].name == StringName() ||
				// Check if the value is different.
				!scene_synchronizer->compare(
						s_vars[var_index].value,
						c_vars[var_index].value);

		if (different) {
			if (p_synchronizer_node_data->vars[var_index].skip_rewinding) {
				// The vars are different, but this variable don't what to
				// trigger a rewind.
				r_postponed_recover.push_back(s_vars[var_index]);
			} else {
				// The vars are different.
				SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, "Difference found on var #" + itos(var_index) + " " + p_synchronizer_node_data->vars[var_index].var.name + " " + "Server value: `" + s_vars[var_index].value + "` " + "Client value: `" + c_vars[var_index].value + "`.    " + "[Server name: `" + s_vars[var_index].name + "` " + "Client name: `" + c_vars[var_index].name + "`].");
#ifdef DEBUG_ENABLED
				diff = true;
#else
				return true;
#endif
			}
		}
	}

#ifdef DEBUG_ENABLED
	return diff;
#else

	// The vars are not different.
	return false;
#endif
}

void ClientSynchronizer::notify_server_full_snapshot_is_needed() {
	if (need_full_snapshot_notified) {
		return;
	}

	// Notify the server that a full snapshot is needed.
	need_full_snapshot_notified = true;
	scene_synchronizer->rpc_id(1, SNAME("_rpc_notify_need_full_snapshot"));
}

void ClientSynchronizer::send_actions_to_server() {
	const uint64_t now = OS::get_singleton()->get_ticks_msec();

	LocalVector<SenderNetAction *> packet_actions;

	// The packet will contains the most recent actions.
	for (uint32_t i = 0; i < pending_actions.size(); i += 1) {
		if (pending_actions[i].sent_by_the_server) {
			// This is a remote Action. Do not network it.
			continue;
		}

		if (int(pending_actions[i].send_count) >= scene_synchronizer->get_actions_redundancy()) {
			// Nothing to do.
			continue;
		}

		if ((pending_actions[i].send_timestamp + 2 /*ms - give some room*/) > now) {
			// Nothing to do.
			continue;
		}

		pending_actions[i].send_timestamp = now;
		pending_actions[i].send_count += 1;
		packet_actions.push_back(&pending_actions[i]);
	}

	if (packet_actions.size() <= 0) {
		// Nothing to send.
		return;
	}

	// Encode the actions.
	DataBuffer db;
	db.begin_write(0);
	net_action::encode_net_action(packet_actions, 1, db);
	db.dry();

	// Send to the server.
	const int server_peer_id = 1;
	scene_synchronizer->rpc_unreliable_id(
			server_peer_id,
			"_rpc_send_actions",
			db.get_buffer().get_bytes());
}

void ClientSynchronizer::clean_pending_actions() {
	// The packet will contains the most recent actions.
	for (int64_t i = int64_t(pending_actions.size()) - 1; i >= 0; i -= 1) {
		if (
				pending_actions[i].locally_executed == false ||
				(pending_actions[i].sent_by_the_server == false && int(pending_actions[i].send_count) < scene_synchronizer->get_actions_redundancy())) {
			// Still somethin to do.
			continue;
		}

		pending_actions.remove_unordered(i);
	}
}

void ClientSynchronizer::check_missing_actions() {
	server_sender_info.check_missing_actions_and_clean_up(scene_synchronizer);
}
