/*************************************************************************/
/*  no_net_synchronizer.h                                                */
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

#ifndef NO_NET_SYNCHRONIZER_H
#define NO_NET_SYNCHRONIZER_H

#include "core/local_vector.h"
#include "core/oa_hash_map.h"
#include "net_action.h"
#include "net_utilities.h"
#include "scene/main/node.h"
#include "scene_synchronizer.h"
#include <deque>

#include "godot_backward_utility_header.h"

class Synchronizer;
class NetworkedController;
struct PlayerController;

class NoNetSynchronizer : public Synchronizer {
	friend class SceneSynchronizer;

	bool enabled = true;
	uint32_t frame_count = 0;
	LocalVector<NetActionProcessor> pending_actions;

public:
	NoNetSynchronizer(SceneSynchronizer *p_node);

	virtual void clear() override;
	virtual void process() override;
	virtual void on_action_triggered(
			NetUtility::NodeData *p_node_data,
			NetActionId p_id,
			const Array &p_arguments,
			const Vector<int> &p_recipients) override;

	void set_enabled(bool p_enabled);
	bool is_enabled() const;
};

#endif // NO_NET_SYNCHRONIZER_H
