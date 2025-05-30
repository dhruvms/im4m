#include <pushplan/utils/data_gen.hpp>
#include <pushplan/utils/helpers.hpp>

#include <eigen_conversions/eigen_msg.h>

#include <string>

namespace clutter
{

DataGeneration::DataGeneration() : m_rng(m_dev()), m_ph("~")
{
	m_distD = std::uniform_real_distribution<double>(0.0, 1.0);
}

void DataGeneration::Reset()
{
	if (m_robot)
	{
		m_sim.reset();
		m_robot.reset();
	}

	std::string tables;
	int num_immov, num_mov;

	m_ph.getParam("object_filename", tables);
	m_ph.getParam("objects/num_immov", num_immov);
	m_ph.getParam("objects/num_mov", num_mov);

	m_sim = std::make_unique<BulletSim>(
				tables, false,
				-1, std::string(),
				0, 1, false); // z_offset = true randomly displaces table/shelf along z-axis

	m_robot = std::make_unique<Robot>();
	m_robot->SetSim(m_sim.get());
	m_robot->Setup();
	m_robot->AddObstaclesFromSim();

	setupSim(m_sim.get(), m_robot->GetStartState()->joint_state, -1);
	m_vis_pub = m_nh.advertise<visualization_msgs::Marker>( "/visualization_marker", 10);
}

void DataGeneration::GetPushData()
{
	Object movable;
	createObject(movable);

	std::string filename(__FILE__);
	auto found = filename.find_last_of("/\\");

	filename = filename.substr(0, found + 1) + "../../dat/push_data/PUSH_DATA";
	int modifier;
	m_ph.getParam("robot/study", modifier);
	filename += "_" + std::to_string(modifier) + ".csv";

	bool exists = FileExists(filename);
	std::ofstream DATA;
	DATA.open(filename, std::ofstream::out | std::ofstream::app);
	if (!exists)
	{
		DATA 	<< "o_ox,o_oy,o_oz,o_oyaw,o_shape,o_xs,o_ys,o_zs,o_mass,o_mu,"
				<< "m_dir_des,m_dist_des,"
				<< "s_x,s_y,s_z,"
				<< "s_r11,s_r21,s_r31,s_r12,s_r22,s_r32,s_r13,s_r23,s_r33,"
				<< "m_dir_ach,m_dist_ach,"
				<< "e_x,e_y,e_z,"
				<< "e_r11,e_r21,e_r31,e_r12,e_r22,e_r32,e_r13,e_r23,e_r33,"
				<< "r\n";
	}

	int unreachable_count = 0;
	for (int i = 0; i < 25; ++i)
	{
		std::vector<double> push;
		double to_move_dir, to_move_dist, moved_dir, moved_dist;
		Eigen::Affine3d start_pose, result_pose;
		PushResult result = PushResult::PUSH_RESULTS;
		m_robot->GenMovablePush(
			movable,
			push,
			to_move_dir, to_move_dist, moved_dir, moved_dist,
			start_pose, result_pose,
			result);
		ROS_WARN("Push sample result: %d", result);
		unreachable_count += static_cast<int>(result) <= 0 ? -unreachable_count : 1;

		if (unreachable_count == 5) {
			break;
		}
		if (moved_dist < 0.05) {
			continue;
		}

		DATA 	<< movable.desc.o_x << ','
				<< movable.desc.o_y << ','
				<< movable.desc.o_z << ','
				<< movable.desc.o_yaw << ','
				<< movable.desc.shape << ','
				<< movable.desc.x_size << ','
				<< movable.desc.y_size << ','
				<< movable.desc.z_size << ','
				<< movable.desc.mass << ','
				<< movable.desc.mu << ','
				<< to_move_dir << ','
				<< to_move_dist << ','
				<< start_pose.translation().x() << ','
				<< start_pose.translation().y() << ','
				<< start_pose.translation().z() << ','
				<< start_pose.rotation()(0, 0) << ','
				<< start_pose.rotation()(1, 0) << ','
				<< start_pose.rotation()(2, 0) << ','
				<< start_pose.rotation()(0, 1) << ','
				<< start_pose.rotation()(1, 1) << ','
				<< start_pose.rotation()(2, 1) << ','
				<< start_pose.rotation()(0, 2) << ','
				<< start_pose.rotation()(1, 2) << ','
				<< start_pose.rotation()(2, 2) << ','
				<< moved_dir << ','
				<< moved_dist << ','
				<< result_pose.translation().x() << ','
				<< result_pose.translation().y() << ','
				<< result_pose.translation().z() << ','
				<< result_pose.rotation()(0, 0) << ','
				<< result_pose.rotation()(1, 0) << ','
				<< result_pose.rotation()(2, 0) << ','
				<< result_pose.rotation()(0, 1) << ','
				<< result_pose.rotation()(1, 1) << ','
				<< result_pose.rotation()(2, 1) << ','
				<< result_pose.rotation()(0, 2) << ','
				<< result_pose.rotation()(1, 2) << ','
				<< result_pose.rotation()(2, 2) << ','
				<< static_cast<int>(result) << '\n';
	}
	DATA.close();
}

void DataGeneration::VisObject()
{
	Object movable;
	createObject(movable);
	auto marker = movable.GetMarker();
	m_vis_pub.publish(marker);

	std::vector<Eigen::Affine3d> pregrasps;
	movable.GetPregrasps(pregrasps);
	if (!pregrasps.empty())
	{
		for (int i = 0; i < pregrasps.size(); ++i)
		{
			visualization_msgs::Marker marker;
			marker.header.frame_id = "base_footprint";
			marker.header.stamp = ros::Time();
			marker.ns = "object_pregrasp_poses";
			marker.id = i;
			marker.action = visualization_msgs::Marker::ADD;
			marker.type = visualization_msgs::Marker::ARROW;

			geometry_msgs::Pose pose;
			tf::poseEigenToMsg(pregrasps[i], pose);
			marker.pose = pose;

			marker.scale.x = 0.1;
			marker.scale.y = 0.015;
			marker.scale.z = 0.015;

			marker.color.a = 0.7; // Don't forget to set the alpha!
			marker.color.r = 0.0;
			marker.color.g = 0.7;
			marker.color.b = 0.6;

			m_vis_pub.publish(marker);

			// marker.id = i + pregrasps.size();
			// marker.type = visualization_msgs::Marker::SPHERE;

			// marker.scale.x = 2.0 * 0.07;
			// marker.scale.y = 2.0 * 0.07;
			// marker.scale.z = 2.0 * 0.1;

			// m_vis_pub.publish(marker);
		}
	}
	SMPL_INFO("Done visualising object and pregrasps!");
}

void DataGeneration::createObject(Object &movable)
{
	auto mov_objs = m_sim->GetMovableObjs();
	auto mov_obj_ids = m_sim->GetMovableObjIDs();
	movable.desc.id = mov_obj_ids->front().first;
	movable.desc.shape = mov_obj_ids->front().second;
	movable.desc.type = 1; // movable
	movable.desc.o_x = mov_objs->front().at(0);
	movable.desc.o_y = mov_objs->front().at(1);
	movable.desc.o_z = mov_objs->front().at(2);
	movable.desc.o_roll = mov_objs->front().at(3);
	movable.desc.o_pitch = mov_objs->front().at(4);
	movable.desc.o_yaw = mov_objs->front().at(5);
	movable.desc.ycb = (bool)mov_objs->front().back();

	if (movable.desc.ycb)
	{
		auto itr = YCB_OBJECT_DIMS.find(mov_obj_ids->front().second);
		if (itr != YCB_OBJECT_DIMS.end())
		{
			movable.desc.x_size = itr->second.at(0);
			movable.desc.y_size = itr->second.at(1);
			movable.desc.z_size = itr->second.at(2);
			movable.desc.o_yaw += itr->second.at(3);
		}
		movable.desc.mass = -1;
		movable.desc.mu = mov_objs->front().at(6);
	}
	else
	{
		movable.desc.x_size = mov_objs->front().at(6);
		movable.desc.y_size = mov_objs->front().at(7);
		movable.desc.z_size = mov_objs->front().at(8);
		movable.desc.mass = mov_objs->front().at(9);
		movable.desc.mu = mov_objs->front().at(10);
	}
	movable.desc.movable = true;
	movable.desc.locked = false;

	movable.CreateCollisionObjects();
	movable.CreateSMPLCollisionObject();
	movable.GenerateCollisionModels();
	movable.InitProperties();
}

} // namespace clutter
