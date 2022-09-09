#include <pushplan/search/mamo_search.hpp>
#include <pushplan/search/planner.hpp>
#include <pushplan/utils/helpers.hpp>

#include <limits>
#include <stdexcept>

namespace clutter
{

bool MAMOSearch::CreateRoot()
{
	///////////
	// Stats //
	///////////

	m_stats["expansions"] = 0.0;
	m_stats["no_succs"] = 0.0;
	m_stats["only_duplicate"] = 0.0;
	m_stats["no_duplicate"] = 0.0;
	m_stats["mapf_time"] = 0.0;
	m_stats["push_planner_time"] = 0.0;
	m_stats["total_time"] = 0.0;

	double t1 = GetTime();
	m_root_node = new MAMONode;
	m_root_node->SetPlanner(m_planner);
	m_root_node->SetCBS(m_planner->GetCBS());
	m_root_node->SetCC(m_planner->GetCC());
	m_root_node->SetRobot(m_planner->GetRobot());

	m_root_node->InitAgents(m_planner->GetAllAgents(), m_planner->GetStartObjects()); // inits required fields for hashing
	m_root_node->SetEdgeTo(-1, -1);

	// 1. run MAPF
	double t2 = GetTime();
	bool mapf_solved = m_root_node->RunMAPF();
	m_stats["mapf_time"] += GetTime() - t2;
	if (!mapf_solved) {
		return false;
	}

	m_root_id = m_hashtable.GetStateIDForceful(m_root_node);
	m_root_search = getSearchState(m_root_id);
	m_search_nodes.push_back(m_root_node);

	m_root_node->SaveNode(m_root_id, 0);
	m_stats["total_time"] += GetTime() - t1;

	return true;
}

bool MAMOSearch::Solve()
{
	double t1 = GetTime();
	m_root_search->f = 0;
	m_root_search->depth = 0;
	m_root_search->actions = 0;
	m_root_search->noops = 0;
	m_root_search->m_OPEN_h = m_OPEN.push(m_root_search);

	while (!m_OPEN.empty())
	{
		if (GetTime() - t1 > 300.0)
		{
			SMPL_ERROR("MAMO Search took more than 5 minutes!");
			break;
		}

		auto next = m_OPEN.top();
		SMPL_WARN("Select %d, f-value = %u", next->state_id, next->f);
		if (done(next))
		{
			SMPL_INFO("Final plan found!");

			auto node = m_hashtable.GetState(next->state_id);
			auto parent_id = next->bp == nullptr ? 0 : next->bp->state_id;
			double t2 = GetTime();
			node->RunMAPF();
			m_stats["mapf_time"] += GetTime() - t2;

			m_solved_node = node;
			m_solved_search = next;
			m_solved = true;
			extractRearrangements();
			m_stats["total_time"] += GetTime() - t1;

			return true;
		}
		expand(next);
		m_stats["expansions"] += 1;
	}
	m_stats["total_time"] += GetTime() - t1;
	return false;
}

void MAMOSearch::GetRearrangements(std::vector<trajectory_msgs::JointTrajectory>& rearrangements, int& grasp_at)
{
	rearrangements.clear();
	rearrangements = m_rearrangements;
	grasp_at = m_grasp_at;
}

void MAMOSearch::SaveStats()
{
	std::string filename(__FILE__);
	auto found = filename.find_last_of("/\\");
	filename = filename.substr(0, found + 1) + "../../dat/MAMO.csv";

	bool exists = FileExists(filename);
	std::ofstream STATS;
	STATS.open(filename, std::ofstream::out | std::ofstream::app);
	if (!exists)
	{
		STATS << "UID,"
				<< "Solved?,SolveTime,MAPFTime,PushPlannerTime,"
				<< "Expansions,OnlyDuplicate,NoDuplicate\n";
	}

	STATS << m_planner->GetSceneID() << ','
			<< (int)m_solved << ','
			<< m_stats["total_time"] << ',' << m_stats["mapf_time"] << ',' << m_stats["push_planner_time"] << ','
			<< m_stats["expansions"] << ',' << m_stats["only_duplicate"] << ',' << m_stats["no_duplicate"] << '\n';
	STATS.close();
}

bool MAMOSearch::expand(MAMOSearchState *state)
{
	SMPL_WARN("Expand %d, f-value = %u", state->state_id, state->f);
	auto node = m_hashtable.GetState(state->state_id);

	// 2. generate appropriate successor states
	std::vector<std::pair<int, int> > succ_object_centric_actions;
	std::vector<comms::ObjectsPoses> succ_objects;
	std::vector<trajectory_msgs::JointTrajectory> succ_trajs;
	std::vector<std::tuple<State, State, int> > debug_pushes;
	bool close;
	double t = GetTime();
	node->GetSuccs(&succ_object_centric_actions, &succ_objects, &succ_trajs, &debug_pushes, &close);
	m_stats["push_planner_time"] += GetTime() - t;

	assert(succ_object_centric_actions.size() == succ_objects.size());
	assert(succ_objects.size() == succ_trajs.size());
	assert(succ_trajs.size() == succ_object_centric_actions.size());

	if (close)
	{
		state->closed = true;
		m_OPEN.erase(state->m_OPEN_h);
	}

	// 3. add successor states to search graph
	createSuccs(node, state, &succ_object_centric_actions, &succ_objects, &succ_trajs, &debug_pushes);

	return true;
}

bool MAMOSearch::done(MAMOSearchState *state)
{
	auto node = m_hashtable.GetState(state->state_id);
	if (node->kparent() != nullptr && node->object_centric_action() == std::make_pair(-1, -1)) {
		return false;
	}

	bool mapf_done = true;
	const auto& mapf_soln = node->kmapf_soln();
	for (size_t i = 0; i < mapf_soln.size(); ++i)
	{
		const auto& moved = mapf_soln.at(i);
		if (moved.second.size() == 1 || moved.second.front().coord == moved.second.back().coord) {
			continue;
		}
		mapf_done = false;
		break;
	}

	if (mapf_done) {
		return true;
	}

	auto start_state = node->GetCurrentStartState();
	return m_planner->FinalisePlan(node->kobject_states(), start_state, m_exec_traj);
}

void MAMOSearch::extractRearrangements()
{
	m_rearrangements.clear();
	m_rearrangements.push_back(std::move(m_exec_traj));
	ROS_WARN("Solution path state ids (from goal to start):");
	for (MAMOSearchState *state = m_solved_search; state; state = state->bp)
	{
		auto node = m_hashtable.GetState(state->state_id);
		if (node->has_traj())
		{
			ROS_WARN("\t%d", state->state_id);
			m_rearrangements.push_back(node->krobot_traj());
		}
	}
	std::reverse(m_rearrangements.begin(), m_rearrangements.end());
	m_grasp_at = m_planner->GetRobot()->GraspAt();
}

void MAMOSearch::createSuccs(
	MAMONode *parent_node,
	MAMOSearchState *parent_search_state,
	std::vector<std::pair<int, int> > *succ_object_centric_actions,
	std::vector<comms::ObjectsPoses> *succ_objects,
	std::vector<trajectory_msgs::JointTrajectory> *succ_trajs,
	std::vector<std::tuple<State, State, int> > *debug_pushes)
{
	size_t num_succs = succ_object_centric_actions->size();
	bool duplicate_successor = succ_object_centric_actions->back() == std::make_pair(-1, -1);

	// if (num_succs == 0)
	// {
	// 	m_stats["no_succs"] += 1;
	// 	parent_search_state->closed = true;
	// 	m_OPEN.erase(parent_search_state->m_OPEN_h);
	// 	return;
	// }
	if (duplicate_successor && num_succs == 1)
	{
		m_stats["only_duplicate"] += 1;
		// parent_search_state->closed = true;
		// m_OPEN.erase(parent_search_state->m_OPEN_h);
	}
	if (!duplicate_successor) {
		m_stats["no_duplicate"] += 1;
	}

	for (size_t i = 0; i < num_succs; ++i)
	{
		auto succ = new MAMONode;
		succ->SetPlanner(m_planner);
		succ->SetCBS(m_planner->GetCBS());
		succ->SetCC(m_planner->GetCC());
		succ->SetRobot(m_planner->GetRobot());

		succ->InitAgents(m_planner->GetAllAgents(), succ_objects->at(i)); // inits required fields for hashing
		succ->SetEdgeTo(succ_object_centric_actions->at(i).first, succ_object_centric_actions->at(i).second);

		// run MAPF
		double t = GetTime();
		bool mapf_solved = succ->RunMAPF();
		m_stats["mapf_time"] += GetTime() - t;
		if (!mapf_solved)
		{
			delete succ;
			continue;
		}

		// check if we have visited this mamo state before
		unsigned int old_id, old_f;
		MAMOSearchState *prev_search_state = nullptr;
		if (m_hashtable.Exists(succ))
		{
			old_id = m_hashtable.GetStateID(succ);
			prev_search_state = getSearchState(old_id);
			old_f = prev_search_state->f;
		}

		unsigned int succ_actions = parent_search_state->actions + (1 - (duplicate_successor && i == num_succs - 1));
		unsigned int succ_noops = parent_search_state->noops + (duplicate_successor && i == num_succs - 1);
		if (prev_search_state != nullptr && (prev_search_state->closed ||
				prev_search_state->actions <= succ_actions || prev_search_state->noops <= succ_noops))
		{
			// previously visited this mamo state with fewer actions, move on
			delete succ;
			continue;
		}
		else
		{
			succ->SetRobotTrajectory(succ_trajs->at(i));
			succ->SetParent(parent_node);
			if (duplicate_successor && i == num_succs - 1)
			{
				for (auto it = debug_pushes->begin() + i; it != debug_pushes->end(); ++it) {
					succ->AddDebugPush(*it);
				}
			}
			else {
				succ->AddDebugPush(debug_pushes->at(i));
			}

			if (prev_search_state != nullptr)
			{
				// update search state in open list
				auto old_succ = m_hashtable.GetState(old_id);
				old_succ->parent()->RemoveChild(old_succ);

				m_hashtable.UpdateState(succ);
				prev_search_state->bp = parent_search_state;
				prev_search_state->depth = parent_search_state->depth + 1;
				prev_search_state->actions = succ_actions;
				prev_search_state->noops = succ_noops;
				m_OPEN.update(prev_search_state->m_OPEN_h);
				SMPL_WARN("Update %d, new f-value = %u (previously %u)", old_id, old_f, prev_search_state->f);
			}
			else
			{
				// add search state to open list
				unsigned int succ_id = m_hashtable.GetStateIDForceful(succ);
				auto succ_search_state = getSearchState(succ_id);
				succ_search_state->bp = parent_search_state;
				succ_search_state->f = succ->ComputeMAMOPriority();
				succ_search_state->depth = parent_search_state->depth + 1;
				succ_search_state->actions = succ_actions;
				succ_search_state->noops = succ_noops;
				succ_search_state->m_OPEN_h = m_OPEN.push(succ_search_state);

				SMPL_WARN("Generate %d, f-value = %u", succ_id, succ_search_state->f);
				succ->SaveNode(succ_id, parent_search_state->state_id);
			}
			parent_node->AddChild(succ);
			m_search_nodes.push_back(succ);
		}
	}
}

MAMOSearchState* MAMOSearch::getSearchState(unsigned int state_id)
{
	if (m_search_states.size() <= state_id)	{
		m_search_states.resize(state_id + 1, nullptr);
	}

	auto& state = m_search_states[state_id];
	if (state == nullptr) {
		state = createSearchState(state_id);
	}

	return state;
}

MAMOSearchState* MAMOSearch::createSearchState(unsigned int state_id)
{
	auto state = new MAMOSearchState;
	state->state_id = state_id;
	initSearchState(state);
}

void MAMOSearch::initSearchState(MAMOSearchState *state)
{
	state->f = std::numeric_limits<unsigned int>::max();
	state->depth = -1;
	state->actions = -1;
	state->noops = -1;
	state->closed = false;
	state->bp = nullptr;
}

} // namespace clutter
