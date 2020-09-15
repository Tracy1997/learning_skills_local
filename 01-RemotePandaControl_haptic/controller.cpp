// This example tests the haptic device driver and the open-loop bilateral teleoperation controller.

#include "redis/RedisClient.h"
#include "timer/LoopTimer.h"
#include "haptic_tasks/HapticController.h"

#include <iostream>
#include <string>
#include <random>
#include <queue>

#define INIT            0
#define CONTROL         1

int state = INIT;

#include <signal.h>
bool runloop = false;
void sighandler(int){runloop = false;}

using namespace std;
using namespace Eigen;

// redis keys:
//// Haptic device related keys ////
// Maximum stiffness, damping and force specifications
vector<string> DEVICE_MAX_STIFFNESS_KEYS = {
	"sai2::ChaiHapticDevice::device0::specifications::max_stiffness",
	"sai2::ChaiHapticDevice::device1::specifications::max_stiffness",
	};
vector<string> DEVICE_MAX_DAMPING_KEYS = {
	"sai2::ChaiHapticDevice::device0::specifications::max_damping",
	"sai2::ChaiHapticDevice::device1::specifications::max_damping",
	};
vector<string> DEVICE_MAX_FORCE_KEYS = {
	"sai2::ChaiHapticDevice::device0::specifications::max_force",
	"sai2::ChaiHapticDevice::device1::specifications::max_force",
	};
// Set force and torque feedback of the haptic device
vector<string> DEVICE_COMMANDED_FORCE_KEYS = {
	"sai2::ChaiHapticDevice::device0::actuators::commanded_force",
	"sai2::ChaiHapticDevice::device1::actuators::commanded_force",
	};
vector<string> DEVICE_COMMANDED_TORQUE_KEYS = {
	"sai2::ChaiHapticDevice::device0::actuators::commanded_torque",
	"sai2::ChaiHapticDevice::device1::actuators::commanded_torque",
	};
vector<string> DEVICE_COMMANDED_GRIPPER_FORCE_KEYS = {
	"sai2::ChaiHapticDevice::device0::actuators::commanded_force_gripper",
	"sai2::ChaiHapticDevice::device1::actuators::commanded_force_gripper",
	};
// Haptic device current position and rotation
vector<string> DEVICE_POSITION_KEYS = {
	"sai2::ChaiHapticDevice::device0::sensors::current_position",
	"sai2::ChaiHapticDevice::device1::sensors::current_position",
	};
vector<string> DEVICE_ROTATION_KEYS = {
	"sai2::ChaiHapticDevice::device0::sensors::current_rotation",
	"sai2::ChaiHapticDevice::device1::sensors::current_rotation",
	};
vector<string> DEVICE_GRIPPER_POSITION_KEYS = {
	"sai2::ChaiHapticDevice::device0::sensors::current_position_gripper",
	"sai2::ChaiHapticDevice::device1::sensors::current_position_gripper",
	};
// Haptic device current velocity
vector<string> DEVICE_TRANS_VELOCITY_KEYS = {
	"sai2::ChaiHapticDevice::device0::sensors::current_trans_velocity",
	"sai2::ChaiHapticDevice::device1::sensors::current_trans_velocity",
	};
vector<string> DEVICE_ROT_VELOCITY_KEYS = {
	"sai2::ChaiHapticDevice::device0::sensors::current_rot_velocity",
	"sai2::ChaiHapticDevice::device1::sensors::current_rot_velocity",
	};
vector<string> DEVICE_GRIPPER_VELOCITY_KEYS = {
	"sai2::ChaiHapticDevice::device0::sensors::current_gripper_velocity",
	"sai2::ChaiHapticDevice::device1::sensors::current_gripper_velocity",
	};
vector<string> DEVICE_SENSED_FORCE_KEYS = {
	"sai2::ChaiHapticDevice::device0::sensors::sensed_force",
	"sai2::ChaiHapticDevice::device1::sensors::sensed_force",
	};
vector<string> DEVICE_SENSED_TORQUE_KEYS = {
	"sai2::ChaiHapticDevice::device0::sensors::sensed_torque",
	"sai2::ChaiHapticDevice::device1::sensors::sensed_torque",
	};

// dual proxy
string ROBOT_PROXY_KEY = "sai2::HapticApplications::01::dual_proxy::robot_proxy";
string HAPTIC_PROXY_KEY = "sai2::HapticApplications::01::dual_proxy::haptic_proxy";
string FORCE_SPACE_DIMENSION_KEY = "sai2::HapticApplications::01::dual_proxy::force_space_dimension";
string SIGMA_FORCE_KEY = "sai2::HapticApplications::01::dual_proxy::sigma_force";

string HAPTIC_DEVICE_READY_KEY = "sai2::HapticApplications::01::dual_proxy::haptic_device_ready";
string CONTROLLER_RUNNING_KEY = "sai2::HapticApplications::01::dual_proxy::controller_running";

