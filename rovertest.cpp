// =============================================================================
// PROJECT CHRONO - http://projectchrono.org
//
// Copyright (c) 2019 projectchrono.org
// All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file at the top level of the distribution and at
// http://projectchrono.org/license-chrono.txt.
//
// =============================================================================
// Authors: Nic Olsen, Conlain Kelly
// =============================================================================
// Simplified rover using Chrono for rover dynamics co-simulated with
// Chrono::Granular for granular terrain.
// =============================================================================

#include <iostream>
#include <string>
#include <vector>

#include "chrono/core/ChTimer.h"
#include "chrono/physics/ChBody.h"
#include "chrono/physics/ChForce.h"
#include "chrono/physics/ChLinkMotorRotationAngle.h"
#include "chrono/physics/ChLinkMotorRotationSpeed.h"
#include "chrono/physics/ChLinkMotorRotationTorque.h"
#include "chrono/physics/ChSystemNSC.h"
#include "chrono/timestepper/ChTimestepper.h"
#include "chrono/utils/ChUtilsCreators.h"
#include "chrono/utils/ChUtilsSamplers.h"
#include "chrono_gpu/ChGpuData.h"
#include "chrono_gpu/physics/ChSystemGpu.h"
#include "chrono_gpu/utils/ChGpuJsonParser.h"
#include "chrono_thirdparty/filesystem/path.h"

using namespace chrono;
using namespace chrono::gpu;

constexpr double mars_grav_mag = 370;

constexpr double time_settling = 1.0; // TODO
constexpr double time_running = 10.0; // TODO

constexpr double METERS_TO_CM = 100;
constexpr double KG_TO_GRAM = 1000;

constexpr double wheel_rad = 0.13 * METERS_TO_CM;
constexpr double wheel_width = 0.16 * METERS_TO_CM;

constexpr double ROVER_MASS_REDUCTION = 1.;

constexpr double wheel_mass = ROVER_MASS_REDUCTION * 4 * KG_TO_GRAM;
constexpr double chassis_mass = ROVER_MASS_REDUCTION * 161 * KG_TO_GRAM;

// distance wheels are in front of / behind chassis COM
constexpr double front_wheel_offset_x = 0.7 * METERS_TO_CM;
constexpr double front_wheel_offset_y = 0.6 * METERS_TO_CM;

constexpr double middle_wheel_offset_x = -0.01 * METERS_TO_CM;
constexpr double middle_wheel_offset_y = 0.55 * METERS_TO_CM;

constexpr double rear_wheel_offset_x = -0.51 * METERS_TO_CM;
constexpr double rear_wheel_offset_y = 0.6 * METERS_TO_CM;

constexpr double wheel_offset_z = -0.164 * METERS_TO_CM;

// assume chassis is solid rectangle inertially, these are the dimensions
constexpr double chassis_length_x = 2 * METERS_TO_CM;
constexpr double chassis_length_y = 2 * METERS_TO_CM;
constexpr double chassis_length_z = 1.5 * METERS_TO_CM;

const ChMatrix33<float> wheel_scaling = ChMatrix33<float>(
    ChVector<float>(wheel_rad * 2, wheel_width, wheel_rad * 2));

constexpr double wheel_inertia_x =
    (1. / 4.) * wheel_mass * wheel_rad * wheel_rad + (1 / 12.) * wheel_mass;
constexpr double wheel_inertia_y =
    (1. / 2.) * wheel_mass * wheel_rad * wheel_rad;
constexpr double wheel_inertia_z = wheel_inertia_x;

unsigned int out_fps = 50;

double terrain_height_offset = 0;

enum RUN_MODE { SETTLING = 0, TESTING = 1 };

enum ROVER_BODY_ID {
  WHEEL_FRONT_LEFT,
  WHEEL_FRONT_RIGHT,
  WHEEL_REAR_LEFT,
  WHEEL_REAR_RIGHT
};

std::vector<std::shared_ptr<chrono::ChBody>> wheel_bodies;

std::vector<string> mesh_filenames;
std::vector<ChMatrix33<float>> mesh_rotscales;
std::vector<float3> mesh_translations;
std::vector<float> mesh_masses;

