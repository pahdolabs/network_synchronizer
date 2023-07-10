/*************************************************************************/
/*  scene_synchronizer.h                                                 */
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

#ifndef SERVER_SYNCHRONIZER_H
#define SERVER_SYNCHRONIZER_H

#include <deque>

#include "core/local_vector.h"
#include "core/oa_hash_map.h"
#include "net_action.h"
#include "net_utilities.h"
#include "scene/main/node.h"
#include "scene_synchronizer.h"

#include "godot_backward_utility_header.h"

class Synchronizer;
class NetworkedController;
struct PlayerController;

class ServerSynchronizer : public Synchronizer {
	friend class SceneSynchronizer;

	real_t state_notifier_timer = 0.0;

	struct Change {
		bool not_known_before = false;
		Set<StringName> uknown_vars;
		Set<StringName> vars;
	};

	enum SnapshotGenerationMode {
		/// The shanpshot will include The NodeId or NodePath and allthe changed variables.
		SNAPSHOT_GENERATION_MODE_NORMAL,
		/// The snapshot will include The NodePath only in case it was unknown before.
		SNAPSHOT_GENERATION_MODE_NODE_PATH_ONLY,
		/// The snapshot will include The NodePath only.
		SNAPSHOT_GENERATION_MODE_FORCE_NODE_PATH_ONLY,
		/// The snapshot will contains everything no matter what.
		SNAPSHOT_GENERATION_MODE_FORCE_FULL,
	};

	/// The changes; the order matters because the index is the NetNodeId.
	LocalVector<Change> changes;

	OAHashMap<int, NetActionSenderInfo> senders_info;
	OAHashMap<int, uint32_t> peers_next_action_trigger_input_id;

	uint32_t server_actions_count = 0;
	LocalVector<SenderNetAction> server_actions;

public:
	ServerSynchronizer(SceneSynchronizer *p_node);

	virtual void clear() override;
	virtual void process() override;
	virtual void on_node_added(NetUtility::NodeData *p_node_data) override;
	virtual void on_node_removed(NetUtility::NodeData *p_node_data) override;
	virtual void on_variable_added(NetUtility::NodeData *p_node_data, const StringName &p_var_name) override;
	virtual void on_variable_changed(NetUtility::NodeData *p_node_data, NetVarId p_var_id, const Variant &p_old_value, int p_flag) override;
	virtual void on_action_triggered(
			NetUtility::NodeData *p_node_data,
			NetActionId p_id,
			const Array &p_arguments,
			const Vector<int> &p_recipients) override;
	virtual void on_actions_received(
			int sender_peer,
			const LocalVector<SenderNetAction> &p_actions) override;

	void process_snapshot_notificator(real_t p_delta);
	Vector<Variant> global_nodes_generate_snapshot(bool p_force_full_snapshot) const;
	void controller_generate_snapshot(const NetUtility::NodeData *p_node_data, bool p_force_full_snapshot, Vector<Variant> &r_snapshot_result) const;
	void generate_snapshot_node_data(const NetUtility::NodeData *p_node_data, SnapshotGenerationMode p_mode, Vector<Variant> &r_result) const;

	void execute_actions();
	void send_actions_to_clients();

	void clean_pending_actions();
	void check_missing_actions();
};

#endif // SERVER_SYNCHRONIZER_H