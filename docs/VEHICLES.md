# Engine vehicles: car, tank, plane, helicopter

A drivable-vehicle layer in the engine, so the same four vehicle types run in
every GoldSrc game the engine loads (Half-Life, Day of Defeat, the MOHAA
conversion), with no game DLL changes.

The handling is **Glide**'s, StyledStrike's vehicle base for Garry's Mod
([gmod-glide](https://github.com/StyledStrike/gmod-glide), MIT licence, see
the end of this file). The equations and the stock numbers are ported from
Glide's Lua, file by file, onto a small rigid body written for GoldSrc. Where
GoldSrc cannot do what Garry's Mod does, the difference is listed under
[Differences from Glide](#differences-from-glide).

| Type | Glide class | What it is |
|---|---|---|
| `car` | `base_glide_car` | Four raycast wheels with Glide's spring/damper suspension, traction circle and slip-angle side grip. A fake-flywheel engine with an RPM torque curve, a six-speed automatic gearbox and an auto clutch, a speed-sensitive steering cone with counter-steer, handbrake slides, burnouts, J-turns, air control and unflipping. Stock numbers are base_glide_car's on the Infernus layout (800 kg). |
| `tank` | `base_glide_tank` | The car engine and wheels with the tank's numbers (20 t, high-grip track wheels, one gear), front wheels steering +1 and rear -1 like Glide's Rhino, turn in place when only steering at rest, and a turret that follows the aim at Glide's slew rate, with recoil. |
| `plane` | `base_glide_plane` | Glide's `SimulatePlane`: lift that grows with the square of airspeed and takes away sink across the wings, drag, weathervaning toward the flight path, control surfaces that bite with airspeed, an engine whose power spools between 0 and 2, and landing gear on the wheels (the Stunt plane's tail dragger). |
| `heli` | `base_glide_heli` | Glide's `SimulateHelicopter`: lift along the rotor axis, collective, cyclic and tail rotor torques, a keep-upright torque, capped drag, turbulence, a rotor that spins up when the pilot gets in and winds down after, and skids. |

## Controls

GoldSrc gives one usercmd: forward/back, strafe, up, view angles and the
button bits. They are bound the way Glide binds its input actions.

| | car, tank | plane, heli |
|---|---|---|
| forward / back | accelerate / brake, and reverse once stopped (the pedals swap in reverse) | throttle (plane power, heli collective) |
| strafe | steer | roll |
| look | tank: aims the turret | pitch and rudder: the nose turns toward where you look (Glide's mouse aim). `mouse_aim 0` flies with the keys instead: forward/back pitch, jump/duck throttle |
| jump | handbrake | throttle up |
| duck | brake pedal (hold with forward for a burnout) | throttle down |
| walk (`+speed`) | 70% throttle (Glide's throttle modifier) | |
| attack | tank: main gun | |
| attack2 | | free look (no steering from the view) |

`use` gets in (when you are next to a vehicle) and out. With nobody in it, a
car or tank's engine stops and the brakes hold it; a plane's engine stops and
the parking brake is set; a helicopter's rotor winds down and it settles.

## Trying it

```
vehicle spawn car            // in front of you; single player, or sv_cheats 1 in multiplayer
vehicle spawn tank models/some_tank.mdl
vehicle spawn plane
vehicle spawn heli
vehicle use                  // or press use next to it
vehicle info                 // speed, wheels down, gear, rpm, clutch, power, altitude
vehicle remove
```

Launch option `-vehicle car` (or `tank`, `plane`, `heli`) spawns one on the
player when the map starts and puts them in it, which is the quickest test on
any map: for example `-vehicle car +map c1a0` on a desktop build. `flight`
still works as a name for the plane.

Models: `vehicle spawn <type> <model>`, or set `vehicle_model_car`,
`vehicle_model_tank`, `vehicle_model_plane`, `vehicle_model_heli`. With none
set the vehicle uses the driver's own player model as a stand-in. A model
that is not already precached by the map is late-precached (it must exist).
Studio models are expected to face +X.

### GameCube

The layer is about 40 KB of code, so GameCube builds leave it out unless the
build asks for it, because HL1 has no vehicles and plays at the MEM1 edge:

```
XASH_GC_VEHICLES=1 scripts/build-gamecube.sh
```

Without that flag the hooks compile to empty inline stubs and no vehicle code
is linked. Vehicles need the route that runs `SV_RunCmd` (real player
movement); the lean `-gcnewgame` route without `-gcfullphysics` moves the
player elsewhere and does not drive vehicles.

## Tuning

Every tunable is a float in a per-type table, listed and set from the console
(no cvar per parameter). The names follow Glide's:

```
vehicle tune car                         // list
vehicle tune car max_rpm 7000            // set (sv_cheats 1 in multiplayer)
vehicle tune car side_traction_max 2800
vehicle tune tank wheels_per_side 4      // wheel layout changes apply at once
vehicle tune plane mouse_aim 0           // fly with the keys
```

| Group | Names | Glide |
|---|---|---|
| body | `mass`, `inertia_roll/pitch/yaw`, `drag_roll/pitch/yaw` | `ChassisMass`, the VPhysics inertia, `AngularDrag` |
| wheels | `wheel_front_x/z`, `wheel_rear_x/z`, `wheel_y`, `wheel_radius`, `susp_length`, `spring_strength`, `spring_damper`, `brake_power`, `fwd_traction_max`, `side_traction_mult`, `side_traction_max_ang`, `side_traction_max`, `side_traction_min`, `rolling_resistance`, `surface_grip` | `CreateWheel` positions and the glide_wheel parameters |
| engine (car, tank) | `min_rpm`, `max_rpm`, `min_rpm_torque`, `max_rpm_torque`, `diff_ratio`, `efficiency`, `power_distribution`, `engine_brake_torque`, `flywheel_*`, `turbo`, `fast_shift`, `shift_delay`, `gear_count`, `gear_reverse`, `gear1`..`gear6` | base_glide_car's NW vars, `GetGears` |
| steering (car, tank) | `max_steer`, `steer_cone_rate`, `steer_cone_max_speed`, `steer_cone_max_angle`, `counter_steer`, `burnout_force`, `unflip_force`, `air_roll/pitch/yaw`, `air_max_roll/pitch/yaw` | `MaxSteerAngle`, `SteerCone*`, `CounterSteer`, `BurnoutForce`, `UnflipForce`, `AirControlForce`, `AirMaxAngularVelocity` |
| tank | `wheels_per_side`, `turn_in_place`, `turret_height`, `barrel_length`, `turret_pitch_up/down`, `turret_yaw_speed`, `turret_yaw_accel`, `fire_interval`, `recoil` | `TurnInPlaceTorqueMultiplier`, `TurretOffset`, `PitchAngMax/Min`, `MaxYawSpeed`, `YawAcceleration`, `FireDelay`, `TurretRecoilForce` |
| plane | `lift_drag_roll/pitch/yaw`, `lift_fwd_drag`, `lift_side_drag`, `lift_factor`, `max_speed`, `lift_speed`, `control_speed`, `engine_force`, `align_force`, `pitch/yaw/roll_force`, `power_response`, `steer_max`, `steer_cone_max_speed`, `reverse_torque`, `max_reverse_speed`, `mouse_aim`, `aim_gain`, `input_rate` | `PlaneParams` and base_glide_plane |
| heli | `base_power`, `heli_drag_fwd/side/up`, `max_fwd_drag`, `max_side_drag`, `turbulence`, `push_up_force`, `pitch/yaw/roll_force`, `push_fwd_force`, `max_speed`, `upright_force`, `power_response`, `mouse_aim`, `aim_gain`, `input_rate` | `HelicopterParams` and base_glide_heli |
| all | `gravity_scale`, `bounce`, `impact_speed`, `crash_speed`, `cam_dist`, `cam_height`, `cam_pitch`, `cam_lag`, `cam_follow` | (engine side) |

Glide's numbers drop in unchanged: a Glide vehicle's `CreateFeatures` (wheel
positions, `Set*` calls, `PlaneParams` overrides) maps one to one onto these
names. Camera tunables are sent to the driver's client, so changes show at
once. Other cvars: `vehicle_input_scale` (400, the move value that counts as
full stick), `vehicle_tank_magnitude` (150, the shell's `env_explosion`
magnitude, 0 for effect only), and on the client `cl_vehicle_cam` (1 = chase
camera, 0 = ride in the seat).

## How it fits together

```
common/vehicle.h          public API: types, tunables, input, step, camera
common/vehicle.c          tunable tables, input mapping, fixed step, rigid body,
                          glide_wheel (suspension, traction), hull slide move, camera
common/vehicle_car.c      base_glide_car: engine, gearbox, clutch, steering, air control
common/vehicle_tank.c     base_glide_tank: tank numbers, turn in place, turret, gun
common/vehicle_aircraft.c base_glide_aircraft/plane/heli: SimulatePlane, SimulateHelicopter, mouse aim
server/sv_vehicle.c       edicts, drivers, console, usercmd and frame hooks
client/cl_vehicle.c       chase camera from the vehicle entity and our view
tests/test_vehicle.py     host test (tests/fixtures/vehicle_test.c)
```

The core knows nothing about edicts or cvars: it steps a `veh_t` from a
`veh_input_t` and a `veh_world_t` whose `trace` callback sweeps a box. The
server binding implements that with `SV_Move`, the client camera with
`CL_TraceLine`, and the host test with a few infinite planes.

- **Units.** Glide's forces keep Glide's meaning, so its numbers need no
  conversion. Wheel and engine forces are impulses once a Source physics tick
  (1/66.67 s); a force at a point gives the torque r × F in VPhysics' kg·m²
  degrees; lift, drag and control terms are per-mass accelerations and
  torques against `mass * inertia`. Body rates are Source's: roll, pitch
  (positive = nose down), yaw (positive = left), degrees per second.
- **Stepping.** Fixed 60 Hz steps with an accumulator (`Veh_Update`), at most
  4 a call, so handling is the same at any frame rate or usercmd rate. Each
  step runs Glide's order: think (engine, gears, steering, aircraft power and
  controls), wheel think (torque and brakes to each wheel), then the physics
  (wheel forces, flight forces, the slow-speed slope hold) and one
  integration. A driver's usercmds step their vehicle in place of `PM_Move`
  (`SV_RunCmd` → `SV_VehicleRunCmd`); empty vehicles coast and settle in
  `SV_Physics` (`SV_VehicleFrame`) and sleep after two still seconds.
- **Body.** Semi-implicit Euler: forces to velocity, Glide's `AngularDrag`
  integrated implicitly (so a stiff drag on a light axis stays stable), the
  basis rotated by the body rate and re-orthonormalised once a step. GoldSrc
  BSP collision snaps a box to the nearest stock hull, so the body collides
  as a 64×64×64 large-hull box, offset so it rides just above the wheels,
  with Quake-style sliding and a small wall bounce.
- **Wheels.** One point trace per wheel down the body's up axis, exactly
  glide_wheel's `DoPhysics`: spring and damper, bottom-out guard, rolling
  resistance, brake and drive torque on the traction circle, wheel spin from
  grip loss, the slip angle side traction ramp, and the small unclamped side
  force. Glide's surface tables are a single `surface_grip` and
  `rolling_resistance` here (GoldSrc textures carry no physics material).
- **Driver.** While driving, the player is `MOVETYPE_NONE`, `SOLID_NOT`,
  `EF_NODRAW`, with no view or weapon model, and placed with their eye at the
  seat every command, linked with trigger touches so changelevels and
  scripted triggers still fire. Everything is restored on exit, beside the
  vehicle where there is room (driver's side, other side, behind, front,
  roof). Death or a lost vehicle forces an exit.
- **Camera.** The server tells the driver's client which entity it drives
  (`cl_vehicle` stufftext with the camera tunables). The client orbits that
  entity's interpolated origin with its own view angles, so looking round has
  no network delay; a wall between the vehicle and the camera pulls it in at
  once. For the car the view yaw turns with the vehicle. The vehicle camera
  is applied in `V_RenderView` and on the GameCube world-render route in
  `GC_RenderNewGameWorldFrames`, in both cases before the cinematic camera,
  which still wins.
- **Tank gun.** A shot traces 8192 units from the muzzle and fires the game's
  own `env_explosion` at the hit point, so damage, sprite and sound are that
  game's. A game without `env_explosion` gets sparks.

## Differences from Glide

Each of these is something GoldSrc or this engine cannot do the Garry's Mod
way; everything else is Glide's code path and Glide's numbers.

- **Collision shape.** Glide collides with the model's VPhysics mesh. GoldSrc
  moves boxes, so the body is an axis-aligned large-hull box. A box cannot
  tip over an edge, so while it rests on a floor without its wheels, gravity
  tips it the way the mesh would: back onto its wheels when it is less than
  90° over, onto its roof past that (where steering unflips it, as in Glide).
  A hard knock takes half the spin out, as VPhysics contact friction would.
- **Inertia.** VPhysics computes inertia from the mesh. Here it is a tunable
  per axis (per kilogram, m²), set from the size of the Glide vehicle each
  default is based on.
- **Plane speeds.** Glide's plane needs 1600 units/s to lift, a runway longer
  than a GoldSrc map is wide (Source maps are four times the area). The
  plane's `lift_speed`, `max_speed` and `control_speed` are Glide's × 0.6;
  the forces are Glide's. Set them back with `vehicle tune` on a big map.
- **Gravity.** GoldSrc's 800 is used as is (Garry's Mod's is 600), so springs
  sit a little lower and a plane needs a few degrees more nose-up to hold
  height; Glide's helicopter lift is written against the world gravity, so
  it hovers the same.
- **Brakes at rest.** Glide's brake always pushes against the wheel's
  direction of roll, which chatters at zero speed and relies on VPhysics
  friction to hold the car. Here the brake fades out below 5 units/s so a
  stopped wheel is held without the chatter.
- **Two pedals on one axis.** Glide's throttle and brake are separate keys;
  GoldSrc's forward/back is one axis, so duck is a second brake pedal (needed
  for burnouts). Glide's mouse steering for cars is not used; strafe steers.
- **Not ported (yet).** Engine and tyre sounds, damage and health, wheel
  models spinning (the state is there: `veh_wheel_t.spin`), the plane's
  retractable landing gear, VTOL, boats and motorbikes, weapons other than
  the tank gun, countermeasures, and a helicopter going out of control when
  the pilot bails at height.

## Budget

- No allocation until the first `vehicle` command or `-vehicle`; then one
  ~10 KB block from the host pool (8 vehicle slots, the four tunable tables,
  per-client button state), reused across maps and never per frame.
- A step is one point trace per wheel (car 4, tank 4 by default and up to 8,
  plane 3, helicopter 4), one altitude probe for aircraft, and 1 to 4 hull
  sweeps. Math is single precision with a few `sinf`/`cosf`/`atan2f`/`expf`
  per wheel and step.

## Not done yet

- Vehicles are not saved in save games and do not cross level changes.
- The hull is an axis-aligned box: a long vehicle turned 45° collides as its
  64-unit box, not its rotated shape.
- No passengers, no AI drivers.
- A map entity (`vehicle_car` etc. placed in a BSP) is not read yet; spawning
  is by console or launch option.

## Glide licence

The handling model is ported from Glide, which is under this licence:

```
MIT License

Copyright (c) 2024 StyledStrike

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