// y is height, x and z are radial
// starts as height=1, diameter = 1

void ShowUsage(std::string name) {
  std::cout << "usage: " + name +
                   " <json_file> <run_mode: 0-settling, 1-running> "
                   "<checkpoint_file_base> <gravity "
                   "angle (deg)>"
            << std::endl;
}

std::vector<ChVector<float>> loadCheckpointFile(std::string checkpoint_file) {
  // Read in checkpoint file
  std::vector<ChVector<float>> body_points;

  std::string line;
  std::ifstream cp_file(checkpoint_file);
  if (!cp_file.is_open()) {
    std::cout << "ERROR reading checkpoint file" << std::endl;
    exit(1);
  }

  std::string d = ",";
  std::getline(cp_file, line); // Skip the header
  while (std::getline(cp_file, line)) {
    size_t pos = line.find(d);
    std::string tok;
    std::string d = ",";
    ChVector<float> point;
    for (size_t i = 0; i < 3; i++) {
      pos = line.find(d);
      tok = line.substr(0, pos);
      point[i] = std::stof(tok);
      line.erase(0, pos + 1);
      // if (i == 2 && point[i] > max_gran_z) {
      //     max_gran_z = point[i];
      // }
    }
    body_points.push_back(point);
  }
  cp_file.close();
  return body_points;
}

void addWheelBody(ChSystemNSC &rover_sys, std::shared_ptr<ChBody> chassis_body,
                  std::string wheel_filename,
                  const ChVector<> &wheel_initial_pos_relative) {
  ChVector<> wheel_initial_pos =
      chassis_body->GetPos() + wheel_initial_pos_relative;
  std::shared_ptr<ChBody> wheel_body(rover_sys.NewBody());

  wheel_body->SetMass(wheel_mass);
  wheel_body->SetBodyFixed(false);
  // assume it's a cylinder inertially
  wheel_body->SetInertiaXX(
      ChVector<>(wheel_inertia_x, wheel_inertia_y, wheel_inertia_z));

  printf("Inertia tensor is %f, %f, %f\n", wheel_inertia_x, wheel_inertia_y,
         wheel_inertia_z);
  wheel_body->SetPos(wheel_initial_pos);
  rover_sys.AddBody(wheel_body);

  auto joint = std::make_shared<ChLinkLockRevolute>();
  joint->Initialize(chassis_body, wheel_body,
                    ChCoordsys<>(wheel_initial_pos, Q_from_AngX(CH_C_PI / 2)));
  rover_sys.AddLink(joint);

  auto motor = std::make_shared<ChLinkMotorRotationAngle>();

  motor->Initialize(chassis_body, wheel_body,
                    ChFrame<>(wheel_initial_pos, Q_from_AngX(CH_C_PI / 2)));

  motor->SetMotorFunction(std::make_shared<ChFunction_Ramp>(0, CH_C_PI));
  rover_sys.AddLink(motor);

  mesh_masses.push_back(wheel_mass);
  mesh_rotscales.push_back(wheel_scaling);
  mesh_filenames.push_back(wheel_filename);
  mesh_translations.push_back(make_float3(0, 0, 0));
  wheel_bodies.push_back(wheel_body);
}

void writeMeshFrames(std::ostringstream &outstream,
                     std::shared_ptr<ChBody> body, std::string obj_name,
                     ChMatrix33<float> mesh_scaling) {
  outstream << obj_name << ",";

  // Get frame position
  ChFrame<> body_frame = body->GetFrame_REF_to_abs();
  ChQuaternion<> rot = body_frame.GetRot();
  ChVector<> pos =
      body_frame.GetPos() + ChVector<>(0, 0, terrain_height_offset);

  // Get basis vectors
  ChVector<> vx = rot.GetXaxis();
  ChVector<> vy = rot.GetYaxis();
  ChVector<> vz = rot.GetZaxis();

  printf("Rot is (%f, %f, %f, %f)\n", rot.e0(), rot.e1(), rot.e2(), rot.e3());
  // normalize basis vectors
  vx = vx / vx.Length();
  vy = vy / vy.Length();
  vz = vz / vz.Length();

  // Output in order
  outstream << pos.x() << ",";
  outstream << pos.y() << ",";
  outstream << pos.z() << ",";
  outstream << vx.x() << ",";
  outstream << vx.y() << ",";
  outstream << vx.z() << ",";
  outstream << vy.x() << ",";
  outstream << vy.y() << ",";
  outstream << vy.z() << ",";
  outstream << vz.x() << ",";
  outstream << vz.y() << ",";
  outstream << vz.z() << ",";

  // printf("wheel scaling is %f, %f, %f\n", mesh_scaling.x, mesh_scaling.y,
  // mesh_scaling.z);
  outstream << mesh_scaling(0, 0) << "," << mesh_scaling(1, 1) << ","
            << mesh_scaling(2, 2);
  outstream << "\n";
}

