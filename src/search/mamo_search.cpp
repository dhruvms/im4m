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
	m_stats["generated"] = 0.0;
	m_stats["no_succs"] = 0.0;
	m_stats["only_duplicate"] = 0.0;
	m_stats["no_duplicate"] = 0.0;
	m_stats["mapf_time"] = 0.0;
	m_stats["push_planner_time"] = 0.0;
	m_stats["sim_time"] = 0.0;
	m_stats["finalise_time"] = 0.0;
	m_stats["total_time"] = 0.0;

	createDists();

	double t1 = GetTime();
	m_root_node = new MAMONode;
	m_root_node->SetPlanner(m_planner);
	m_root_node->SetCBS(m_planner->GetCBS());
	m_root_node->SetCC(m_planner->GetCC());
	m_root_node->SetRobot(m_planner->GetRobot());
	MAMOAction action(MAMOActionType::DUMMY, -1);
	action._params.clear();
	m_root_node->SetEdgeTo(action);

	std::vector<int> reachable_ids;
	// m_planner->GetRobot()->IdentifyReachableMovables(m_planner->GetAllAgents(), m_planner->GetStartObjects(), reachable_ids);
	m_root_node->InitAgents(m_planner->GetAllAgents(), m_planner->GetStartObjects(), reachable_ids); // inits required fields for hashing

	m_root_id = m_hashtable.GetStateIDForceful(m_root_node);
	m_root_search = getSearchState(m_root_id);
	m_search_nodes.push_back(m_root_node);
	m_stats["total_time"] += GetTime() - t1;

	std::stringstream reachable_str;
	std::copy(reachable_ids.begin(), reachable_ids.end(), std::ostream_iterator<int>(reachable_str, ","));
	SMPL_INFO("Movables reachable in state %d : (%s)", m_root_id, reachable_str.str().c_str());

	return true;
}

bool MAMOSearch::Solve(double budget)
{
	m_timer = GetTime();
	m_root_search->actions = 0;
	m_root_search->noops = 0;
	// m_root_search->priority = m_root_node->ComputeMAMOPriorityOrig();
	m_root_node->ComputePriorityFactors();
	computeMAMOPriority(m_root_search);
	m_root_search->m_OPEN_h = m_OPEN.push(m_root_search);
	++m_stats["generated"];

	while (!m_OPEN.empty())
	{
		if (GetTime() - m_timer > budget)
		{
			SMPL_ERROR("MAMO Search took more than %f seconds!", budget);
			break;
		}

		auto next = m_OPEN.top();
		SMPL_WARN("Select %d, priority = %.2e", next->state_id, next->priority);
		if (done(next))
		{
			SMPL_INFO("Final plan found OR NGR cleared!");

			auto node = m_hashtable.GetState(next->state_id);
			double t2 = GetTime();
			node->RunMAPF();
			m_stats["mapf_time"] += GetTime() - t2;
			node->SaveNode(next->state_id, next->bp == nullptr ? 0 : next->bp->state_id, "TERMINATED");

			m_solved_node = node;
			m_solved_search = next;
			m_solved = true;
			extractRearrangements();
			m_stats["total_time"] += GetTime() - m_timer;

			return true;
		}
		expand(next);
		m_stats["expansions"] += 1;
	}
	m_stats["total_time"] += GetTime() - m_timer;
	return false;
}

void MAMOSearch::GetRearrangements(
	std::vector<trajectory_msgs::JointTrajectory> &rearrangements,
	std::vector<MAMOAction> &actions)
{
	rearrangements.clear();
	rearrangements = m_rearrangements;

	actions.clear();
	actions = m_actions;
}

void MAMOSearch::SaveStats(int exec_success)
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
				<< "Solved?,ExecSuccess?,NumTrajs,SolveTime,MAPFTime,PushPlannerTime,SimTime,"
				<< "FinaliseTime,"
				<< "StatesGenerated,Expansions,OnlyDuplicate,NoDuplicate,NoSuccs\n";
	}

	STATS << m_planner->GetSceneID() << ','
			<< (int)m_solved << ',' << exec_success << ',' << (int)m_rearrangements.size() << ','
			<< m_stats["total_time"] << ',' << m_stats["mapf_time"] << ',' << m_stats["push_planner_time"] << ',' << m_stats["sim_time"] << ','
			<< m_stats["finalise_time"] << ','
			<< m_stats["generated"] << ',' << m_stats["expansions"] << ',' << m_stats["only_duplicate"] << ',' << m_stats["no_duplicate"] << ','
			<< m_stats["no_succs"] << '\n';
	STATS.close();
}

