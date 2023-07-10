/*************************************************************************/
/*  client_synchronizer.h                                                */
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

#ifndef CLIENT_SYNCHRONIZER_H
#define CLIENT_SYNCHRONIZER_H

#include "core/local_vector.h"
#include "core/oa_hash_map.h"
#include "net_action.h"
#include "net_utilities.h"
#include "scene/main/node.h"
#include "scene_synchronizer.h"
#include <deque>

#include "godot_backward_utility_header.h"

class NetworkedController;
struct PlayerController;

class ClientSynchronizer : public Synchronizer {
	friend class SceneSynchronizer;

	NetUtility::NodeData *player_controller_node_data = nullptr;
	OAHashMap<NetNodeId, NodePath> node_paths;

	NetUtility::Snapshot last_received_snapshot;
	std::deque<NetUtility::Snapshot> client_snapshots;
	std::deque<NetUtility::Snapshot> server_snapshots;
	uint32_t last_checked_input = 0;
	bool enabled = true;
	bool want_to_enable = false;

	bool need_full_snapshot_notified = false;

	struct EndSyncEvent {
		NetUtility::NodeData *node_data;
		NetVarId var_id;
		Variant old_value;

		bool operator<(const EndSyncEvent &p_other) const {
			if (node_data->id == p_other.node_data->id) {
				return var_id < p_other.var_id;
			} else {
				return node_data->id < p_other.node_data->id;
			}
		}
	};

	Set<EndSyncEvent> sync_end_events;

	uint32_t locally_triggered_actions_count = 0;
	uint32_t actions_input_id = 0;
	LocalVector<SenderNetAction> pending_actions;
	NetActionSenderInfo server_sender_info;

public:
	ClientSynchronizer(SceneSynchronizer *p_node);

	virtual void clear() override;

	virtual void process() override;
	virtual void on_node_added(NetUtility::NodeData *p_node_data) override;
	virtual void on_node_removed(NetUtility::NodeData *p_node_data) override;
	virtual void on_variable_changed(NetUtility::NodeData *p_node_data, NetVarId p_var_id, const Variant &p_old_value, int p_flag) override;
	virtual void on_controller_reset(NetUtility::NodeData *p_node_data) override;
	virtual void on_action_triggered(
			NetUtility::NodeData *p_node_data,
			NetActionId p_id,
			const Array &p_arguments,
			const Vector<int> &p_recipients) override;
	virtual void on_actions_received(
			int sender_peer,
			const LocalVector<SenderNetAction> &p_actions) override;

	void receive_snapshot(Variant p_snapshot);
	bool parse_sync_data(
			Variant p_snapshot,
			void *p_user_pointer,
			void (*p_node_parse)(void *p_user_pointer, NetUtility::NodeData *p_node_data),
			void (*p_input_id_parse)(void *p_user_pointer, uint32_t p_input_id),
			void (*p_controller_parse)(void *p_user_pointer, NetUtility::NodeData *p_node_data),
			void (*p_variable_parse)(void *p_user_pointer, NetUtility::NodeData *p_node_data, NetVarId p_var_id, const Variant &p_value));

	void set_enabled(bool p_enabled);

private:
	/// Store node data organized per controller.
	void store_snapshot();

	void store_controllers_snapshot(
			const NetUtility::Snapshot &p_snapshot,
			std::deque<NetUtility::Snapshot> &r_snapshot_storage);

	void process_controllers_recovery(real_t p_delta);

	void __pcr__fetch_recovery_info(
			const uint32_t p_input_id,
			bool &r_need_recover,
			bool &r_recover_controller,
			LocalVector<NetUtility::NodeData *> &r_nodes_to_recover,
			LocalVector<NetUtility::PostponedRecover> &r_postponed_recover);

	void __pcr__sync_pre_rewind(
			const LocalVector<NetUtility::NodeData *> &p_nodes_to_recover);

	void __pcr__rewind(
			real_t p_delta,
			const uint32_t p_checkable_input_id,
			NetworkedController *p_controller,
			PlayerController *p_player_controller,
			const bool p_recover_controller,
			const LocalVector<NetUtility::NodeData *> &p_nodes_to_recover);

	void __pcr__sync_no_rewind(
			const LocalVector<NetUtility::PostponedRecover> &p_postponed_recover);

	void apply_last_received_server_snapshot();
	void process_paused_controller_recovery(real_t p_delta);
	bool parse_snapshot(Variant p_snapshot);
	bool compare_vars(
			const NetUtility::NodeData *p_synchronizer_node_data,
			const Vector<NetUtility::Var> &p_server_vars,
			const Vector<NetUtility::Var> &p_client_vars,
			Vector<NetUtility::Var> &r_postponed_recover);

	void notify_server_full_snapshot_is_needed();

	void send_actions_to_server();
	void clean_pending_actions();
	void check_missing_actions();
};

#endif // CLIENT_SYNCHRONIZER_H
