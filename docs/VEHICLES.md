# Engine vehicles: car, flight, tank

A drivable-vehicle layer that lives in the engine, so the same three vehicle
types run in every GoldSrc game the engine loads (Half-Life, Day of Defeat,
the MOHAA conversion), with no game DLL changes.

| Type | Feel | Controls (default binds) |
|---|---|---|
| `car` | Late-90s arcade racer: grippy until the handbrake breaks the rear loose, then a held powerslide | move forward/back: throttle, brake, reverse. strafe: steer. jump: handbrake (drift). run / attack2: boost |
| `tank` | Tracked hull that pivots on the spot, turret slews toward where you look | forward/back: drive. strafe: turn (pivot at rest). jump: brake. look: aim the turret. attack: main gun |
| `flight` | Arcade flight model: thrust, drag, lift with a stall, coordinated turns | look: where to fly (mouse aim). forward/back or jump/duck: throttle lever. strafe: roll. attack2 on the ground: wheel brake |

`use` gets in (when you are next to a vehicle) and out.

## Trying it

```
vehicle spawn car            // in front of you; single player, or sv_cheats 1 in multiplayer
vehicle spawn tank models/some_tank.mdl
vehicle use                  // or press use next to it
vehicle info
vehicle remove
```

Launch option `-vehicle car` (or `tank`, `flight`) spawns one on the player
when the map starts and puts them in it, which is the quickest test on any
map: for example `-vehicle car +map c1a0` on a desktop build.

Models: `vehicle spawn <type> <model>`, or set `vehicle_model_car`,
`vehicle_model_tank`, `vehicle_model_flight`. With none set the vehicle uses
the driver's own player model as a stand-in. A model that is not already
precached by the map is late-precached (it must exist). Studio models are
expected to face +X.

### GameCube

The layer is about 20 KB of code, so GameCube builds leave it out unless the
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
(no cvar per parameter):

```
vehicle tune car                     // list
vehicle tune car drift_grip 0.3      // set (sv_cheats 1 in multiplayer)
vehicle tune flight mouse_aim 0      // stick flying instead of mouse aim
```

Units are GoldSrc units/s, units/s², degrees and seconds. The defaults are in
`Veh_CarDefaults`, `Veh_TankDefaults` and `Veh_FlightDefaults`. Camera
tunables (`cam_dist`, `cam_height`, `cam_pitch`, `cam_lag`, `cam_follow`) are
sent to the driver's client, so changes show immediately.

Other cvars: `vehicle_input_scale` (400, the move value that counts as full
stick), `vehicle_tank_magnitude` (150, the shell's `env_explosion`
magnitude, 0 for effect only), and on the client `cl_vehicle_cam` (1 = chase
camera, 0 = ride in the seat).

## How it fits together

```
common/vehicle.h         public API: types, tunables, input, step, camera
common/vehicle.c         shared: tunable tables, input mapping, fixed step,
                         body basis, raycast wheels, hull slide move, camera
common/vehicle_car.c     drive, grip, steering, drift
common/vehicle_tank.c    tracks, pivot turns, turret, gun
common/vehicle_flight.c  thrust, lift, stall, control laws (mouse aim / stick)
server/sv_vehicle.c      edicts, drivers, console, usercmd and frame hooks
client/cl_vehicle.c      chase camera from the vehicle entity and our view
tests/test_vehicle.py    host test (tests/fixtures/vehicle_test.c)
```

The core knows nothing about edicts or cvars: it steps a `veh_t` from a
`veh_input_t` and a `veh_world_t` whose `trace` callback sweeps a box. The
server binding implements that with `SV_Move`, the client camera with
`CL_TraceLine`, and the host test with a few infinite planes.

- **Stepping.** Fixed 60 Hz steps with an accumulator (`Veh_Update`), at most
  4 a call, so handling is the same at any frame rate or usercmd rate. A
  driver's usercmds step their vehicle in place of `PM_Move`
  (`SV_RunCmd` → `SV_VehicleRunCmd`); empty vehicles coast and settle in
  `SV_Physics` (`SV_VehicleFrame`) and sleep after two still seconds.
- **Ground vehicles.** One point trace per wheel (four) down the body's up
  axis gives spring/damper suspension, the ground normal the body tilts to,
  and traction. The hull is one swept box with Quake-style sliding and a
  small wall bounce. GoldSrc BSP collision snaps a box to the nearest stock
  hull, so the default hulls are the 64×64×64 large hull, offset so the box
  rides just above the wheels.
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

## Budget

- No allocation until the first `vehicle` command or `-vehicle`; then one
  ~5 KB block from the host pool (8 vehicle slots, the three tunable tables,
  per-client button state), reused across maps and never per frame.
- A car or tank step is 4 wheel traces plus 1 to 4 hull traces; a flight step
  is 1 ground probe plus the hull. Math is single precision with a few
  `sinf`/`cosf`/`atan2f` a step; the body basis is rotated with first-order
  updates and re-orthonormalised once a step.

## Not done yet

- Vehicles are not saved in save games and do not cross level changes.
- No engine or tyre sound yet (needs a per-game sound that is precached).
- The hull is an axis-aligned box: a long vehicle turned 45° collides as its
  64-unit box, not its rotated shape.
- No passengers, no damage model for the vehicle itself, no AI drivers.
- A map entity (`vehicle_car` etc. placed in a BSP) is not read yet; spawning
  is by console or launch option.