void MAMOSearch::SaveNBData()
{
	std::string filename(__FILE__);
	auto found = filename.find_last_of("/\\");
	filename = filename.substr(0, found + 1) + "../../dat/EM4M-NB.csv";

	bool exists = FileExists(filename);
	std::ofstream STATS;
	STATS.open(filename, std::ofstream::out | std::ofstream::app);
	if (!exists)
	{
		STATS << "SceneID,Solved,FirstTrajSuccess,"
				<< "SuccessfulPushes,UnsuccessfulPushes,"
				<< "PercentNGR,NumObjs,ObjDatas\n";
	}

	STATS << m_planner->GetSceneID() << ','
			<< (int)m_solved << ',' << m_planner->GetFirstTrajSuccess() << ','
			<< m_root_search->actions << ',' << m_root_search->noops << ','
			<< m_root_node->percent_ngr() << ',';

	const auto odata = m_root_node->obj_priority_data();
	STATS << odata.size() << ',';
	for (size_t i = 0; i < odata.size(); ++i)
	{
		// we store four elements for each object inside the ngr:
		// percentage of object volume inside, object height, object mass, object friction
		STATS << odata[i][0] << ',' << odata[i][1] << ',' << odata[i][2] << ',' << odata[i][3];
		if (i != odata.size()) {
			STATS << ',';
		}
	}
	STATS << '\n';

	STATS.close();
}

void MAMOSearch::Cleanup()
{
	for (auto& node: m_search_nodes)
	{
		if (node != nullptr)
		{
			delete node;
			node = nullptr;
		}
	}

	for (auto& state: m_search_states)
	{
		if (state != nullptr)
		{
			delete state;
			state = nullptr;
		}
	}

	m_hashtable.Reset();
}

bool MAMOSearch::expand(MAMOSearchState *state)
{
	SMPL_WARN("Expand %d, priority = %.2e", state->state_id, state->priority);
	auto node = m_hashtable.GetState(state->state_id);

	std::vector<MAMOAction> succ_object_centric_actions;
	std::vector<comms::ObjectsPoses> succ_objects;
	std::vector<trajectory_msgs::JointTrajectory> succ_trajs;
	std::vector<std::tuple<State, State, int> > debug_actions;
	bool close;
	double mapf_time = 0.0, get_succs_time = 0.0, sim_time = 0.0;
	node->GetSuccs(&succ_object_centric_actions, &succ_objects, &succ_trajs, &debug_actions, &close, &mapf_time, &get_succs_time, &sim_time);
	m_stats["push_planner_time"] += get_succs_time - sim_time;
	m_stats["sim_time"] += sim_time;
	m_stats["mapf_time"] += mapf_time;

	assert(succ_object_centric_actions.size() == succ_objects.size());
	assert(succ_objects.size() == succ_trajs.size());
	assert(succ_trajs.size() == succ_object_centric_actions.size());

	if (close)
	{
		state->closed = true;
		m_OPEN.erase(state->m_OPEN_h);
		return true;
	}

	createSuccs(node, state, &succ_object_centric_actions, &succ_objects, &succ_trajs, &debug_actions);

	double t_temp = GetTime();
	node->SaveNode(state->state_id, state->bp == nullptr ? 0 : state->bp->state_id, "EXPANDED");
	m_timer -= GetTime() - t_temp;

	return true;
}

bool MAMOSearch::done(MAMOSearchState *state)
{
	auto node = m_hashtable.GetState(state->state_id);
	auto action_to_node = node->kobject_centric_action();
	if (node->kparent() != nullptr && action_to_node._oid == -1 && action_to_node._type == MAMOActionType::DUMMY) {
		return false;
	}

	if (node->has_mapf_soln())
	{
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
	}

	if (state->force_done) {
		return true;
	}

	if (state->try_finalise) {
		return false;
	}

	double timer = GetTime();
	auto start_state = node->GetCurrentStartState();
	state->try_finalise = true;
	bool done = m_planner->FinalisePlan(node->kall_object_states(), start_state, m_exec_traj);
	m_stats["finalise_time"] += GetTime() - timer;
	return done;
}