int force_space_dimension = 0;
Matrix3d sigma_force = Matrix3d::Zero();
int haptic_device_ready = 0;
Vector3d haptic_proxy = Vector3d::Zero();
Vector3d robot_proxy = Vector3d::Zero();

RedisClient redis_client_local;
RedisClient redis_client_remote;

// communication function prototype
void communication();

int main() {

	// start redis clients
	redis_client_local = RedisClient();
	redis_client_local.connect();

	string remote_ip = "127.0.0.1";
	int remote_port = 6379;
	redis_client_remote = RedisClient();
	redis_client_remote.connect(remote_ip, remote_port);

	// set up signal handler
	signal(SIGABRT, &sighandler);
	signal(SIGTERM, &sighandler);
	signal(SIGINT, &sighandler);

	// dual proxy parameters
	double k_vir = 200.0;
	double max_force_diff = 0.02;
	double max_force = 10.0;

	Vector3d prev_desired_force = Vector3d::Zero();

	// haptic control
	////Haptic teleoperation controller ////
	if(redis_client_remote.get(CONTROLLER_RUNNING_KEY) != "1")
	{
		cout << "run the robot controller before the haptic controller" << endl;
		return 0;
	}
	Vector3d robot_workspace_center = redis_client_remote.getEigenMatrixJSON(HAPTIC_PROXY_KEY);
	auto teleop_task = new Sai2Primitives::HapticController(robot_workspace_center, Matrix3d::Zero());
	teleop_task->_send_haptic_feedback = false;

	//User switch states
	teleop_task->UseGripperAsSwitch();
	bool gripper_state = false;
	bool gripper_state_prev = false;

	robot_proxy = robot_workspace_center;

	 //Task scaling factors
	double Ks = 2.5;
	double KsR = 1.0;
	teleop_task->setScalingFactors(Ks, KsR);

	VectorXd _max_stiffness_device0 = redis_client_local.getEigenMatrixJSON(DEVICE_MAX_STIFFNESS_KEYS[0]);
	VectorXd _max_damping_device0 = redis_client_local.getEigenMatrixJSON(DEVICE_MAX_DAMPING_KEYS[0]);
	VectorXd _max_force_device0 = redis_client_local.getEigenMatrixJSON(DEVICE_MAX_FORCE_KEYS[0]);

	//set the device specifications to the haptic controller
	teleop_task->_max_linear_stiffness_device = _max_stiffness_device0[0];
	teleop_task->_max_angular_stiffness_device = _max_stiffness_device0[1];
	teleop_task->_max_linear_damping_device = _max_damping_device0[0];
	teleop_task->_max_angular_damping_device = _max_damping_device0[1];
	teleop_task->_max_force_device = _max_force_device0[0];
	teleop_task->_max_torque_device = _max_force_device0[1];

	double kv_haptic = 0.9 * _max_damping_device0[0];

	// setup redis keys to be updated with the callback
	redis_client_local.createReadCallback(0);
	redis_client_local.createWriteCallback(0);

	// Objects to read from redis
    redis_client_local.addEigenToReadCallback(0, DEVICE_POSITION_KEYS[0], teleop_task->_current_position_device);
    redis_client_local.addEigenToReadCallback(0, DEVICE_ROTATION_KEYS[0], teleop_task->_current_rotation_device);
    redis_client_local.addEigenToReadCallback(0, DEVICE_TRANS_VELOCITY_KEYS[0], teleop_task->_current_trans_velocity_device);
    redis_client_local.addEigenToReadCallback(0, DEVICE_ROT_VELOCITY_KEYS[0], teleop_task->_current_rot_velocity_device);
    redis_client_local.addEigenToReadCallback(0, DEVICE_SENSED_FORCE_KEYS[0], teleop_task->_sensed_force_device);
    redis_client_local.addEigenToReadCallback(0, DEVICE_SENSED_TORQUE_KEYS[0], teleop_task->_sensed_torque_device);
    redis_client_local.addDoubleToReadCallback(0, DEVICE_GRIPPER_POSITION_KEYS[0], teleop_task->_current_position_gripper_device);
    redis_client_local.addDoubleToReadCallback(0, DEVICE_GRIPPER_VELOCITY_KEYS[0], teleop_task->_current_gripper_velocity_device);

	// Objects to write to redis
	//write haptic commands
	redis_client_local.addEigenToWriteCallback(0, DEVICE_COMMANDED_FORCE_KEYS[0], teleop_task->_commanded_force_device);
	redis_client_local.addEigenToWriteCallback(0, DEVICE_COMMANDED_TORQUE_KEYS[0], teleop_task->_commanded_torque_device);
	redis_client_local.addDoubleToWriteCallback(0, DEVICE_COMMANDED_GRIPPER_FORCE_KEYS[0], teleop_task->_commanded_gripper_force_device);

	// start communication thread and loop
	runloop = true;
	thread communication_thread(communication);

	// create a timer
	double control_loop_freq = 1000.0;
	unsigned long long controller_counter = 0;
	LoopTimer timer;
	timer.initializeTimer();
	timer.setLoopFrequency(control_loop_freq); //Compiler en mode release
	double current_time = 0;
	double prev_time = 0;
	// double dt = 0;
	bool fTimerDidSleep = true;
	double start_time = timer.elapsedTime(); //secs

	while (runloop)
	{
		// wait for next scheduled loop
		timer.waitForNextLoop();
		current_time = timer.elapsedTime() - start_time;

		// read haptic state and robot state
		redis_client_local.executeReadCallback(0);

		teleop_task->UseGripperAsSwitch();
		gripper_state_prev = gripper_state;
		gripper_state = teleop_task->gripper_state;

		if(state == INIT)
		{
  			// compute homing haptic device
  			teleop_task->HomingTask();

			if(teleop_task->device_homed && gripper_state)
			{
				teleop_task->setRobotCenter(haptic_proxy);
				teleop_task->setDeviceCenter(teleop_task->_current_position_device);

				// Reinitialize controllers
				teleop_task->reInitializeTask();

				haptic_device_ready = 1;
				state = CONTROL;
			}
		}

		else if(state == CONTROL)
		{

			// dual proxy
			teleop_task->computeHapticCommands3d(robot_proxy);

			Vector3d device_position = teleop_task->_current_position_device;
			Vector3d proxy_position_device_frame = teleop_task->_home_position_device + teleop_task->_Rotation_Matrix_DeviceToRobot * (haptic_proxy - teleop_task->_center_position_robot) / Ks;

			Vector3d desired_force = k_vir * sigma_force * (proxy_position_device_frame - device_position);
			Vector3d desired_force_diff = desired_force - prev_desired_force;
			if( desired_force_diff.norm() > max_force_diff )
			{
				desired_force = prev_desired_force + max_force_diff * desired_force_diff/desired_force_diff.norm();
			}
			if(desired_force.norm() > max_force)
			{
				desired_force *= max_force/desired_force.norm();
			}

			teleop_task->_commanded_force_device = desired_force - kv_haptic * sigma_force * teleop_task->_current_trans_velocity_device;

			// 
			prev_desired_force = desired_force;

		}

		// write control torques
		redis_client_local.executeWriteCallback(0);

		controller_counter++;
	}

	communication_thread.join();

	//// Send zero force/torque to haptic device through Redis keys ////
	redis_client_local.setEigenMatrixJSON(DEVICE_COMMANDED_FORCE_KEYS[0], Vector3d::Zero());
	redis_client_local.setEigenMatrixJSON(DEVICE_COMMANDED_TORQUE_KEYS[0], Vector3d::Zero());
	redis_client_local.set(DEVICE_COMMANDED_GRIPPER_FORCE_KEYS[0], "0.0");

	double end_time = timer.elapsedTime();
	std::cout << "\n";
	std::cout << "Controller Loop run time  : " << end_time << " seconds\n";
	std::cout << "Controller Loop updates   : " << timer.elapsedCycles() << "\n";
    std::cout << "Controller Loop frequency : " << timer.elapsedCycles()/end_time << "Hz\n";


}

