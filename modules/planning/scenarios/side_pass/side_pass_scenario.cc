/******************************************************************************
 * Copyright 2018 The Apollo Authors. All Rights Reserved.
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

#include "modules/planning/scenarios/side_pass/side_pass_scenario.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "cyber/common/log.h"
#include "modules/common/math/math_utils.h"
#include "modules/common/time/time.h"
#include "modules/planning/common/frame.h"
#include "modules/planning/scenarios/side_pass/side_pass_stage.h"
#include "modules/planning/scenarios/side_pass/side_pass_stop_on_wait_point.h"

namespace apollo {
namespace planning {
namespace scenario {
namespace side_pass {

apollo::common::util::Factory<
    ScenarioConfig::StageType, Stage,
    Stage* (*)(const ScenarioConfig::StageConfig& stage_config)>
    SidePassScenario::s_stage_factory_;

void SidePassScenario::RegisterStages() {
  s_stage_factory_.Clear();
  s_stage_factory_.Register(
      ScenarioConfig::SIDE_PASS_APPROACH_OBSTACLE,
      [](const ScenarioConfig::StageConfig& config) -> Stage* {
        return new SidePassApproachObstacle(config);
      });
  s_stage_factory_.Register(
      ScenarioConfig::SIDE_PASS_DETECT_SAFETY,
      [](const ScenarioConfig::StageConfig& config) -> Stage* {
        return new SidePassDetectSafety(config);
      });
  s_stage_factory_.Register(
      ScenarioConfig::SIDE_PASS_GENERATE_PATH,
      [](const ScenarioConfig::StageConfig& config) -> Stage* {
        return new SidePassGeneratePath(config);
      });
  s_stage_factory_.Register(
      ScenarioConfig::SIDE_PASS_STOP_ON_WAITPOINT,
      [](const ScenarioConfig::StageConfig& config) -> Stage* {
        return new SidePassStopOnWaitPoint(config);
      });
  s_stage_factory_.Register(
      ScenarioConfig::SIDE_PASS_PASS_OBSTACLE,
      [](const ScenarioConfig::StageConfig& config) -> Stage* {
        return new SidePassPassObstacle(config);
      });
}

SidePassScenario::SidePassScenario(
  const ScenarioConfig& config, const ScenarioContext* scenario_context)
      : Scenario(config, scenario_context) {
    side_pass_context_.scenario_config_.CopyFrom(config.side_pass_config());
  }

std::unique_ptr<Stage> SidePassScenario::CreateStage(
    const ScenarioConfig::StageConfig& stage_config) {
  if (s_stage_factory_.Empty()) {
    RegisterStages();
  }
  auto ptr = s_stage_factory_.CreateObjectOrNull(stage_config.stage_type(),
                                                 stage_config);
  if (ptr == nullptr) {
    AERROR << "Failed to create stage for config: "
           << stage_config.DebugString();
    return nullptr;
  }
  ptr->SetContext(&side_pass_context_);
  return ptr;
}

bool SidePassScenario::IsTransferable(const Scenario& current_scenario,
                                      const common::TrajectoryPoint& ego_point,
                                      const Frame& frame) const {
  if (frame.reference_line_info().size() > 1) {
    return false;
  }
  if (current_scenario.scenario_type() == ScenarioConfig::SIDE_PASS) {
    return (current_scenario.GetStatus() !=
            Scenario::ScenarioStatus::STATUS_DONE);
  } else if (current_scenario.scenario_type() != ScenarioConfig::LANE_FOLLOW) {
    return false;
  } else {
    return IsSidePassScenario(ego_point, frame);
  }
}

bool SidePassScenario::IsSidePassScenario(
    const common::TrajectoryPoint& planning_start_point,
    const Frame& frame) const {
  const SLBoundary& adc_sl_boundary =
      frame.reference_line_info().front().AdcSlBoundary();
  const PathDecision& path_decision =
      frame.reference_line_info().front().path_decision();
  return HasBlockingObstacle(adc_sl_boundary, path_decision);
}

bool SidePassScenario::IsFarFromIntersection(const Frame& frame) {
  if (frame.reference_line_info().size() > 1) {
    return false;
  }
  const SLBoundary& adc_sl_boundary =
      frame.reference_line_info().front().AdcSlBoundary();
  const auto& first_encounters =
      frame.reference_line_info().front().FirstEncounteredOverlaps();
  const double kClearDistance = 15.0;  // in meters
  for (const auto& encounter : first_encounters) {
    if (encounter.first != ReferenceLineInfo::SIGNAL ||
        encounter.first != ReferenceLineInfo::STOP_SIGN) {
      continue;
    }
    if (encounter.second.start_s - adc_sl_boundary.end_s() < kClearDistance) {
      return false;
    }
  }
  return true;
}

bool SidePassScenario::HasBlockingObstacle(
    const SLBoundary& adc_sl_boundary,
    const PathDecision& path_decision) const {
  // a blocking obstacle is an obstacle blocks the road when it is not blocked
  // (by other obstacles or traffic rules)
  for (const auto* obstacle : path_decision.obstacles().Items()) {
    if (obstacle->IsVirtual() || !obstacle->IsStatic()) {
      continue;
    }
    CHECK(obstacle->IsStatic());

    if (obstacle->PerceptionSLBoundary().start_s() <=
        adc_sl_boundary.end_s()) {  // such vehicles are behind the ego car.
      continue;
    }
    constexpr double kAdcDistanceThreshold = 15.0;  // unit: m
    if (obstacle->PerceptionSLBoundary().start_s() >
        adc_sl_boundary.end_s() +
            kAdcDistanceThreshold) {  // vehicles are far away
      continue;
    }
    if (obstacle->PerceptionSLBoundary().start_l() > 1.0 ||
        obstacle->PerceptionSLBoundary().end_l() < -1.0) {
      continue;
    }

    bool is_blocked_by_others = false;
    for (const auto* other_obstacle : path_decision.obstacles().Items()) {
      if (other_obstacle->Id() == obstacle->Id()) {
        continue;
      }
      if (other_obstacle->PerceptionSLBoundary().start_l() >
              obstacle->PerceptionSLBoundary().end_l() ||
          other_obstacle->PerceptionSLBoundary().end_l() <
              obstacle->PerceptionSLBoundary().start_l()) {
        // not blocking the backside vehicle
        continue;
      }

      double delta_s = other_obstacle->PerceptionSLBoundary().start_s() -
                       obstacle->PerceptionSLBoundary().end_s();
      if (delta_s < 0.0 || delta_s > kAdcDistanceThreshold) {
        continue;
      } else {
        // TODO(All): fixed the segmentation bug for large vehicles, otherwise
        // the follow line will be problematic.
        // is_blocked_by_others = true; break;
      }
    }
    if (!is_blocked_by_others) {
      return true;
    }
  }
  return false;
}

}  // namespace side_pass
}  // namespace scenario
}  // namespace planning
}  // namespace apollo