void MAMOSearch::extractRearrangements()
{
	if (m_exec_traj.points.empty()) {
		SMPL_ERROR("Found MAMO solution without retrieval traj?");
	}

	m_rearrangements.clear();
	m_rearrangements.push_back(std::move(m_exec_traj));
	MAMOAction action(MAMOActionType::RETRIEVEOOI, m_planner->GetOoIID());
	action._params.push_back(m_planner->GetRobot()->GraspAt());
	m_actions.push_back(action);

	if (!m_home_traj.points.empty())
	{
		m_rearrangements.push_back(std::move(m_home_traj));

		action._params.clear();
		action._type = MAMOActionType::PUSH;
		action._oid = m_planner->GetOoIID();
		m_actions.push_back(action);
	}

	SMPL_WARN("Solution path state ids (from goal to start):");
	for (MAMOSearchState *state = m_solved_search; state; state = state->bp)
	{
		auto node = m_hashtable.GetState(state->state_id);
		if (node->has_traj())
		{
			SMPL_WARN("\t%d", state->state_id);
			m_rearrangements.push_back(node->krobot_traj());
			m_actions.push_back(node->kobject_centric_action());
		}
	}
	std::reverse(m_rearrangements.begin(), m_rearrangements.end());
	std::reverse(m_actions.begin(), m_actions.end());
}

void MAMOSearch::createSuccs(
	MAMONode *parent_node,
	MAMOSearchState *parent_search_state,
	std::vector<MAMOAction> *succ_object_centric_actions,
	std::vector<comms::ObjectsPoses> *succ_objects,
	std::vector<trajectory_msgs::JointTrajectory> *succ_trajs,
	std::vector<std::tuple<State, State, int> > *debug_actions)
{
	size_t num_succs = succ_object_centric_actions->size();
	if (num_succs == 0)
	{
		m_stats["no_succs"] += 1;
		// parent_search_state->closed = true;
		// m_OPEN.erase(parent_search_state->m_OPEN_h);

		parent_search_state->actions = std::numeric_limits<unsigned int>::max();
		parent_search_state->force_done = true;

		auto node = m_hashtable.GetState(parent_search_state->state_id);
		auto start_state = node->GetCurrentStartState();
		m_planner->PlanToHomeState(node->kall_object_states(), start_state, m_home_traj);
		m_exec_traj = m_planner->GetFirstTraj();

		m_OPEN.update(parent_search_state->m_OPEN_h);
		return;
	}
	bool duplicate_successor = false;
	if (succ_object_centric_actions->back()._oid == -1 && succ_object_centric_actions->back()._type == MAMOActionType::DUMMY) {
		duplicate_successor = true;
	}

	if (duplicate_successor && num_succs == 1) {
		m_stats["only_duplicate"] += 1;
	}
	if (!duplicate_successor) {
		m_stats["no_duplicate"] += 1;
	}

	for (size_t i = 0; i < num_succs; ++i)
	{
		bool at_duplicate = duplicate_successor && i == num_succs - 1;

		auto succ = new MAMONode;
		succ->SetPlanner(m_planner);
		succ->SetCBS(m_planner->GetCBS());
		succ->SetCC(m_planner->GetCC());
		succ->SetRobot(m_planner->GetRobot());
		succ->SetEdgeTo(succ_object_centric_actions->at(i));

		std::vector<int> reachable_ids;
		// SMPL_INFO("Successor number %d - IdentifyReachableMovables", i);
		// m_planner->GetRobot()->IdentifyReachableMovables(m_planner->GetAllAgents(), succ_objects->at(i), reachable_ids);
		succ->InitAgents(m_planner->GetAllAgents(), succ_objects->at(i), reachable_ids); // inits required fields for hashing

		// check if we have visited this mamo state before
		unsigned int old_id;
		MAMOSearchState *prev_search_state = nullptr;
		if (m_hashtable.Exists(succ))
		{
			old_id = m_hashtable.GetStateID(succ);
			prev_search_state = getSearchState(old_id);
		}

		unsigned int succ_actions = parent_search_state->actions + (1 - at_duplicate);
		unsigned int succ_noops = at_duplicate ? parent_search_state->noops + 1 : 0;
		if (prev_search_state != nullptr && (prev_search_state->closed || prev_search_state->actions < succ_actions))
		{
			// previously visited this mamo state with fewer actions, move on
			// SMPL_INFO("Successor number %d - previously visited this mamo state with fewer actions, move on", i);
			delete succ;
			continue;
		}
		else
		{
			if (at_duplicate)
			{
				delete succ;
				for (auto it = debug_actions->begin() + i; it != debug_actions->end(); ++it) {
					parent_node->AddDebugAction(*it);
				}
				// parent_search_state->priority = parent_search_state->priority * (boost::math::pdf(D_noops, succ_noops) / boost::math::pdf(D_noops, parent_search_state->noops));
				parent_search_state->noops = succ_noops;
				m_OPEN.update(parent_search_state->m_OPEN_h);
				SMPL_WARN("Update duplicate %d, priority = %.2e", old_id, parent_search_state->priority);

				// std::stringstream reachable_str;
				// std::copy(reachable_ids.begin(), reachable_ids.end(), std::ostream_iterator<int>(reachable_str, ","));
				// SMPL_INFO("Movables reachable in DUPLICATE state %d : (%s)", old_id, reachable_str.str().c_str());
			}
			else
			{
				succ->SetRobotTrajectory(succ_trajs->at(i));
				succ->SetParent(parent_node);
				succ->AddDebugAction(debug_actions->at(i));

				if (prev_search_state != nullptr)
				{
					// update search state in open list
					auto old_succ = m_hashtable.GetState(old_id);
					old_succ->parent()->RemoveChild(old_succ);

					m_hashtable.UpdateState(succ);
					prev_search_state->bp = parent_search_state;
					prev_search_state->actions = succ_actions;
					// prev_search_state->noops = succ_noops;
					// // priority update unaffected by visiting the same state as before
					// computeMAMOPriority(prev_search_state);
					m_OPEN.update(prev_search_state->m_OPEN_h);
					// SMPL_WARN("Update %d, priority = %.2e ", old_id, prev_search_state->priority);
				}
				else
				{
					// add search state to open list
					unsigned int succ_id = m_hashtable.GetStateIDForceful(succ);
					auto succ_search_state = getSearchState(succ_id);
					succ_search_state->bp = parent_search_state;
					succ_search_state->actions = succ_actions;
					succ_search_state->noops = succ_noops;
					// succ_search_state->priority = succ->ComputeMAMOPriorityOrig();
					succ->ComputePriorityFactors();
					computeMAMOPriority(succ_search_state);
					succ_search_state->m_OPEN_h = m_OPEN.push(succ_search_state);

					SMPL_WARN("Generate %d, priority = %.2e", succ_id, succ_search_state->priority);
					++m_stats["generated"];

					double t_temp = GetTime();
					auto node = m_hashtable.GetState(succ_id);
					node->RunMAPF();
					node->SaveNode(succ_id, parent_search_state->state_id, "GENERATED");
					node->ResetConstraints();
					m_timer -= GetTime() - t_temp;

					// std::stringstream reachable_str;
					// std::copy(reachable_ids.begin(), reachable_ids.end(), std::ostream_iterator<int>(reachable_str, ","));
					// SMPL_INFO("Movables reachable in NEW state %d : (%s)", succ_id, reachable_str.str().c_str());
				}
				parent_node->AddChild(succ);
				m_search_nodes.push_back(succ);
			}
		}
	}
}