void communication()
{

	// prepare redis client remote communication
	redis_client_remote.createReadCallback(0);
	redis_client_remote.createWriteCallback(0);

	redis_client_remote.addEigenToReadCallback(0, HAPTIC_PROXY_KEY, haptic_proxy);
	redis_client_remote.addIntToReadCallback(0, FORCE_SPACE_DIMENSION_KEY, force_space_dimension);
	redis_client_remote.addEigenToReadCallback(0, SIGMA_FORCE_KEY, sigma_force);

	redis_client_remote.addEigenToWriteCallback(0, ROBOT_PROXY_KEY, robot_proxy);
	redis_client_remote.addIntToWriteCallback(0, HAPTIC_DEVICE_READY_KEY, haptic_device_ready);

	// create a timer
	double communication_freq = 50.0;
	LoopTimer timer;
	timer.initializeTimer();
	timer.setLoopFrequency(communication_freq); //Compiler en mode release
	double current_time = 0;
	double prev_time = 0;
	// double dt = 0;
	bool fTimerDidSleep = true;
	double start_time = timer.elapsedTime(); //secs

	unsigned long long communication_counter = 0;
	
	while(runloop)
	{
		timer.waitForNextLoop();

		redis_client_remote.executeReadCallback(0);
		redis_client_remote.executeWriteCallback(0);

		communication_counter++;
	}

	redis_client_remote.set(HAPTIC_DEVICE_READY_KEY, "0");

	double end_time = timer.elapsedTime();
	std::cout << "\n";
	std::cout << "Communication Loop run time  : " << end_time << " seconds\n";
	std::cout << "Communication Loop updates   : " << timer.elapsedCycles() << "\n";
    std::cout << "Communication Loop frequency : " << timer.elapsedCycles()/end_time << "Hz\n";



}
