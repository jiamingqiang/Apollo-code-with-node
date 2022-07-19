/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

/**
 * @file
 **/

#include "modules/planning/tasks/optimizers/road_graph/trajectory_cost.h"

#include "modules/common/configs/vehicle_config_helper.h"
#include "modules/common/math/vec2d.h"
#include "modules/common/proto/pnc_point.pb.h"
#include "modules/common/util/point_factory.h"
#include "modules/planning/common/planning_gflags.h"

namespace apollo {
namespace planning {

TrajectoryCost::TrajectoryCost(const DpPolyPathConfig &config,
                               const ReferenceLine &reference_line,
                               const bool is_change_lane_path,
                               const std::vector<const Obstacle *> &obstacles,
                               const common::VehicleParam &vehicle_param,
                               const SpeedData &heuristic_speed_data,
                               const common::SLPoint &init_sl_point,
                               const SLBoundary &adc_sl_boundary)
    : config_(config),
      reference_line_(&reference_line),
      is_change_lane_path_(is_change_lane_path),
      vehicle_param_(vehicle_param),
      heuristic_speed_data_(heuristic_speed_data),
      init_sl_point_(init_sl_point),
      adc_sl_boundary_(adc_sl_boundary) {
  const double total_time =
      std::min(heuristic_speed_data_.TotalTime(), FLAGS_prediction_total_time);

  num_of_time_stamps_ = static_cast<uint32_t>(
      std::floor(total_time / config.eval_time_interval()));

  for (const auto *ptr_obstacle : obstacles) {
    if (ptr_obstacle->IsIgnore()) {
      continue;
    } else if (ptr_obstacle->LongitudinalDecision().has_stop()) {
      continue;
    }
    const auto &sl_boundary  = ptr_obstacle->PerceptionSLBoundary();  //障碍物的sl边界；
    //自车左边界
    const double adc_left_l  = init_sl_point_.l() + vehicle_param_.left_edge_to_center();
    //自车右边界
    const double adc_right_l = init_sl_point_.l() - vehicle_param_.right_edge_to_center();

    if (adc_left_l + FLAGS_lateral_ignore_buffer < sl_boundary.start_l() ||
        adc_right_l - FLAGS_lateral_ignore_buffer > sl_boundary.end_l()) {
      continue;     //撞不到的障碍物直接忽略掉
    }

    bool is_bycycle_or_pedestrian =
        (ptr_obstacle->Perception().type() ==
             perception::PerceptionObstacle::BICYCLE ||
         ptr_obstacle->Perception().type() ==
             perception::PerceptionObstacle::PEDESTRIAN);

    if (ptr_obstacle->IsVirtual()) {
      // Virtual obstacle
      continue;
    } else if (ptr_obstacle->IsStatic() || is_bycycle_or_pedestrian) {
      // 静态障碍物
      static_obstacle_sl_boundaries_.push_back(std::move(sl_boundary));  //行人或者静态障碍物存入边界值
    } else {
      // 依据时间得到动态障碍物
      std::vector<Box2d> box_by_time;
      //每一动态障碍物依据时间存入 box_by_time
      for (uint32_t t = 0; t <= num_of_time_stamps_; ++t) {
        TrajectoryPoint trajectory_point =
            ptr_obstacle->GetPointAtTime(t * config.eval_time_interval());  //障碍物的轨迹根据时间进行插值

        Box2d obstacle_box = ptr_obstacle->GetBoundingBox(trajectory_point);  //得到障碍物的边界框信息包括长宽，位姿等。
        static constexpr double kBuff = 0.5;
        Box2d expanded_obstacle_box =
            Box2d(obstacle_box.center(), obstacle_box.heading(),
                  obstacle_box.length() + kBuff, obstacle_box.width() + kBuff);  //对障碍物的边界框进行扩展膨胀
        box_by_time.push_back(expanded_obstacle_box);
        //对于每一个动态障碍物，根据总时间和设置的时间间隔把动态障碍物的运动轨迹分散成一份一份的，对动态障碍物的边界框进行膨胀，完事后放入一个二维数组里，行是每个障碍物，列是这个障碍物一定时间内离散的膨胀的边界框轨迹
      }
      // 每一动态障碍物序列压入
      dynamic_obstacle_boxes_.push_back(std::move(box_by_time));
    }
  }  //这是对障碍物的一个处理
}



ComparableCost TrajectoryCost::CalculatePathCost(
    const QuinticPolynomialCurve1d &curve, const double start_s,
    const double end_s, const uint32_t curr_level, const uint32_t total_level) {
  ComparableCost cost;
  double path_cost = 0.0;
  std::function<double(const double)> quasi_softmax = [this](const double x) { //[this] 捕获当前类中的
  // this 指针，让 lambda 表达式拥有和当前类成员函数同样的访问权限。
  //如果已经使用了 & 或者 =，就默认添加此选项。捕获 this 的目的是可以在 lamda 
  //中使用当前类的成员函数和成员变量
    const double l0 = this->config_.path_l_cost_param_l0();
    const double b = this->config_.path_l_cost_param_b();
    const double k = this->config_.path_l_cost_param_k();
    return (b + std::exp(-k * (x - l0))) / (1.0 + std::exp(-k * (x - l0)));
  };

  for (double curve_s = 0.0; curve_s < (end_s - start_s);
       curve_s += config_.path_resolution()) {
    const double l = curve.Evaluate(0, curve_s);

    // 0阶导的cost
    path_cost += l * l * config_.path_l_cost() * quasi_softmax(std::fabs(l));

    //一阶导的cost
    const double dl = std::fabs(curve.Evaluate(1, curve_s));
    if (IsOffRoad(curve_s + start_s, l, dl, is_change_lane_path_)) {
      cost.cost_items[ComparableCost::OUT_OF_BOUNDARY] = true;
    }

    path_cost += dl * dl * config_.path_dl_cost();

    //二阶导的cost
    const double ddl = std::fabs(curve.Evaluate(2, curve_s));
    path_cost += ddl * ddl * config_.path_ddl_cost();
  }
  path_cost *= config_.path_resolution();
  // 最后一层，距离车道中心线的cost,就是说还是希望轨迹的最后再回到车道中心线上
  if (curr_level == total_level) {
    const double end_l = curve.Evaluate(0, end_s - start_s);
    path_cost +=
        std::sqrt(end_l - init_sl_point_.l() / 2.0) * config_.path_end_l_cost();
  }
  cost.smoothness_cost = path_cost;  // 上面计算的这些cost 都是“平滑指标”，即smoothness_cost
  return cost;
}



bool TrajectoryCost::IsOffRoad(const double ref_s, const double l,
                               const double dl,
                               const bool is_change_lane_path) {
  static constexpr double kIgnoreDistance = 5.0;
  if (ref_s - init_sl_point_.s() < kIgnoreDistance) {
    return false;
  }
  Vec2d rear_center(0.0, l);

  const auto &param = common::VehicleConfigHelper::GetConfig().vehicle_param();
  Vec2d vec_to_center(
      (param.front_edge_to_center() - param.back_edge_to_center()) / 2.0,
      (param.left_edge_to_center() - param.right_edge_to_center()) / 2.0);

  Vec2d rear_center_to_center = vec_to_center.rotate(std::atan(dl));
  Vec2d center = rear_center + rear_center_to_center;
  Vec2d front_center = center + rear_center_to_center;

  const double buffer = 0.1;  // in meters
  const double r_w =
      (param.left_edge_to_center() + param.right_edge_to_center()) / 2.0;
  const double r_l = param.back_edge_to_center();
  const double r = std::sqrt(r_w * r_w + r_l * r_l);

  double left_width = 0.0;
  double right_width = 0.0;
  reference_line_->GetLaneWidth(ref_s, &left_width, &right_width);

  double left_bound = std::max(init_sl_point_.l() + r + buffer, left_width);
  double right_bound = std::min(init_sl_point_.l() - r - buffer, -right_width);
  if (rear_center.y() + r + buffer / 2.0 > left_bound ||
      rear_center.y() - r - buffer / 2.0 < right_bound) {
    return true;
  }
  if (front_center.y() + r + buffer / 2.0 > left_bound ||
      front_center.y() - r - buffer / 2.0 < right_bound) {
    return true;
  }

  return false;
}




// 从ego的 start_s 到ens_s计算 所有静态障碍物的cost
// 其实就是以某一分辨率沿着curve走一遍，在离static obstacle比较近时，算一个cost，其他的都对cost没贡献
ComparableCost TrajectoryCost::CalculateStaticObstacleCost(
    const QuinticPolynomialCurve1d &curve, const double start_s,
    const double end_s) {
  ComparableCost obstacle_cost;
  //从ego的 start_s 到ens_s计算 所有静态障碍物的cost
  for (double curr_s = start_s; curr_s <= end_s;
       curr_s += config_.path_resolution()) {
    const double curr_l = curve.Evaluate(0, curr_s - start_s); //用curr_s求出curr_l
    for (const auto &obs_sl_boundary : static_obstacle_sl_boundaries_) {
      //对于每个静态障碍物，计算当前sl下的cost
      obstacle_cost += GetCostFromObsSL(curr_s, curr_l, obs_sl_boundary);
    }
  }
  obstacle_cost.safety_cost *= config_.path_resolution(); // 计算的是“安全指标”，safety_cost
  return obstacle_cost;
}



ComparableCost TrajectoryCost::CalculateDynamicObstacleCost(
    const QuinticPolynomialCurve1d &curve, const double start_s,
    const double end_s) const {
  ComparableCost obstacle_cost;
  if (dynamic_obstacle_boxes_.empty()) {
    return obstacle_cost;
  }

  double time_stamp = 0.0;
  // for形成的两层循环
  // 外循环，依据time_stamp，拿到自车的信息，即ego_box
  // 内循环，ego_box依次与同一时刻所有的 dynamic_obstacle_boxes_ 计算cost
  for (size_t index = 0; index < num_of_time_stamps_;
       ++index, time_stamp += config_.eval_time_interval()) {
    common::SpeedPoint speed_point;
    // 从 SpeedData容器中，依据 time_stamp 返回一个speed_point，用到了线性插值
    // 见 apollo-master\modules\planning\common\speed\speed_data.cc\ SpeedData::EvaluateByTime()
    heuristic_speed_data_.EvaluateByTime(time_stamp, &speed_point);
    double ref_s = speed_point.s() + init_sl_point_.s();
    if (ref_s < start_s) {
      continue;
    }
    if (ref_s > end_s) {
      break;
    }
    const double  s = ref_s - start_s;  // s on spline curve
    const double  l = curve.Evaluate(0, s);
    const double dl = curve.Evaluate(1, s); // 对应于航向角 yaw
    const common::SLPoint sl = common::util::PointFactory::ToSLPoint(ref_s, l);
    const Box2d ego_box = GetBoxFromSLPoint(sl, dl);
    // 当前时刻下，与所有动态dynamic_obstacle的cost
    for (const auto &obstacle_trajectory : dynamic_obstacle_boxes_) {
      obstacle_cost += GetCostBetweenObsBoxes(ego_box, obstacle_trajectory.at(index));
    }
  }

  static constexpr double kDynamicObsWeight = 1e-6;
  // 也是安全性cost，即safety_cost
  obstacle_cost.safety_cost *= (config_.eval_time_interval() * kDynamicObsWeight);
  return obstacle_cost;
}



// 计算ego某一（s,l）对于某一obstacle的cost
ComparableCost TrajectoryCost::GetCostFromObsSL(
    const double adc_s, const double adc_l, const SLBoundary &obs_sl_boundary) {
  const auto &vehicle_param =
      common::VehicleConfigHelper::Instance()->GetConfig().vehicle_param();

  ComparableCost obstacle_cost;
  if (obs_sl_boundary.start_l() * obs_sl_boundary.end_l() <= 0.0) {
    return obstacle_cost;
  }

  //车身四个边界
  const double adc_front_s = adc_s + vehicle_param.front_edge_to_center();
  const double adc_end_s = adc_s - vehicle_param.back_edge_to_center();
  const double adc_left_l = adc_l + vehicle_param.left_edge_to_center();
  const double adc_right_l = adc_l - vehicle_param.right_edge_to_center();

  if (adc_left_l + FLAGS_lateral_ignore_buffer < obs_sl_boundary.start_l() ||
      adc_right_l - FLAGS_lateral_ignore_buffer > obs_sl_boundary.end_l()) {
    return obstacle_cost;
  }

  bool no_overlap = ((adc_front_s < obs_sl_boundary.start_s() ||
                      adc_end_s > obs_sl_boundary.end_s()) ||  // longitudinal
                     (adc_left_l + 0.1 < obs_sl_boundary.start_l() ||
                      adc_right_l - 0.1 > obs_sl_boundary.end_l()));  // lateral

  if (!no_overlap) {  //任何一个不成立，就撞了
    obstacle_cost.cost_items[ComparableCost::HAS_COLLISION] = true;
  }

  // if obstacle is behind ADC, ignore its cost contribution.
  if (adc_front_s > obs_sl_boundary.end_s()) {
    return obstacle_cost;
  }

  const double delta_l = std::fmax(adc_right_l - obs_sl_boundary.end_l(),
                                   obs_sl_boundary.start_l() - adc_left_l);
  /*
  AWARN << "adc_s: " << adc_s << "; adc_left_l: " << adc_left_l
        << "; adc_right_l: " << adc_right_l << "; delta_l = " << delta_l;
  AWARN << obs_sl_boundary.ShortDebugString();
  */

  static constexpr double kSafeDistance = 0.6;
  //距离小于 0.6m 才计算cost
  // 用Sigmoid函数计算距离近的静态障碍cost
  if (delta_l < kSafeDistance) {
    obstacle_cost.safety_cost +=
        config_.obstacle_collision_cost() *
        Sigmoid(config_.obstacle_collision_distance() - delta_l);
  }

  return obstacle_cost;
}



// Simple version: calculate obstacle cost by distance
// 计算两个box之间的cost，使用的也是sigmoid函数
ComparableCost TrajectoryCost::GetCostBetweenObsBoxes(
    const Box2d &ego_box, const Box2d &obstacle_box) const {
  ComparableCost obstacle_cost;

  const double distance = obstacle_box.DistanceTo(ego_box);
  if (distance > config_.obstacle_ignore_distance()) {
    return obstacle_cost;
  }

  obstacle_cost.safety_cost +=
      config_.obstacle_collision_cost() *
      Sigmoid(config_.obstacle_collision_distance() - distance);
  obstacle_cost.safety_cost +=
      20.0 * Sigmoid(config_.obstacle_risk_distance() - distance);
  return obstacle_cost;
}



// 把一个(s,l)膨胀成box
Box2d TrajectoryCost::GetBoxFromSLPoint(const common::SLPoint &sl,
                                        const double dl) const {
  Vec2d xy_point;
  reference_line_->SLToXY(sl, &xy_point);

  ReferencePoint reference_point = reference_line_->GetReferencePoint(sl.s());

  const double one_minus_kappa_r_d = 1 - reference_point.kappa() * sl.l();
  const double delta_theta = std::atan2(dl, one_minus_kappa_r_d);
  const double theta =
      common::math::NormalizeAngle(delta_theta + reference_point.heading());
  return Box2d(xy_point, theta, vehicle_param_.length(),
               vehicle_param_.width());
}



// TODO(All): optimize obstacle cost calculation time
ComparableCost TrajectoryCost::Calculate(const QuinticPolynomialCurve1d &curve,
                                         const double start_s,
                                         const double end_s,
                                         const uint32_t curr_level,
                                         const uint32_t total_level) {
  ComparableCost total_cost;
  total_cost += CalculatePathCost(curve, start_s, end_s, curr_level, total_level);
  total_cost += CalculateStaticObstacleCost( curve, start_s, end_s);
  total_cost += CalculateDynamicObstacleCost(curve, start_s, end_s);
  return total_cost;
}

}  // namespace planning
}  // namespace apollo
