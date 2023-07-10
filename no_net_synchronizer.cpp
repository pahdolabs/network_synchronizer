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

#include "no_net_synchronizer.h"

#include "core/method_bind_ext.gen.inc"
#include "core/os/os.h"
#include "input_network_encoder.h"
#include "networked_controller.h"
#include "scene_diff.h"
#include "scene_synchronizer.h"
#include "scene_synchronizer_debugger.h"

#include "godot_backward_utility_cpp.h"

NoNetSynchronizer::NoNetSynchronizer(SceneSynchronizer *p_node) :
		Synchronizer(p_node) {
	SceneSynchronizerDebugger::singleton()->setup_debugger("nonet", 0, scene_synchronizer->get_tree());
}

void NoNetSynchronizer::clear() {
	enabled = true;
	frame_count = 0;
}

void NoNetSynchronizer::process() {
	if (unlikely(enabled == false)) {
		return;
	}

	SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, "NoNetSynchronizer::process", true);

	const uint32_t frame_index = frame_count;
	frame_count += 1;

	SceneSynchronizerDebugger::singleton()->scene_sync_process_start(scene_synchronizer);

	const double physics_ticks_per_second = Engine::get_singleton()->get_iterations_per_second();
	const double delta = 1.0 / physics_ticks_per_second;

	// Process the scene
	for (uint32_t i = 0; i < scene_synchronizer->node_data.size(); i += 1) {
		NetUtility::NodeData *nd = scene_synchronizer->node_data[i];
		nd->process(delta);
	}

	// Process the controllers_node_data
	for (uint32_t i = 0; i < scene_synchronizer->node_data_controllers.size(); i += 1) {
		NetUtility::NodeData *nd = scene_synchronizer->node_data_controllers[i];
		static_cast<NetworkedController *>(nd->node)->get_nonet_controller()->process(delta);
	}

	// Execute the actions.
	for (uint32_t i = 0; i < pending_actions.size(); i += 1) {
		pending_actions[i].execute();
	}
	// No need to do anything else, just claen the acts.
	pending_actions.clear();

	// Pull the changes.
	scene_synchronizer->change_events_begin(NetEventFlag::CHANGE);
	for (uint32_t i = 0; i < scene_synchronizer->node_data.size(); i += 1) {
		NetUtility::NodeData *nd = scene_synchronizer->node_data[i];
		scene_synchronizer->pull_node_changes(nd);
	}
	scene_synchronizer->change_events_flush();

	SceneSynchronizerDebugger::singleton()->scene_sync_process_end(scene_synchronizer);
	SceneSynchronizerDebugger::singleton()->write_dump(0, frame_index);
	SceneSynchronizerDebugger::singleton()->start_new_frame();
}

void NoNetSynchronizer::on_action_triggered(
		NetUtility::NodeData *p_node_data,
		NetActionId p_id,
		const Array &p_arguments,
		const Vector<int> &p_recipients) {
	NetActionProcessor action_processor = NetActionProcessor(p_node_data, p_id, p_arguments);
	if (action_processor.server_validate()) {
		pending_actions.push_back(action_processor);
	} else {
		SceneSynchronizerDebugger::singleton()->debug_print(scene_synchronizer, "The `" + action_processor + "` action validation returned `false`. The action is discarded.");
	}
}

void NoNetSynchronizer::set_enabled(bool p_enabled) {
	if (enabled == p_enabled) {
		// Nothing to do.
		return;
	}

	enabled = p_enabled;

	if (enabled) {
		scene_synchronizer->emit_signal("sync_started");
	} else {
		scene_synchronizer->emit_signal("sync_paused");
	}
}

bool NoNetSynchronizer::is_enabled() const {
	return enabled;
}
