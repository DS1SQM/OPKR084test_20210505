#include <iostream>
#include <stdio.h>
#include <cmath>
#include <unistd.h>
#include <assert.h>

#include "common/util.h"
#include "common/swaglog.h"
#include "common/visionimg.h"
#include "ui.hpp"
#include "paint.hpp"
#include "dashcam.h"

// Projects a point in car to space to the corresponding point in full frame
// image space.
static bool calib_frame_to_full_frame(const UIState *s, float in_x, float in_y, float in_z, vertex_data *out) {
  const float margin = 500.0f;
  const vec3 pt = (vec3){{in_x, in_y, in_z}};
  const vec3 Ep = matvecmul3(s->scene.view_from_calib, pt);
  const vec3 KEp = matvecmul3(s->wide_camera ? ecam_intrinsic_matrix : fcam_intrinsic_matrix, Ep);

  // Project.
  float x = KEp.v[0] / KEp.v[2];
  float y = KEp.v[1] / KEp.v[2];

  nvgTransformPoint(&out->x, &out->y, s->car_space_transform, x, y);
  return out->x >= -margin && out->x <= s->fb_w + margin && out->y >= -margin && out->y <= s->fb_h + margin;
}

static void ui_init_vision(UIState *s) {
  // Invisible until we receive a calibration message.
  s->scene.world_objects_visible = false;

  for (int i = 0; i < s->vipc_client->num_buffers; i++) {
    s->texture[i].reset(new EGLImageTexture(&s->vipc_client->buffers[i]));

    glBindTexture(GL_TEXTURE_2D, s->texture[i]->frame_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

    // BGR
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
  }
  assert(glGetError() == GL_NO_ERROR);
  s->scene.recording = false;
  s->scene.touched = false;
  s->scene.map_on_top = false;
}


void ui_init(UIState *s) {
  s->sm = new SubMaster({
    "modelV2", "controlsState", "liveCalibration", "radarState", "deviceState", "liveLocationKalman",
    "pandaState", "carParams", "driverState", "driverMonitoringState", "sensorEvents", "carState", "ubloxGnss", "gpsLocationExternal", "liveParameters", "lateralPlan",
#ifdef QCOM2
    "roadCameraState",
#endif
  });

  s->scene.started = false;
  s->status = STATUS_OFFROAD;


  s->last_frame = nullptr;
  s->wide_camera = false;

#ifdef QCOM2
  s->wide_camera = Params().getBool("EnableWideCamera");
#endif

  s->setbtn_count = 0;
  s->homebtn_count = 0;

  s->scene.satelliteCount = -1;

  Params params;
  s->nOpkrAutoScreenDimming = params.getBool("OpkrAutoScreenDimming");
  s->nOpkrUIBrightness = (int)params.get("OpkrUIBrightness");
  s->nOpkrUIVolumeBoost = (int)params.get("OpkrUIVolumeBoost");
  s->nDebugUi1 = params.getBool("DebugUi1");
  s->nDebugUi2 = params.getBool("DebugUi2");
  s->nOpkrBlindSpotDetect = params.getBool("OpkrBlindSpotDetect");
  s->lat_control = (int)params.get("LateralControlMethod");
  s->driving_record = params.getBool("OpkrDrivingRecord");
  s->speed_lim_off = (int)params.get("OpkrSpeedLimitOffset");
  s->scene.laneless_mode = (int)params.get("LanelessMode");
  params.put("LimitSetSpeedCamera", "0", 1);

  ui_nvg_init(s);

  s->vipc_client_rear = new VisionIpcClient("camerad", s->wide_camera ? VISION_STREAM_RGB_WIDE : VISION_STREAM_RGB_BACK, true);
  s->vipc_client_front = new VisionIpcClient("camerad", VISION_STREAM_RGB_FRONT, true);
  s->vipc_client = s->vipc_client_rear;
}

static int get_path_length_idx(const cereal::ModelDataV2::XYZTData::Reader &line, const float path_height) {
  const auto line_x = line.getX();
  int max_idx = 0;
  for (int i = 0; i < TRAJECTORY_SIZE && line_x[i] < path_height; ++i) {
    max_idx = i;
  }
  return max_idx;
}

static void update_leads(UIState *s, const cereal::RadarState::Reader &radar_state, std::optional<cereal::ModelDataV2::XYZTData::Reader> line) {
  for (int i = 0; i < 2; ++i) {
    auto lead_data = (i == 0) ? radar_state.getLeadOne() : radar_state.getLeadTwo();
    if (lead_data.getStatus()) {
      float z = line ? (*line).getZ()[get_path_length_idx(*line, lead_data.getDRel())] : 0.0;
      // negative because radarState uses left positive convention
      calib_frame_to_full_frame(s, lead_data.getDRel(), -lead_data.getYRel(), z + 1.22, &s->scene.lead_vertices[i]);
    }
    s->scene.lead_data[i] = lead_data;
  }
}

static void update_line_data(const UIState *s, const cereal::ModelDataV2::XYZTData::Reader &line,
                             float y_off, float z_off, line_vertices_data *pvd, int max_idx) {
  const auto line_x = line.getX(), line_y = line.getY(), line_z = line.getZ();
  vertex_data *v = &pvd->v[0];
  for (int i = 0; i <= max_idx; i++) {
    v += calib_frame_to_full_frame(s, line_x[i], line_y[i] - y_off, line_z[i] + z_off, v);
  }
  for (int i = max_idx; i >= 0; i--) {
    v += calib_frame_to_full_frame(s, line_x[i], line_y[i] + y_off, line_z[i] + z_off, v);
  }
  pvd->cnt = v - pvd->v;
  assert(pvd->cnt < std::size(pvd->v));
}

static void update_model(UIState *s, const cereal::ModelDataV2::Reader &model) {
  UIScene &scene = s->scene;
  auto model_position = model.getPosition();
  float max_distance = std::clamp(model_position.getX()[TRAJECTORY_SIZE - 1],
                                  MIN_DRAW_DISTANCE, MAX_DRAW_DISTANCE);

  // update lane lines
  const auto lane_lines = model.getLaneLines();
  const auto lane_line_probs = model.getLaneLineProbs();
  int max_idx = get_path_length_idx(lane_lines[0], max_distance);
  for (int i = 0; i < std::size(scene.lane_line_vertices); i++) {
    scene.lane_line_probs[i] = lane_line_probs[i];
    update_line_data(s, lane_lines[i], 0.025 * scene.lane_line_probs[i], 0, &scene.lane_line_vertices[i], max_idx);
  }

  // update road edges
  const auto road_edges = model.getRoadEdges();
  const auto road_edge_stds = model.getRoadEdgeStds();
  for (int i = 0; i < std::size(scene.road_edge_vertices); i++) {
    scene.road_edge_stds[i] = road_edge_stds[i];
    update_line_data(s, road_edges[i], 0.025, 0, &scene.road_edge_vertices[i], max_idx);
  }

  // update path
  if (scene.lead_data[0].getStatus()) {
    const float lead_d = scene.lead_data[0].getDRel() * 2.;
    max_distance = std::clamp((float)(lead_d - fmin(lead_d * 0.35, 10.)), 0.0f, max_distance);
  }
  max_idx = get_path_length_idx(model_position, max_distance);
  update_line_data(s, model_position, 0.7, 1.22, &scene.track_vertices, max_idx);
}

static void update_sockets(UIState *s) {
  SubMaster &sm = *(s->sm);
  if (sm.update(0) == 0) return;

  UIScene &scene = s->scene;
  if (scene.started && sm.updated("controlsState")) {
    scene.controls_state = sm["controlsState"].getControlsState();
    scene.lateralControlMethod = scene.controls_state.getLateralControlMethod();
    if (scene.lateralControlMethod == 0) {
      scene.output_scale = scene.controls_state.getLateralControlState().getPidState().getOutput();
    } else if (scene.lateralControlMethod == 1) {
      scene.output_scale = scene.controls_state.getLateralControlState().getIndiState().getOutput();
    } else if (s->scene.lateralControlMethod == 2) {
      scene.output_scale = scene.controls_state.getLateralControlState().getLqrState().getOutput();
    }
    scene.angleSteersDes = scene.controls_state.getSteeringAngleDesiredDeg();

    scene.alertTextMsg1 = scene.controls_state.getAlertTextMsg1(); //debug1
    scene.alertTextMsg2 = scene.controls_state.getAlertTextMsg2(); //debug2

    scene.limitSpeedCamera = scene.controls_state.getLimitSpeedCamera();
    scene.limitSpeedCameraDist = scene.controls_state.getLimitSpeedCameraDist();
    scene.steerRatio = scene.controls_state.getSteerRatio();
  }
  if (sm.updated("carState")) {
    scene.car_state = sm["carState"].getCarState();
    auto data = sm["carState"].getCarState();
    if(scene.leftBlinker!=data.getLeftBlinker() || scene.rightBlinker!=data.getRightBlinker()){
      scene.blinker_blinkingrate = 120;
    }
    scene.brakePress = data.getBrakePressed();
    scene.brakeLights = data.getBrakeLights();
    scene.getGearShifter = data.getGearShifter();
    scene.leftBlinker = data.getLeftBlinker();
    scene.rightBlinker = data.getRightBlinker();
    scene.leftblindspot = data.getLeftBlindspot();
    scene.rightblindspot = data.getRightBlindspot();
    scene.tpmsPressureFl = data.getTpmsPressureFl();
    scene.tpmsPressureFr = data.getTpmsPressureFr();
    scene.tpmsPressureRl = data.getTpmsPressureRl();
    scene.tpmsPressureRr = data.getTpmsPressureRr();
    scene.radarDistance = data.getRadarDistance();
    scene.standStill = data.getStandStill();
    scene.vSetDis = data.getVSetDis();
    scene.cruiseAccStatus = data.getCruiseAccStatus();
    scene.angleSteers = data.getSteeringAngleDeg();
  }

  if (sm.updated("liveParameters")) {
    //scene.liveParams = sm["liveParameters"].getLiveParameters();
    auto data = sm["liveParameters"].getLiveParameters();
    scene.liveParams.angleOffset = data.getAngleOffsetDeg();
    scene.liveParams.angleOffsetAverage = data.getAngleOffsetAverageDeg();
    scene.liveParams.stiffnessFactor = data.getStiffnessFactor();
    scene.liveParams.steerRatio = data.getSteerRatio();
  }
  if (sm.updated("radarState")) {
    std::optional<cereal::ModelDataV2::XYZTData::Reader> line;
    if (sm.rcv_frame("modelV2") > 0) {
      line = sm["modelV2"].getModelV2().getPosition();
    }
    update_leads(s, sm["radarState"].getRadarState(), line);
  }
  if (sm.updated("liveCalibration")) {
    scene.world_objects_visible = true;
    auto rpy_list = sm["liveCalibration"].getLiveCalibration().getRpyCalib();
    Eigen::Vector3d rpy;
    rpy << rpy_list[0], rpy_list[1], rpy_list[2];
    Eigen::Matrix3d device_from_calib = euler2rot(rpy);
    Eigen::Matrix3d view_from_device;
    view_from_device << 0,1,0,
                        0,0,1,
                        1,0,0;
    Eigen::Matrix3d view_from_calib = view_from_device * device_from_calib;
    for (int i = 0; i < 3; i++) {
      for (int j = 0; j < 3; j++) {
        scene.view_from_calib.v[i*3 + j] = view_from_calib(i,j);
      }
    }
  }
  if (sm.updated("modelV2")) {
    update_model(s, sm["modelV2"].getModelV2());
  }
  if (sm.updated("deviceState")) {
    scene.deviceState = sm["deviceState"].getDeviceState();
    s->scene.cpuPerc = scene.deviceState.getCpuUsagePercent();
    s->scene.cpuTemp = scene.deviceState.getCpuTempC()[0];
    s->scene.fanSpeed = scene.deviceState.getFanSpeedPercentDesired();
    auto data = sm["deviceState"].getDeviceState();
    snprintf(scene.ipAddr, sizeof(scene.ipAddr), "%s", data.getIpAddr().cStr());
  }
  if (sm.updated("pandaState")) {
    auto pandaState = sm["pandaState"].getPandaState();
    scene.pandaType = pandaState.getPandaType();
    scene.ignition = pandaState.getIgnitionLine() || pandaState.getIgnitionCan();
  } else if ((s->sm->frame - s->sm->rcv_frame("pandaState")) > 5*UI_FREQ) {
    scene.pandaType = cereal::PandaState::PandaType::UNKNOWN;
  }
  if (sm.updated("ubloxGnss")) {
    auto data = sm["ubloxGnss"].getUbloxGnss();
    if (data.which() == cereal::UbloxGnss::MEASUREMENT_REPORT) {
      scene.satelliteCount = data.getMeasurementReport().getNumMeas();
    }
    auto data2 = sm["gpsLocationExternal"].getGpsLocationExternal();
      scene.gpsAccuracyUblox = data2.getAccuracy();
      scene.altitudeUblox = data2.getAltitude();
      scene.bearingUblox = data2.getBearingDeg();
  }
  if (sm.updated("liveLocationKalman")) {
    scene.gpsOK = sm["liveLocationKalman"].getLiveLocationKalman().getGpsOK();
  }
  if (sm.updated("carParams")) {
    scene.longitudinal_control = sm["carParams"].getCarParams().getOpenpilotLongitudinalControl();
  }
  if (sm.updated("driverState")) {
    scene.driver_state = sm["driverState"].getDriverState();
  }
  if (sm.updated("driverMonitoringState")) {
    scene.dmonitoring_state = sm["driverMonitoringState"].getDriverMonitoringState();
  }
  if (sm.updated("sensorEvents")) {
    for (auto sensor : sm["sensorEvents"].getSensorEvents()) {
      if (sensor.which() == cereal::SensorEventData::LIGHT) {
#ifndef QCOM2
        scene.light_sensor = sensor.getLight();
#endif
      } else if (!scene.started && sensor.which() == cereal::SensorEventData::ACCELERATION) {
        auto accel = sensor.getAcceleration().getV();
        if (accel.totalSize().wordCount){ // TODO: sometimes empty lists are received. Figure out why
          scene.accel_sensor = accel[2];
        }
      } else if (!scene.started && sensor.which() == cereal::SensorEventData::GYRO_UNCALIBRATED) {
        auto gyro = sensor.getGyroUncalibrated().getV();
        if (gyro.totalSize().wordCount){
          scene.gyro_sensor = gyro[1];
        }
      }
    }
  }
#ifdef QCOM2
  if (sm.updated("roadCameraState")) {
    auto camera_state = sm["roadCameraState"].getRoadCameraState();
    float gain = camera_state.getGainFrac() * (camera_state.getGlobalGain() > 100 ? 2.5 : 1.0) / 10.0;
    scene.light_sensor = std::clamp<float>((1023.0 / 1757.0) * (1757.0 - camera_state.getIntegLines()) * (1.0 - gain), 0.0, 1023.0);
  }
#endif
  scene.started = scene.deviceState.getStarted() || scene.driver_view;

  if (sm.updated("lateralPlan")) {
    scene.lateral_plan = sm["lateralPlan"].getLateralPlan();
    auto data = sm["lateralPlan"].getLateralPlan();

    scene.lateralPlan.laneWidth = data.getLaneWidth();
    scene.lateralPlan.dProb = data.getDProb();
    scene.lateralPlan.lProb = data.getLProb();
    scene.lateralPlan.rProb = data.getRProb();
    scene.lateralPlan.steerRateCost = data.getSteerRateCost();
    scene.lateralPlan.standstillElapsedTime = data.getStandstillElapsedTime();
    scene.lateralPlan.lanelessModeStatus = data.getLanelessMode();
  }
}

static void update_alert(UIState *s) {
  UIScene &scene = s->scene;
  if (s->sm->updated("controlsState")) {
    auto alert_sound = scene.controls_state.getAlertSound();
    if (scene.alert_type.compare(scene.controls_state.getAlertType()) != 0) {
      if (alert_sound == AudibleAlert::NONE) {
        s->sound->stop();
      } else {
        s->sound->play(alert_sound);
      }
    }
    scene.alert_text1 = scene.controls_state.getAlertText1();
    scene.alert_text2 = scene.controls_state.getAlertText2();
    scene.alert_size = scene.controls_state.getAlertSize();
    scene.alert_type = scene.controls_state.getAlertType();
    scene.alert_blinking_rate = scene.controls_state.getAlertBlinkingRate();
  }

  // Handle controls timeout
  if (scene.deviceState.getStarted() && (s->sm->frame - scene.started_frame) > 10 * UI_FREQ) {
    const uint64_t cs_frame = s->sm->rcv_frame("controlsState");
    if (cs_frame < scene.started_frame) {
      // car is started, but controlsState hasn't been seen at all
      if( !s->is_OpenpilotViewEnabled ) {
        scene.alert_text1 = "openpilot Unavailable";
        scene.alert_text2 = "Waiting for controls to start";
        scene.alert_size = cereal::ControlsState::AlertSize::MID;
      }
    } else if ((s->sm->frame - cs_frame) > 5 * UI_FREQ) {
      // car is started, but controls is lagging or died
      if (scene.alert_text2 != "Controls Unresponsive") {
        s->sound->play(AudibleAlert::CHIME_WARNING_REPEAT);
        LOGE("Controls unresponsive");
      }

      scene.alert_text1 = "TAKE CONTROL IMMEDIATELY";
      scene.alert_text2 = "Controls Unresponsive";
      scene.alert_size = cereal::ControlsState::AlertSize::FULL;
      s->status = STATUS_ALERT;
    }
  }
}

static void update_params(UIState *s) {
  const uint64_t frame = s->sm->frame;
  UIScene &scene = s->scene;
  Params params;
  if (frame % (5*UI_FREQ) == 0) {
    scene.is_metric = params.getBool("IsMetric");
    s->is_OpenpilotViewEnabled = params.getBool("IsOpenpilotViewEnabled");
    s->nOpkrUIBrightness = (int)params.get("OpkrUIBrightness");
    s->nOpkrUIVolumeBoost = (int)params.get("OpkrUIVolumeBoost");
    s->lat_control = (int)params.get("LateralControlMethod");
    s->driving_record = params.getBool("OpkrDrivingRecord");
    scene.end_to_end = params.getBool("EndToEndToggle");
  } else if (frame % (6*UI_FREQ) == 0) {
    scene.athenaStatus = NET_DISCONNECTED;
    if (auto last_ping = params.get<float>("LastAthenaPingTime"); last_ping) {
      scene.athenaStatus = nanos_since_boot() - *last_ping < 70e9 ? NET_CONNECTED : NET_ERROR;
    }
  }
}

static void update_vision(UIState *s) {
  if (!s->vipc_client->connected && s->scene.started) {
    if (s->vipc_client->connect(false)){
      ui_init_vision(s);
    }
  }

  if (s->vipc_client->connected){
    VisionBuf * buf = s->vipc_client->recv();
    if (buf != nullptr){
      s->last_frame = buf;
    } else {
#if defined(QCOM) || defined(QCOM2)
      LOGE("visionIPC receive timeout");
#endif
    }
  }
}

static void update_status(UIState *s) {
  if (s->scene.started && s->sm->updated("controlsState")) {
    auto alert_status = s->scene.controls_state.getAlertStatus();
    if (alert_status == cereal::ControlsState::AlertStatus::USER_PROMPT) {
      s->status = STATUS_WARNING;
    } else if (alert_status == cereal::ControlsState::AlertStatus::CRITICAL) {
      s->status = STATUS_ALERT;
    } else {
      s->status = s->scene.controls_state.getEnabled() ? STATUS_ENGAGED : STATUS_DISENGAGED;
    }
  }

  // Handle onroad/offroad transition
  static bool started_prev = false;
  if (s->scene.started != started_prev) {
    if (s->scene.started) {
      s->status = STATUS_DISENGAGED;
      s->scene.started_frame = s->sm->frame;

      s->scene.is_rhd = Params().getBool("IsRHD");
      s->sidebar_collapsed = true;
      s->scene.alert_size = cereal::ControlsState::AlertSize::NONE;
      s->vipc_client = s->scene.driver_view ? s->vipc_client_front : s->vipc_client_rear;
    } else {
      s->status = STATUS_OFFROAD;
      s->sidebar_collapsed = false;
      s->sound->stop();
      s->vipc_client->connected = false;
    }
  }
  started_prev = s->scene.started;
}

void ui_update(UIState *s) {
  update_params(s);
  update_sockets(s);
  update_status(s);
  update_alert(s);
  update_vision(s);
  dashcam(s);
}