int main(int argc, char *argv[]) {
  gpu::SetDataPath("../data/");

  ChGpuSimulationParameters params;
  if (argc != 5 || ParseJSON(gpu::GetDataFile(argv[1]), params) == false) {
    ShowUsage(argv[0]);
    return 1;
  }
  std::string chassis_filename =
      gpu::GetDataFile("meshes/MER_body.obj"); // For output only
  std::string wheel_filename = gpu::GetDataFile("meshes/wheel_scaled.obj");

  RUN_MODE run_mode = (RUN_MODE)std::atoi(argv[2]);
  std::string checkpoint_file_base = std::string(argv[3]);

  // Rotates gravity about +Y axis
  double input_grav_angle_deg = std::stod(argv[4]);
  double grav_angle = 2.0 * CH_C_PI * input_grav_angle_deg / 360.0;

  double Gx = -mars_grav_mag * std::sin(grav_angle);
  double Gy = 0;
  double Gz = -mars_grav_mag * std::cos(grav_angle);

  std::cout << "Gravity (" << input_grav_angle_deg << "deg): " << Gx << " "
            << Gy << " " << Gz << std::endl;

  double iteration_step = params.step_size;

  // Setup granular simulation
  ChSystemGpuMesh gpu_sys(
      params.sphere_radius, params.sphere_density,
      make_float3(params.box_X, params.box_Y, params.box_Z));

  double fill_bottom = 0; // TODO
  double fill_top = params.box_Z / 2.0;

  // leave a 4cm margin at edges of sampling
  ChVector<> hdims(params.box_X / 2 - 2.0, params.box_Y / 2 - 2.0,
                   std::abs((fill_bottom - fill_top) / 2.) - 2.0);
  ChVector<> center(0, 0, (fill_bottom + fill_top) / 2.);

  std::vector<ChVector<float>> body_points;
  if (run_mode == RUN_MODE::SETTLING) {
    body_points = utils::PDLayerSampler_BOX<float>(
        center, hdims, 2. * params.sphere_radius, 1.01);
  } else if (run_mode == RUN_MODE::TESTING) {
    body_points = loadCheckpointFile(checkpoint_file_base + ".csv");
  }

  gpu_sys.SetParticlePositions(body_points);

  gpu_sys.SetBDFixed(true);

  gpu_sys.SetKn_SPH2SPH(params.normalStiffS2S);
  gpu_sys.SetKn_SPH2WALL(params.normalStiffS2W);
  gpu_sys.SetKn_SPH2MESH(params.normalStiffS2M);

  gpu_sys.SetGn_SPH2SPH(params.normalDampS2S);
  gpu_sys.SetGn_SPH2WALL(params.normalDampS2W);
  gpu_sys.SetGn_SPH2MESH(params.normalDampS2M);

  gpu_sys.SetKt_SPH2SPH(params.tangentStiffS2S);
  gpu_sys.SetKt_SPH2WALL(params.tangentStiffS2W);
  gpu_sys.SetKt_SPH2MESH(params.tangentStiffS2M);

  gpu_sys.SetGt_SPH2SPH(params.tangentDampS2S);
  gpu_sys.SetGt_SPH2WALL(params.tangentDampS2W);
  gpu_sys.SetGt_SPH2MESH(params.tangentDampS2M);

  gpu_sys.SetCohesionRatio(params.cohesion_ratio);
  gpu_sys.SetAdhesionRatio_SPH2MESH(params.adhesion_ratio_s2m);
  gpu_sys.SetAdhesionRatio_SPH2WALL(params.adhesion_ratio_s2w);
  gpu_sys.SetGravitationalAcceleration(ChVector<>(Gx, Gy, Gz));

  gpu_sys.SetFixedStepSize(params.step_size);
  gpu_sys.SetFrictionMode(CHGPU_FRICTION_MODE::MULTI_STEP);
  gpu_sys.SetTimeIntegrator(CHGPU_TIME_INTEGRATOR::CENTERED_DIFFERENCE);
  gpu_sys.SetStaticFrictionCoeff_SPH2SPH(params.static_friction_coeffS2S);
  gpu_sys.SetStaticFrictionCoeff_SPH2WALL(params.static_friction_coeffS2W);
  gpu_sys.SetStaticFrictionCoeff_SPH2MESH(params.static_friction_coeffS2M);

  // Create rigid wheel simulation
  ChSystemNSC rover_sys;

  // rover_sys.SetMaxItersSolverSpeed(200);

  // rover_sys.SetContactForceModel(ChSystemNSC::ContactForceModel::Hooke);
  // rover_sys.SetTimestepperType(ChTimestepper::Type::EULER_EXPLICIT);
  rover_sys.Set_G_acc(ChVector<>(Gx, Gy, Gz));

  std::shared_ptr<ChBody> chassis_body(rover_sys.NewBody());

  double height_offset_chassis_to_bottom =
      std::abs(wheel_offset_z) + 2 * wheel_rad; // TODO
  double init_offset_x = -params.box_X / 4;
  // start well above the terrain
  terrain_height_offset = params.box_Z + height_offset_chassis_to_bottom;

  bool chassis_fixed = true;
  chassis_body->SetMass(chassis_mass);
  // assume it's a solid box inertially
  chassis_body->SetInertiaXX(ChVector<>((chassis_length_y * chassis_length_y +
                                         chassis_length_z * chassis_length_z) *
                                            chassis_mass / 12,
                                        (chassis_length_x * chassis_length_x +
                                         chassis_length_z * chassis_length_z) *
                                            chassis_mass / 12,
                                        (chassis_length_x * chassis_length_x +
                                         chassis_length_y * chassis_length_y) *
                                            chassis_mass / 12));
  chassis_body->SetPos(ChVector<>(init_offset_x, 0, 0));
  rover_sys.AddBody(chassis_body);

  chassis_body->SetBodyFixed(true);

  // NOTE these must happen before the gran system loads meshes!!!
  // two wheels at front
  addWheelBody(
      rover_sys, chassis_body, wheel_filename,
      ChVector<>(front_wheel_offset_x, front_wheel_offset_y, wheel_offset_z));
  addWheelBody(
      rover_sys, chassis_body, wheel_filename,
      ChVector<>(front_wheel_offset_x, -front_wheel_offset_y, wheel_offset_z));

  // two wheels at back
  addWheelBody(
      rover_sys, chassis_body, wheel_filename,
      ChVector<>(middle_wheel_offset_x, middle_wheel_offset_y, wheel_offset_z));
  addWheelBody(rover_sys, chassis_body, wheel_filename,
               ChVector<>(middle_wheel_offset_x, -middle_wheel_offset_y,
                          wheel_offset_z));

  // two wheels in middle of chassis
  addWheelBody(
      rover_sys, chassis_body, wheel_filename,
      ChVector<>(rear_wheel_offset_x, rear_wheel_offset_y, wheel_offset_z));
  addWheelBody(
      rover_sys, chassis_body, wheel_filename,
      ChVector<>(rear_wheel_offset_x, -rear_wheel_offset_y, wheel_offset_z));

  // Load in meshes
  gpu_sys.LoadMeshes(mesh_filenames, mesh_rotscales, mesh_translations,
                     mesh_masses);

  gpu_sys.SetOutputMode(params.write_mode);
  gpu_sys.SetVerbosity(params.verbose);

  // change the output directory
  std::string out_dir = "../";
  filesystem::create_directory(filesystem::path(out_dir));
  out_dir = out_dir + params.output_dir;
  filesystem::create_directory(filesystem::path(out_dir));

  unsigned int nSoupFamilies = gpu_sys.GetNumMeshes();
  std::cout << nSoupFamilies << " soup families" << std::endl;

  gpu_sys.Initialize();

  std::cout << "Rendering at " << out_fps << "FPS" << std::endl;

  unsigned int out_steps = 1 / (out_fps * iteration_step);

  int currframe = 0;
  unsigned int curr_step = 0;

  if (run_mode == RUN_MODE::SETTLING) {
    gpu_sys.EnableMeshCollision(false);
    params.time_end = time_settling;
  } else if (run_mode == RUN_MODE::TESTING) {
    gpu_sys.EnableMeshCollision(true);
    params.time_end = time_running;
  }

  printf("Chassis mass: %f g, each wheel mass: %f g\n", chassis_mass,
         wheel_mass);
  printf("Total Chassis Mars weight in CGS: %f\n",
         std::abs((chassis_mass + 4 * wheel_mass) * mars_grav_mag));

  clock_t start = std::clock();
  for (float t = 0; t < params.time_end; t += iteration_step, curr_step++) {
    if (chassis_fixed && t >= 0.5) {
      printf("Setting wheel free!\n");
      chassis_fixed = false;
      chassis_body->SetBodyFixed(false);
      float max_terrain_z = gpu_sys.GetMaxParticleZ();
      printf("terrain max is %f\n", max_terrain_z);
      // put terrain just below bottom of wheels
      terrain_height_offset = max_terrain_z + height_offset_chassis_to_bottom;
    }
    for (unsigned int i = 0; i < wheel_bodies.size(); i++) {
      auto curr_body = wheel_bodies.at(i);

      gpu_sys.ApplyMeshMotion(i, curr_body->GetPos(), curr_body->GetRot(),
                              curr_body->GetPos_dt(), curr_body->GetWvel_par());
    }

    gpu_sys.AdvanceSimulation(iteration_step);
    rover_sys.DoStepDynamics(iteration_step);

    ChVector<> wheel_force;
    ChVector<> wheel_torque;
    for (unsigned int i = 0; i < wheel_bodies.size(); i++) {
      auto curr_body = wheel_bodies.at(i);

      gpu_sys.CollectMeshContactForces(i, wheel_force, wheel_torque);

      curr_body->Empty_forces_accumulators();
      curr_body->Accumulate_force(wheel_force, curr_body->GetPos(), false);
      curr_body->Accumulate_torque(wheel_torque, false);
    }

    if (curr_step % out_steps == 0) {
      std::cout << "Rendering frame " << currframe << std::endl;
      printf("Wheel forces: %f, %f, %f\n", wheel_force.x(), wheel_force.y(),
             wheel_force.z());
      printf("Wheel torques: %f, %f, %f\n", wheel_torque.x(), wheel_torque.y(),
             wheel_torque.z());
      char filename[100];
      sprintf(filename, "%s/step%06d", params.output_dir.c_str(), currframe++);
      gpu_sys.WriteFile(std::string(filename));
      std::string mesh_output = std::string(filename) + "_meshframes.csv";
      std::ofstream meshfile(mesh_output);
      std::ostringstream outstream;
      outstream << "mesh_name,dx,dy,dz,x1,x2,x3,y1,y2,y3,z1,z2,z3,sx,sy,sz\n";
      // if the wheel is free, output its mesh, otherwise leave file empty
      // if (!wheel_fixed) {
      for (unsigned int i = 0; i < wheel_bodies.size(); i++) {
        writeMeshFrames(outstream, wheel_bodies.at(i), wheel_filename,
                        wheel_scaling);
      }

      writeMeshFrames(outstream, chassis_body, chassis_filename,
                      {METERS_TO_CM, METERS_TO_CM, METERS_TO_CM});

      meshfile << outstream.str();
      // }
    }
  }

  if (run_mode == RUN_MODE::SETTLING) {
    gpu_sys.SetOutputMode(CHGPU_OUTPUT_MODE::CSV);

    gpu_sys.WriteFile(checkpoint_file_base);
  }

  clock_t end = std::clock();
  double total_time = ((double)(end - start)) / CLOCKS_PER_SEC;

  std::cout << "Time: " << total_time << " seconds" << std::endl;

  return 0;
}
