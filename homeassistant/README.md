# Home Assistant — treadmill fitness add-ons

Companion HA config for the ESP32-S3 iFit treadmill bridge (`../firmware`). The
firmware publishes live telemetry over MQTT with Home Assistant discovery; these
files add **weight-aware calories** and a **Fitness dashboard** on top.

<p align="center">
  <img src="images/dashboard-run.jpg" alt="Fitness dashboard — Run view, live" width="320">
</p>

*Live "Run" view: speed / incline / burn gauges, session stats (distance, pace,
calories from real body weight), and the workout speed profile — streaming
treadmill → ESP32-S3 → MQTT → Home Assistant.*

## Files
- `templates/treadmill_calories.yaml` — template sensors: `treadmill_burn_rate`
  (kcal/h, ACSM model), `treadmill_pace` (min/mi), `treadmill_energy_today` (kcal).
- `dashboards/fitness.yaml` — 4-view dashboard (Run / Body / Trends / Suite).

## One thing to set: your weight entity
Both files have a placeholder **`sensor.YOUR_BODY_WEIGHT`** — replace it with your
body-weight entity (e.g. from a smart scale). kg or lb both work; the unit is
auto-detected. In the dashboard's *Body* view, optional body-composition rows are
commented out — swap in your scale's entities or delete them.

## Install
1. Copy `templates/treadmill_calories.yaml` into your HA `config/templates/` dir
   (config must include `template: !include_dir_merge_list templates/`), edit the
   weight placeholder, then **Developer Tools → YAML → Template entities → Reload**.
2. Dashboard: **Settings → Dashboards → Add → New from scratch →** open → ✏️ → ⋮ →
   **Raw configuration editor →** paste `dashboards/fitness.yaml` → Save.
   (Or register it as a YAML dashboard via `lovelace: dashboards:` in configuration.yaml.)

## Entities expected from the firmware (MQTT discovery, device "iFit Treadmill")
| Entity | Unit |
|--------|------|
| `sensor.ifit_treadmill_speed` | mph |
| `sensor.ifit_treadmill_incline` | % |
| `sensor.ifit_treadmill_elapsed` | s |
| `sensor.ifit_treadmill_distance` | mi |
| `binary_sensor.ifit_treadmill_moving` | — |
| `binary_sensor.ifit_treadmill_connectivity` | — |

## Calorie model
ACSM walking/running metabolic equations → VO₂ → kcal, scaled by your live weight:
`kcal/min = VO₂(ml/kg/min) × weight_kg / 1000 × 5`, integrated over time. Because it
uses your real weight (not the treadmill's default-weight guess), it's accurate and
suitable as the "active energy" for an Apple Health / Health Connect workout.