MAMOSearchState* MAMOSearch::getSearchStateForceful()
{
	unsigned int state_id = m_search_states.size();
	m_search_states.resize(state_id + 1, nullptr);

	auto& state = m_search_states[state_id];
	state = createSearchState(state_id);

	return state;
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
	state->priority = std::numeric_limits<double>::lowest();
	state->actions = -1;
	state->noops = -1;
	state->closed = false;
	state->try_finalise = false;
	state->force_done = false;
	state->bp = nullptr;
}

void MAMOSearch::createDists()
{
	D_percent_ngr = boost::math::beta_distribution<>(0.3403101839, 45.0750087432);
	D_num_objs = boost::math::exponential_distribution<>(1.5634743875);
	D_noops = boost::math::exponential_distribution<>(0.4526209677);
	D_odata = boost::math::exponential_distribution<>(32.1092604858);
}

double MAMOSearch::computeMAMOPriority(MAMOSearchState *state)
{
	auto node = m_hashtable.GetState(state->state_id);
	const auto odata = node->obj_priority_data();

	if (node->percent_ngr() == 0) {
		state->priority = std::numeric_limits<double>::max();
	}
	else
	{
		state->priority = solvable_prior *
							boost::math::pdf(D_percent_ngr, node->percent_ngr()) *
							boost::math::pdf(D_num_objs, odata.size());
							//  *
							// boost::math::pdf(D_noops, state->noops);
		for (size_t i = 0; i < odata.size(); ++i) {
			state->priority *= boost::math::pdf(D_odata, odata[i][0] * odata[i][2] * odata[i][3]);
		}
	}
}

} // namespace clutter
