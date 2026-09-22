# SBEM chunk map for /Logbook/byId/<id>/Data (Suunto Race)

Generated from the real captured `/Logbook/byId/<id>/Descriptors` response
(2026-09-21 HCI log), parsed with the chunk-header rules decompiled from
`libmds.so`'s `BSML::SmlStreamParser::parseChunkHeader`. See
`logbook-data-format.md`'s "The APK had the whole field map" section.

Chunk id == descriptor id. A group chunk's payload is its children's values
concatenated in the listed order, with no per-child headers.

`delta->N` means the value is a differential against descriptor N's running
value (`<DELTAREF>`); `d0` is a zero-byte "unchanged" marker.

Decoding a raw value: apply `<MOD>`'s first expression (x = raw). `precision`
is output formatting only, NOT a scale factor. `nillable=V` means raw==V is
"no reading".

## chunk 0x01 (1)

- `33` TimeISO8601 — `local64,baseonly`

## chunk 0x02 (2)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `37` Sample.Events+Array.ArrayBegin — `uint8`
- `38` Sample.Events.Array.Lap.Type — `enum:0=Start,1=Stop,2=Distance,3=Manual,4=Interval,5=High Interval,6=Low Interval`

## chunk 0x03 (3)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `37` Sample.Events+Array.ArrayBegin — `uint8`
- `39` Sample.Events.Array.Pause.State — `bool`

## chunk 0x04 (4)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `37` Sample.Events+Array.ArrayBegin — `uint8`
- `40` Sample.Events.Array.Altitude.Source — `enum:0=GPS,1=Pressure,2=Other`
- `41` Sample.Events.Array.Altitude.AltitudeOffset — `int32,precision=0`
- `42` Sample.Events.Array.Altitude.PressureOffset — `int32,precision=0`

## chunk 0x05 (5)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `47` Sample.Distance — `uint32,precision=1,nillable=4294967295`
- `37` Sample.Events+Array.ArrayBegin — `uint8`
- `43` Sample.Events.Array.Swimming.Type — `enum:0=StyleChange,1=Turn,2=Stroke`
- `44` Sample.Events.Array.Swimming.TotalLengths — `uint16`
- `45` Sample.Events.Array.Swimming.PrevPoolLengthDuration — `float32`
- `46` Sample.Events.Array.Swimming.PrevPoolLengthStyle — `enum:1=Other,2=Butterfly,3=Backstroke,4=Breaststroke,5=Freestyle,6=Drill`

## chunk 0x06 (6)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `37` Sample.Events+Array.ArrayBegin — `uint8`
- `43` Sample.Events.Array.Swimming.Type — `enum:0=StyleChange,1=Turn,2=Stroke`

## chunk 0x07 (7)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `47` Sample.Distance — `uint32,precision=1,nillable=4294967295`
- `37` Sample.Events+Array.ArrayBegin — `uint8`
- `43` Sample.Events.Array.Swimming.Type — `enum:0=StyleChange,1=Turn,2=Stroke`
- `46` Sample.Events.Array.Swimming.PrevPoolLengthStyle — `enum:1=Other,2=Butterfly,3=Backstroke,4=Breaststroke,5=Freestyle,6=Drill`

## chunk 0x08 (8)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `48` Sample.Events.Array.Activity.ActivityType — `uint8`
- `49` Sample.Events.Array.Activity.CustomModeId — `utf8`

## chunk 0x09 (9)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `50` Sample.Events.Array.Interval.Type — `enum:0=Warmup,1=Interval,2=Recovery,3=Rest,4=Cooldown,5=RepeatStart,6=RepeatEnd,7=Finished`

## chunk 0x0a (10)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `37` Sample.Events+Array.ArrayBegin — `uint8`
- `51` Sample.Events.Array.VerticalLap.Type — `enum:0=Downhill,1=Uphill,2=Flat,3=Unknown`

## chunk 0x0b (11)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `37` Sample.Events+Array.ArrayBegin — `uint8`
- `52` Sample.Events.Array.Correction.Distance — `uint16`
- `53` Sample.Events.Array.Correction.Lane — `uint8`
- `54` Sample.Events.Array.Correction.Length — `uint16`
- `55` Sample.Events.Array.Correction.XCenter — `float32`
- `56` Sample.Events.Array.Correction.YCenter — `float32`
- `57` Sample.Events.Array.Correction.ZeroLat — `int32`
- `58` Sample.Events.Array.Correction.ZeroLon — `int32`
- `59` Sample.Events.Array.Correction.Angle — `float32`

## chunk 0x0c (12)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `60` Sample.UTC — `local64`
- `61` Sample.Latitude — `int32`  MOD: `PI*x/(10^7*180),y*10^7*180/PI`
- `62` Sample.Longitude — `int32`  MOD: `PI*x/(10^7*180),y*10^7*180/PI`
- `63` Sample.GPSAltitude — `uint16,precision=2,nillable=65535`  MOD: `x/5-1000,(y+1000)*5`

## chunk 0x0d (13)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `68` delta->60 (Sample.UTC) — `dint16`
- `69` delta->61 (Sample.Latitude) — `dint8`
- `70` delta->62 (Sample.Longitude) — `dint8`
- `71` delta->63 (Sample.GPSAltitude) — `dint8`

## chunk 0x0e (14)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `64` Sample.EHPE — `uint16`
- `65` Sample.EVPE — `uint16`
- `66` Sample.Satellite5BestSNR — `uint8`  MOD: `x/5,y*5`
- `67` Sample.NumberOfSatellites — `uint8`

## chunk 0x0f (15)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `72` delta->64 (Sample.EHPE) — `dint8`
- `73` delta->65 (Sample.EVPE) — `dint8`
- `66` Sample.Satellite5BestSNR — `uint8`  MOD: `x/5,y*5`
- `67` Sample.NumberOfSatellites — `uint8`

## chunk 0x10 (16)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `74` Sample.GpsRef.utc — `local64`
- `75` Sample.GpsRef.lat — `int32`
- `76` Sample.GpsRef.lon — `int32`

## chunk 0x11 (17)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `77` delta->74 (Sample.GpsRef.utc) — `dint16`
- `78` delta->75 (Sample.GpsRef.lat) — `dint8`
- `79` delta->76 (Sample.GpsRef.lon) — `dint8`

## chunk 0x12 (18)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `80` Sample.HR — `uint8,precision=2`  MOD: `x/60,y*60`

## chunk 0x13 (19)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `81` R-R+IBI — `uint16`

## chunk 0x14 (20)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`
- `82` delta->81 (R-R+IBI) — `dint8`

## chunk 0x15 (21)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `88` Sample.Distance — `uint32,precision=1,nillable=4294967295`
- `85` Sample.AbsPressure — `uint32,precision=0,nillable=4294967295`
- `86` Sample.SeaLevelPressure — `uint16,precision=0,nillable=65535`  MOD: `x+85000,y-85000`
- `89` Sample.Speed — `uint16,precision=3,nillable=65535`  MOD: `x/50,y*50`
- `90` Sample.VerticalSpeed — `int16,precision=3,nillable=-32768`  MOD: `x/50,y*50`
- `84` Sample.Temperature — `uint16,precision=2,nillable=65535`  MOD: `x/100,y*100`
- `87` Sample.Altitude — `uint16,nillable=65535`  MOD: `x/5-1000,(y+1000)*5`
- `91` Sample.Power — `uint16,precision=0,nillable=65535`
- `92` Sample.Cadence — `uint8,precision=3,nillable=255`  MOD: `x/60,y*60`
- `96` Sample.GroundContactTime — `uint16,precision=3,nillable=65535`  MOD: `x/1000,y*1000`
- `97` Sample.VerticalOscillation — `uint16,precision=3,nillable=65535`  MOD: `x/1000,y*1000`
- `98` Sample.FlightTime — `uint16,precision=3,nillable=65535`  MOD: `x/1000,y*1000`
- `99` Sample.LeftGroundContactBalance — `uint16,precision=1,nillable=65535`  MOD: `x/10,y*10`
- `100` Sample.RightGroundContactBalance — `uint16,precision=1,nillable=65535`  MOD: `x/10,y*10`
- `101` Sample.ContactTimeRatio — `uint16,precision=1,nillable=65535`  MOD: `x/10,y*10`

## chunk 0x16 (22)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `106` delta->88 (Sample.Distance) — `duint8`
- `103` delta->85 (Sample.AbsPressure) — `dint8`
- `104` delta->86 (Sample.SeaLevelPressure) — `dint8`
- `107` delta->89 (Sample.Speed) — `dint8`
- `108` delta->90 (Sample.VerticalSpeed) — `dint8`
- `102` delta->84 (Sample.Temperature) — `dint8`
- `105` delta->87 (Sample.Altitude) — `dint8`
- `109` delta->91 (Sample.Power) — `dint8`
- `92` Sample.Cadence — `uint8,precision=3,nillable=255`  MOD: `x/60,y*60`
- `112` delta->96 (Sample.GroundContactTime) — `dint8`
- `113` delta->97 (Sample.VerticalOscillation) — `dint8`
- `114` delta->98 (Sample.FlightTime) — `dint8`
- `115` delta->99 (Sample.LeftGroundContactBalance) — `dint8`
- `116` delta->100 (Sample.RightGroundContactBalance) — `dint8`
- `117` delta->101 (Sample.ContactTimeRatio) — `dint8`

## chunk 0x17 (23)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `93` Sample.BatteryCurrent — `int16,precision=3,nillable=65535`  MOD: `x/(1000*128),y*(1000*128)`
- `94` Sample.BatteryVoltage — `uint16,precision=5,nillable=65535`  MOD: `x/1000,y*1000`
- `95` Sample.BatteryCharge — `uint8,precision=3,nillable=255`  MOD: `x/100,y*100`

## chunk 0x18 (24)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `110` delta->93 (Sample.BatteryCurrent) — `dint8`
- `111` delta->94 (Sample.BatteryVoltage) — `dint8`
- `95` Sample.BatteryCharge — `uint8,precision=3,nillable=255`  MOD: `x/100,y*100`

## chunk 0x19 (25)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `118` Sample.Depth — `float32,precision=2`

## chunk 0x1a (26)

- `34` delta->33 (TimeISO8601) — `dint16`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `119` Sample.SurfacePressure — `float32,precision=1`
- `120` Sample.MaxSurfacePressure — `float32,precision=1`
- `121` Sample.MinSurfacePressure — `float32,precision=1`

## chunk 0x1b (27)

- `32` TimeISO8601 — `local64`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `136` Header.DateTime — `local64`
- `132` Header.Duration — `uint32,precision=3`  MOD: `x/1000,y*1000`
- `133` Header.PauseDuration — `uint32,precision=3`  MOD: `x/1000,y*1000`
- `137` Header.Distance — `uint32,precision=1,nillable=4294967295`
- `138` Header.StepCount — `uint32,nillable=0`
- `139` Header.StepCountSupervised — `uint32,nillable=0`
- `135` Header.ActivityType — `int32`
- `140` Header.Ascent — `float32,precision=1,nillable=0`
- `141` Header.AscentTime — `float32,precision=1,nillable=0`
- `142` Header.Descent — `float32,precision=1,nillable=0`
- `143` Header.DescentTime — `float32,precision=1,nillable=0`
- `144` Header.VerticalSpeed — `float32,precision=3`
- `145` Header.Altitude.Max — `float32,precision=1`
- `146` Header.Altitude.Min — `float32,precision=1`
- `147` Header.Energy — `float32,precision=1`
- `148` Header.EPOC — `float32,precision=1`
- `149` Header.PeakTrainingEffect — `float32,precision=1`
- `150` Header.RecoveryTime — `uint32`
- `151` Header.MAXVO2 — `float32,precision=1,nillable=0`
- `152` Header.FitnessAge — `uint8,precision=0,nillable=0`
- `153` Header.FitnessAgeClassification — `enum:0=NotAvailable,1=VeryPoor,2=Poor,3=Fair,4=Good,5=Excellent,6=Superior`
- `154` Header.TraingingLoadPeak — `float32,precision=1,nillable=0`
- `201` Header.MoveType — `int32,nillable=0`
- `202` Header.IsSupervised — `bool`
- `203` Header.DeviceLocation — `enum:0=Unspecified,1=WristLeft,2=WristRight,3=WristLeftInside,4=WristRightInside,15=Other`
- `155` Header.HrZones.Zone1Duration — `float32,precision=3`
- `156` Header.HrZones.Zone2Duration — `float32,precision=3`
- `157` Header.HrZones.Zone3Duration — `float32,precision=3`
- `158` Header.HrZones.Zone4Duration — `float32,precision=3`
- `159` Header.HrZones.Zone5Duration — `float32,precision=3`
- `160` Header.HrZones.Zone2LowerLimit — `float32,precision=3,nillable=0`
- `161` Header.HrZones.Zone3LowerLimit — `float32,precision=3,nillable=0`
- `162` Header.HrZones.Zone4LowerLimit — `float32,precision=3,nillable=0`
- `163` Header.HrZones.Zone5LowerLimit — `float32,precision=3,nillable=0`
- `164` Header.SpeedZones.Zone1Duration — `float32,precision=3`
- `165` Header.SpeedZones.Zone2Duration — `float32,precision=3`
- `166` Header.SpeedZones.Zone3Duration — `float32,precision=3`
- `167` Header.SpeedZones.Zone4Duration — `float32,precision=3`
- `168` Header.SpeedZones.Zone5Duration — `float32,precision=3`
- `169` Header.SpeedZones.Zone2LowerLimit — `float32,precision=3,nillable=0`
- `170` Header.SpeedZones.Zone3LowerLimit — `float32,precision=3,nillable=0`
- `171` Header.SpeedZones.Zone4LowerLimit — `float32,precision=3,nillable=0`
- `172` Header.SpeedZones.Zone5LowerLimit — `float32,precision=3,nillable=0`
- `173` Header.PowerZones.Zone1Duration — `float32,precision=3`
- `174` Header.PowerZones.Zone2Duration — `float32,precision=3`
- `175` Header.PowerZones.Zone3Duration — `float32,precision=3`
- `176` Header.PowerZones.Zone4Duration — `float32,precision=3`
- `177` Header.PowerZones.Zone5Duration — `float32,precision=3`
- `178` Header.PowerZones.Zone2LowerLimit — `float32,precision=3,nillable=0`
- `179` Header.PowerZones.Zone3LowerLimit — `float32,precision=3,nillable=0`
- `180` Header.PowerZones.Zone4LowerLimit — `float32,precision=3,nillable=0`
- `181` Header.PowerZones.Zone5LowerLimit — `float32,precision=3,nillable=0`
- `182` Header.Personal.MaxHR — `float32,precision=3,nillable=0`
- `183` Header.Targets.Duration — `float32,precision=1,nillable=0`
- `184` Header.Targets.Distance — `float32,precision=1,nillable=0`
- `185` Header.Targets.HeartRateZone — `enum:0=None,1=Zone1,2=Zone2,3=Zone3,4=Zone4,5=Zone5`
- `186` Header.Targets.PowerZone — `enum:0=None,1=Zone1,2=Zone2,3=Zone3,4=Zone4,5=Zone5`
- `187` Header.Targets.SpeedZone — `enum:0=None,1=Zone1,2=Zone2,3=Zone3,4=Zone4,5=Zone5`
- `188` Header.PoolLength — `float32,precision=1`
- `189` Header.PoolLengths — `uint32`
- `190` Header.DownhillCount — `uint32,nillable=0`
- `191` Header.DownhillDistance — `uint32,precision=1,nillable=0`
- `192` Header.DownhillDescent — `float32,precision=1,nillable=0`
- `193` Header.DownhillSpeed.Avg — `float32,precision=3,nillable=65535`
- `194` Header.DownhillSpeed.Max — `float32,precision=3,nillable=65535`
- `195` Header.DownhillMaxLength — `float32,precision=1,nillable=65535`
- `196` Header.DownhillMaxDescent — `float32,precision=1,nillable=65535`
- `197` Header.DownhillGrade.Max — `float32,precision=1,nillable=65535`
- `198` Header.DownhillDuration — `float32,precision=1,nillable=0`
- `199` Header.DownhillGrade.Avg — `float32,precision=1,nillable=65535`
- `200` Header.Feeling — `int32,nillable=0`
- `211` Header.Settings.SgeeEpoTimestamp — `local64`
- `212` Header.Settings.EnabledNavigationSystems — `enum:1=GPS,3=GPS+Glonass,65=GPS+Beidou,129=GPS+Galileo`
- `213` Header.Settings.AutoPause.Enabled — `bool`
- `214` Header.Settings.AutoLap.Enabled — `bool`
- `215` Header.Settings.AutoLap.Distance — `float32,precision=2,nillable=0`
- `216` Header.Settings.AutoLap.Duration — `float32,precision=0,nillable=0`
- `217` Header.Settings.AltiBaroProfile — `enum:0=Altitude,1=Barometer,2=Automatic`
- `218` Header.Settings.FusedAltiUsed — `bool`
- `219` Header.Settings.BikePodUsed — `bool`
- `220` Header.Settings.PowerPodUsed — `bool`
- `221` Header.Settings.FootPodUsed — `bool`
- `222` Header.Settings.FootPodAutoCalib.Coeff — `float32`
- `223` Header.Settings.FootPodAutoCalib.Used — `bool`
- `224` Header.Settings.HrUsed — `bool`
- `204` Header.Device.Name — `enum:0=Tianjin`
- `205` Header.Device.SerialNumber — `enum:0=2352D0000247`
- `207` Header.Device.Info.SW — `enum:0=2.53.42`
- `206` Header.Device.Info.HW — `enum:0=Phoenix_RevB1`
- `208` Header.Device.Info.BatteryDesignCapacity — `uint16,precision=1,nillable=0`  MOD: `x*3.6,y/3.6`
- `209` Header.Device.Info.BatteryFullCapacity — `uint16,precision=1,nillable=0`  MOD: `x*3.6,y/3.6`
- `225` Header.Temperature.Min — `float32,precision=1,nillable=0`
- `226` Header.Temperature.Max — `float32,precision=1,nillable=0`
- `227` Header.RepetitionCount — `uint32,nillable=0`
- `228` Header.DiveTime — `float32,precision=1,nillable=-16000000`
- `229` Header.DiveTimeMax — `float32,precision=1,nillable=-16000000`
- `230` Header.DiveInWorkout — `uint8`
- `231` Header.Depth.Max — `float32,precision=2,nillable=-16000000`
- `232` Header.MaxDepthAverage — `float32,precision=2,nillable=-16000000`
- `233` Header.DepthAverage — `float32,precision=2,nillable=-16000000`
- `234` Header.GroundContactTime.Avg — `float32,precision=6,nillable=-16000000`
- `235` Header.GroundContactTime.Min — `float32,precision=6,nillable=-16000000`
- `236` Header.GroundContactTime.Max — `float32,precision=6,nillable=-16000000`
- `237` Header.VerticalOscillation.Avg — `float32,precision=6,nillable=-16000000`
- `238` Header.VerticalOscillation.Min — `float32,precision=6,nillable=-16000000`
- `239` Header.VerticalOscillation.Max — `float32,precision=6,nillable=-16000000`
- `240` Header.ZoneSenseZones.Zone1Duration — `float32,precision=3`
- `241` Header.ZoneSenseZones.Zone2Duration — `float32,precision=3`
- `242` Header.ZoneSenseZones.Zone3Duration — `float32,precision=3`
- `243` Header.ZoneSenseZones.Zone2LowerLimit — `float32,precision=3,nillable=0`
- `244` Header.ZoneSenseZones.Zone3LowerLimit — `float32,precision=3,nillable=0`
- `245` Header.Targets.ZoneSenseZones — `enum:0=None,1=Zone1,2=Zone2,3=Zone3`
- `246` Header.Settings.UserOwned — `bool`
- `247` Header.FlightTime.Avg — `float32,precision=6,nillable=-16000000`
- `248` Header.FlightTime.Min — `float32,precision=6,nillable=-16000000`
- `249` Header.FlightTime.Max — `float32,precision=6,nillable=-16000000`
- `250` Header.LeftGroundContactBalance.Avg — `float32,precision=6,nillable=-16000000`
- `251` Header.LeftGroundContactBalance.Min — `float32,precision=6,nillable=-16000000`
- `252` Header.LeftGroundContactBalance.Max — `float32,precision=6,nillable=-16000000`
- `253` Header.RightGroundContactBalance.Avg — `float32,precision=6,nillable=-16000000`
- `254` Header.RightGroundContactBalance.Min — `float32,precision=6,nillable=-16000000`
- `255` Header.RightGroundContactBalance.Max — `float32,precision=6,nillable=-16000000`
- `256` Header.ContactTimeRatio.Avg — `float32,precision=6,nillable=-16000000`
- `257` Header.ContactTimeRatio.Min — `float32,precision=6,nillable=-16000000`
- `258` Header.ContactTimeRatio.Max — `float32,precision=6,nillable=-16000000`
- `259` Header.Targets.Ascent — `float32,precision=1,nillable=0`
- `260` Header.HrRecovery.Drop — `dint16`
- `261` Header.HrRecovery.Level — `enum:0=Low,1=Medium,2=High,3=Invalid`
- `262` Header.HrRecovery.ComparisonLevel — `enum:0=Slow,1=Normal,2=Excellent,3=Invalid`
- `134` Header.Activity — `utf8`
- `210` Header.Notes — `utf8`

## chunk 0x1c (28)

- `32` TimeISO8601 — `local64`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `136` Header.DateTime — `local64`
- `132` Header.Duration — `uint32,precision=3`  MOD: `x/1000,y*1000`
- `137` Header.Distance — `uint32,precision=1,nillable=4294967295`
- `135` Header.ActivityType — `int32`
- `204` Header.Device.Name — `enum:0=Tianjin`
- `205` Header.Device.SerialNumber — `enum:0=2352D0000247`
- `207` Header.Device.Info.SW — `enum:0=2.53.42`
- `206` Header.Device.Info.HW — `enum:0=Phoenix_RevB1`

## chunk 0x1d (29)

- `32` TimeISO8601 — `local64`
- `36` delta->35 (Samples.TimelineSample.Source) — `d0`
- `263` Windows+Window.Type — `enum:0=Move,1=Activity,2=Lap,3=Autolap,4=PoolLength,5=Interval,6=IntervalSet,7=Rest,8=Downhill,9=DownhillTotal,10=Dive`
- `264` Windows.Window.ActivityId — `int32,nillable=-2147483647`
- `265` Windows.Window.Duration — `float32,precision=1,nillable=-16000000`
- `266` Windows.Window.Distance — `float32,precision=1,nillable=-16000000`
- `267` Windows.Window.DistanceMax — `float32,precision=1,nillable=-16000000`
- `268` Windows.Window.Energy — `float32,precision=1,nillable=-16000000`
- `269` Windows.Window+Speed.Min — `float32,precision=3,nillable=-16000000`
- `270` Windows.Window.Speed.Max — `float32,precision=3,nillable=-16000000`
- `271` Windows.Window.Speed.Avg — `float32,precision=3,nillable=-16000000`
- `272` Windows.Window+VerticalSpeed.Min — `float32,precision=3,nillable=-16000000`
- `273` Windows.Window.VerticalSpeed.Max — `float32,precision=3,nillable=-16000000`
- `274` Windows.Window.VerticalSpeed.Avg — `float32,precision=3,nillable=-16000000`
- `275` Windows.Window+HR.Min — `float32,precision=3,nillable=-16000000`
- `276` Windows.Window.HR.Max — `float32,precision=3,nillable=-16000000`
- `277` Windows.Window.HR.Avg — `float32,precision=3,nillable=-16000000`
- `278` Windows.Window+Cadence.Min — `float32,precision=3,nillable=-16000000`
- `279` Windows.Window.Cadence.Max — `float32,precision=3,nillable=-16000000`
- `280` Windows.Window.Cadence.Avg — `float32,precision=3,nillable=-16000000`
- `281` Windows.Window+Power.Min — `float32,precision=1,nillable=-16000000`
- `282` Windows.Window.Power.Max — `float32,precision=1,nillable=-16000000`
- `283` Windows.Window.Power.Avg — `float32,precision=1,nillable=-16000000`
- `284` Windows.Window+DownhillGrade.Min — `float32,precision=1,nillable=-16000000`
- `285` Windows.Window.DownhillGrade.Max — `float32,precision=1,nillable=-16000000`
- `286` Windows.Window.DownhillGrade.Avg — `float32,precision=1,nillable=-16000000`
- `287` Windows.Window+Temperature.Min — `float32,precision=1,nillable=-16000000`
- `288` Windows.Window.Temperature.Max — `float32,precision=1,nillable=-16000000`
- `289` Windows.Window.Temperature.Avg — `float32,precision=1,nillable=-16000000`
- `290` Windows.Window+Altitude.Min — `float32,precision=1,nillable=-16000000`
- `291` Windows.Window.Altitude.Max — `float32,precision=1,nillable=-16000000`
- `292` Windows.Window.Altitude.Avg — `float32,precision=1,nillable=-16000000`
- `293` Windows.Window.Ascent — `float32,precision=1,nillable=-16000000`
- `294` Windows.Window.AscentTime — `float32,precision=1,nillable=-16000000`
- `295` Windows.Window.Descent — `float32,precision=1,nillable=-16000000`
- `296` Windows.Window.DescentMax — `float32,precision=1,nillable=-16000000`
- `297` Windows.Window.DescentTime — `float32,precision=1,nillable=-16000000`
- `298` Windows.Window.RecoveryTime — `float32,precision=1,nillable=-16000000`
- `299` Windows.Window+Swolf.Min — `float32,precision=1,nillable=-16000000`
- `300` Windows.Window.Swolf.Max — `float32,precision=1,nillable=-16000000`
- `301` Windows.Window.Swolf.Avg — `float32,precision=1,nillable=-16000000`
- `302` Windows.Window+Strokes.Min — `float32,precision=1,nillable=-16000000`
- `303` Windows.Window.Strokes.Max — `float32,precision=1,nillable=-16000000`
- `304` Windows.Window.Strokes.Avg — `float32,precision=1,nillable=-16000000`
- `305` Windows.Window+StrokeRate.Min — `float32,precision=3,nillable=-16000000`
- `306` Windows.Window.StrokeRate.Max — `float32,precision=3,nillable=-16000000`
- `307` Windows.Window.StrokeRate.Avg — `float32,precision=3,nillable=-16000000`
- `308` Windows.Window.SwimStyle — `enum:1=Rest,2=Butterfly,3=Back,4=Breast,5=Free,6=Drill`
- `309` Windows.Window.RepetitionCount — `float32,precision=1,nillable=0`
- `310` Windows.Window+Depth.Min — `float32,precision=2,nillable=-16000000`
- `311` Windows.Window.Depth.Max — `float32,precision=2,nillable=-16000000`
- `312` Windows.Window.Depth.Avg — `float32,precision=2,nillable=-16000000`
- `313` Windows.Window.DiveTime — `float32,precision=1,nillable=-16000000`
- `314` Windows.Window.DiveTimeMax — `float32,precision=1,nillable=-16000000`
- `315` Windows.Window.DiveRecoveryTime — `float32,precision=1,nillable=-16000000`
- `316` Windows.Window.DiveInWorkout — `uint8`
- `317` Windows.Window.MaxDepthAverage — `float32,precision=2,nillable=-16000000`
- `318` Windows.Window.DepthAverage — `float32,precision=2,nillable=-16000000`
- `319` Windows.Window.DiveAscentSpeedMax — `float32,precision=1,nillable=-16000000`
- `320` Windows.Window.DiveDescentSpeedMax — `float32,precision=1,nillable=-16000000`
- `321` Windows.Window.SwimTurnStartTime — `float32,precision=1,nillable=-16000000`
- `322` Windows.Window.SwimTurnStopTime — `float32,precision=1,nillable=-16000000`
- `323` Windows.Window+GroundContactTime.Min — `float32,precision=6,nillable=-16000000`
- `324` Windows.Window.GroundContactTime.Max — `float32,precision=6,nillable=-16000000`
- `325` Windows.Window.GroundContactTime.Avg — `float32,precision=6,nillable=-16000000`
- `326` Windows.Window+VerticalOscillation.Min — `float32,precision=6,nillable=-16000000`
- `327` Windows.Window.VerticalOscillation.Max — `float32,precision=6,nillable=-16000000`
- `328` Windows.Window.VerticalOscillation.Avg — `float32,precision=6,nillable=-16000000`
- `329` Windows.Window.IntervalType — `enum:0=Warmup,1=Interval,2=Recovery,3=Rest,4=Cooldown,5=RepeatStart,6=RepeatEnd,7=Finished,8=Unknown`
- `330` Windows.Window.IntervalLoopNum — `uint16,nillable=0`
- `331` Windows.Window+FlightTime.Min — `float32,precision=6,nillable=-16000000`
- `332` Windows.Window.FlightTime.Max — `float32,precision=6,nillable=-16000000`
- `333` Windows.Window.FlightTime.Avg — `float32,precision=6,nillable=-16000000`
- `334` Windows.Window+LeftGroundContactBalance.Min — `float32,precision=6,nillable=-16000000`
- `335` Windows.Window.LeftGroundContactBalance.Max — `float32,precision=6,nillable=-16000000`
- `336` Windows.Window.LeftGroundContactBalance.Avg — `float32,precision=6,nillable=-16000000`
- `337` Windows.Window+RightGroundContactBalance.Min — `float32,precision=6,nillable=-16000000`
- `338` Windows.Window.RightGroundContactBalance.Max — `float32,precision=6,nillable=-16000000`
- `339` Windows.Window.RightGroundContactBalance.Avg — `float32,precision=6,nillable=-16000000`
- `340` Windows.Window+ContactTimeRatio.Min — `float32,precision=6,nillable=-16000000`
- `341` Windows.Window.ContactTimeRatio.Max — `float32,precision=6,nillable=-16000000`
- `342` Windows.Window.ContactTimeRatio.Avg — `float32,precision=6,nillable=-16000000`
- `343` Windows.Window.IntervalNotes — `utf8`

## non-group descriptors (leaf fields, referenced by the groups above)

- `32` TimeISO8601 — `local64`
- `33` TimeISO8601 — `local64,baseonly`
- `35` Samples.TimelineSample.Source — `enum:0=suunto-2352D0000247`
- `37` Sample.Events+Array.ArrayBegin — `uint8`
- `38` Sample.Events.Array.Lap.Type — `enum:0=Start,1=Stop,2=Distance,3=Manual,4=Interval,5=High Interval,6=Low Interval`
- `39` Sample.Events.Array.Pause.State — `bool`
- `40` Sample.Events.Array.Altitude.Source — `enum:0=GPS,1=Pressure,2=Other`
- `41` Sample.Events.Array.Altitude.AltitudeOffset — `int32,precision=0`
- `42` Sample.Events.Array.Altitude.PressureOffset — `int32,precision=0`
- `43` Sample.Events.Array.Swimming.Type — `enum:0=StyleChange,1=Turn,2=Stroke`
- `44` Sample.Events.Array.Swimming.TotalLengths — `uint16`
- `45` Sample.Events.Array.Swimming.PrevPoolLengthDuration — `float32`
- `46` Sample.Events.Array.Swimming.PrevPoolLengthStyle — `enum:1=Other,2=Butterfly,3=Backstroke,4=Breaststroke,5=Freestyle,6=Drill`
- `47` Sample.Distance — `uint32,precision=1,nillable=4294967295`
- `48` Sample.Events.Array.Activity.ActivityType — `uint8`
- `49` Sample.Events.Array.Activity.CustomModeId — `utf8`
- `50` Sample.Events.Array.Interval.Type — `enum:0=Warmup,1=Interval,2=Recovery,3=Rest,4=Cooldown,5=RepeatStart,6=RepeatEnd,7=Finished`
- `51` Sample.Events.Array.VerticalLap.Type — `enum:0=Downhill,1=Uphill,2=Flat,3=Unknown`
- `52` Sample.Events.Array.Correction.Distance — `uint16`
- `53` Sample.Events.Array.Correction.Lane — `uint8`
- `54` Sample.Events.Array.Correction.Length — `uint16`
- `55` Sample.Events.Array.Correction.XCenter — `float32`
- `56` Sample.Events.Array.Correction.YCenter — `float32`
- `57` Sample.Events.Array.Correction.ZeroLat — `int32`
- `58` Sample.Events.Array.Correction.ZeroLon — `int32`
- `59` Sample.Events.Array.Correction.Angle — `float32`
- `60` Sample.UTC — `local64`
- `61` Sample.Latitude — `int32`  MOD: `PI*x/(10^7*180),y*10^7*180/PI`
- `62` Sample.Longitude — `int32`  MOD: `PI*x/(10^7*180),y*10^7*180/PI`
- `63` Sample.GPSAltitude — `uint16,precision=2,nillable=65535`  MOD: `x/5-1000,(y+1000)*5`
- `64` Sample.EHPE — `uint16`
- `65` Sample.EVPE — `uint16`
- `66` Sample.Satellite5BestSNR — `uint8`  MOD: `x/5,y*5`
- `67` Sample.NumberOfSatellites — `uint8`
- `74` Sample.GpsRef.utc — `local64`
- `75` Sample.GpsRef.lat — `int32`
- `76` Sample.GpsRef.lon — `int32`
- `80` Sample.HR — `uint8,precision=2`  MOD: `x/60,y*60`
- `81` R-R+IBI — `uint16`
- `83` R-R.Bug71539 — `uint16`
- `84` Sample.Temperature — `uint16,precision=2,nillable=65535`  MOD: `x/100,y*100`
- `85` Sample.AbsPressure — `uint32,precision=0,nillable=4294967295`
- `86` Sample.SeaLevelPressure — `uint16,precision=0,nillable=65535`  MOD: `x+85000,y-85000`
- `87` Sample.Altitude — `uint16,nillable=65535`  MOD: `x/5-1000,(y+1000)*5`
- `88` Sample.Distance — `uint32,precision=1,nillable=4294967295`
- `89` Sample.Speed — `uint16,precision=3,nillable=65535`  MOD: `x/50,y*50`
- `90` Sample.VerticalSpeed — `int16,precision=3,nillable=-32768`  MOD: `x/50,y*50`
- `91` Sample.Power — `uint16,precision=0,nillable=65535`
- `92` Sample.Cadence — `uint8,precision=3,nillable=255`  MOD: `x/60,y*60`
- `93` Sample.BatteryCurrent — `int16,precision=3,nillable=65535`  MOD: `x/(1000*128),y*(1000*128)`
- `94` Sample.BatteryVoltage — `uint16,precision=5,nillable=65535`  MOD: `x/1000,y*1000`
- `95` Sample.BatteryCharge — `uint8,precision=3,nillable=255`  MOD: `x/100,y*100`
- `96` Sample.GroundContactTime — `uint16,precision=3,nillable=65535`  MOD: `x/1000,y*1000`
- `97` Sample.VerticalOscillation — `uint16,precision=3,nillable=65535`  MOD: `x/1000,y*1000`
- `98` Sample.FlightTime — `uint16,precision=3,nillable=65535`  MOD: `x/1000,y*1000`
- `99` Sample.LeftGroundContactBalance — `uint16,precision=1,nillable=65535`  MOD: `x/10,y*10`
- `100` Sample.RightGroundContactBalance — `uint16,precision=1,nillable=65535`  MOD: `x/10,y*10`
- `101` Sample.ContactTimeRatio — `uint16,precision=1,nillable=65535`  MOD: `x/10,y*10`
- `118` Sample.Depth — `float32,precision=2`
- `119` Sample.SurfacePressure — `float32,precision=1`
- `120` Sample.MaxSurfacePressure — `float32,precision=1`
- `121` Sample.MinSurfacePressure — `float32,precision=1`
- `122` Sample.DiveEvents.DiveState — `enum:0=Idling,1=Diving,2=Recovering`
- `123` Sample.DiveEvents.State.Type — `enum:19=Ndl exceeded,35=At Deco Stop,36=At Deep Stop,37=At Safety Stop,38=Deco Stop Ahead,39=Deep Stop Ahead,40=Safety Stop Ahead`
- `124` Sample.DiveEvents.State.Active — `bool`
- `125` Sample.DiveEvents.Notify.Type — `enum:11=Gas Switch,28=User Tank Pressure,29=User Gas Time,30=Sidemount,31=Depth,32=Dive Time,41=Stop done,42=User Ndl,44=Recovery time,60=Bearing set,61=Bearing cleared,62=Stopwatch started,63=Stopwatch reset`
- `126` Sample.DiveEvents.Notify.Active — `bool`
- `127` Sample.DiveEvents.Ooam.Type — `enum:1=Out of battery,2=Ceiling broken,3=SW crash,4=Max depth,5=Algorithm changed,6=Gauge dive`
- `128` Sample.DiveEvents.Warning.Type — `enum:6=User PO2 High,14=CNS80%,15=OTU250,20=NoDecoTime,28=User Tank Pressure,29=User Gas Time,30=Sidemount,31=Depth,32=Dive Time,42=User Ndl,44=Recovery time,50=Battery`
- `129` Sample.DiveEvents.Warning.Active — `bool`
- `130` Sample.DiveEvents.Alarm.Type — `enum:1=PO2 Low,2=PO2 High,3=Tank Pressure,4=Gas Time,5=Ascent Speed,7=CNS100%,8=OTU300,10=Deco Stop Broken,12=Deep Stop Broken,13=Safety Stop Broken,31=Depth,50=Battery`
- `131` Sample.DiveEvents.Alarm.Active — `bool`
- `132` Header.Duration — `uint32,precision=3`  MOD: `x/1000,y*1000`
- `133` Header.PauseDuration — `uint32,precision=3`  MOD: `x/1000,y*1000`
- `134` Header.Activity — `utf8`
- `135` Header.ActivityType — `int32`
- `136` Header.DateTime — `local64`
- `137` Header.Distance — `uint32,precision=1,nillable=4294967295`
- `138` Header.StepCount — `uint32,nillable=0`
- `139` Header.StepCountSupervised — `uint32,nillable=0`
- `140` Header.Ascent — `float32,precision=1,nillable=0`
- `141` Header.AscentTime — `float32,precision=1,nillable=0`
- `142` Header.Descent — `float32,precision=1,nillable=0`
- `143` Header.DescentTime — `float32,precision=1,nillable=0`
- `144` Header.VerticalSpeed — `float32,precision=3`
- `145` Header.Altitude.Max — `float32,precision=1`
- `146` Header.Altitude.Min — `float32,precision=1`
- `147` Header.Energy — `float32,precision=1`
- `148` Header.EPOC — `float32,precision=1`
- `149` Header.PeakTrainingEffect — `float32,precision=1`
- `150` Header.RecoveryTime — `uint32`
- `151` Header.MAXVO2 — `float32,precision=1,nillable=0`
- `152` Header.FitnessAge — `uint8,precision=0,nillable=0`
- `153` Header.FitnessAgeClassification — `enum:0=NotAvailable,1=VeryPoor,2=Poor,3=Fair,4=Good,5=Excellent,6=Superior`
- `154` Header.TraingingLoadPeak — `float32,precision=1,nillable=0`
- `155` Header.HrZones.Zone1Duration — `float32,precision=3`
- `156` Header.HrZones.Zone2Duration — `float32,precision=3`
- `157` Header.HrZones.Zone3Duration — `float32,precision=3`
- `158` Header.HrZones.Zone4Duration — `float32,precision=3`
- `159` Header.HrZones.Zone5Duration — `float32,precision=3`
- `160` Header.HrZones.Zone2LowerLimit — `float32,precision=3,nillable=0`
- `161` Header.HrZones.Zone3LowerLimit — `float32,precision=3,nillable=0`
- `162` Header.HrZones.Zone4LowerLimit — `float32,precision=3,nillable=0`
- `163` Header.HrZones.Zone5LowerLimit — `float32,precision=3,nillable=0`
- `164` Header.SpeedZones.Zone1Duration — `float32,precision=3`
- `165` Header.SpeedZones.Zone2Duration — `float32,precision=3`
- `166` Header.SpeedZones.Zone3Duration — `float32,precision=3`
- `167` Header.SpeedZones.Zone4Duration — `float32,precision=3`
- `168` Header.SpeedZones.Zone5Duration — `float32,precision=3`
- `169` Header.SpeedZones.Zone2LowerLimit — `float32,precision=3,nillable=0`
- `170` Header.SpeedZones.Zone3LowerLimit — `float32,precision=3,nillable=0`
- `171` Header.SpeedZones.Zone4LowerLimit — `float32,precision=3,nillable=0`
- `172` Header.SpeedZones.Zone5LowerLimit — `float32,precision=3,nillable=0`
- `173` Header.PowerZones.Zone1Duration — `float32,precision=3`
- `174` Header.PowerZones.Zone2Duration — `float32,precision=3`
- `175` Header.PowerZones.Zone3Duration — `float32,precision=3`
- `176` Header.PowerZones.Zone4Duration — `float32,precision=3`
- `177` Header.PowerZones.Zone5Duration — `float32,precision=3`
- `178` Header.PowerZones.Zone2LowerLimit — `float32,precision=3,nillable=0`
- `179` Header.PowerZones.Zone3LowerLimit — `float32,precision=3,nillable=0`
- `180` Header.PowerZones.Zone4LowerLimit — `float32,precision=3,nillable=0`
- `181` Header.PowerZones.Zone5LowerLimit — `float32,precision=3,nillable=0`
- `182` Header.Personal.MaxHR — `float32,precision=3,nillable=0`
- `183` Header.Targets.Duration — `float32,precision=1,nillable=0`
- `184` Header.Targets.Distance — `float32,precision=1,nillable=0`
- `185` Header.Targets.HeartRateZone — `enum:0=None,1=Zone1,2=Zone2,3=Zone3,4=Zone4,5=Zone5`
- `186` Header.Targets.PowerZone — `enum:0=None,1=Zone1,2=Zone2,3=Zone3,4=Zone4,5=Zone5`
- `187` Header.Targets.SpeedZone — `enum:0=None,1=Zone1,2=Zone2,3=Zone3,4=Zone4,5=Zone5`
- `188` Header.PoolLength — `float32,precision=1`
- `189` Header.PoolLengths — `uint32`
- `190` Header.DownhillCount — `uint32,nillable=0`
- `191` Header.DownhillDistance — `uint32,precision=1,nillable=0`
- `192` Header.DownhillDescent — `float32,precision=1,nillable=0`
- `193` Header.DownhillSpeed.Avg — `float32,precision=3,nillable=65535`
- `194` Header.DownhillSpeed.Max — `float32,precision=3,nillable=65535`
- `195` Header.DownhillMaxLength — `float32,precision=1,nillable=65535`
- `196` Header.DownhillMaxDescent — `float32,precision=1,nillable=65535`
- `197` Header.DownhillGrade.Max — `float32,precision=1,nillable=65535`
- `198` Header.DownhillDuration — `float32,precision=1,nillable=0`
- `199` Header.DownhillGrade.Avg — `float32,precision=1,nillable=65535`
- `200` Header.Feeling — `int32,nillable=0`
- `201` Header.MoveType — `int32,nillable=0`
- `202` Header.IsSupervised — `bool`
- `203` Header.DeviceLocation — `enum:0=Unspecified,1=WristLeft,2=WristRight,3=WristLeftInside,4=WristRightInside,15=Other`
- `204` Header.Device.Name — `enum:0=Tianjin`
- `205` Header.Device.SerialNumber — `enum:0=2352D0000247`
- `206` Header.Device.Info.HW — `enum:0=Phoenix_RevB1`
- `207` Header.Device.Info.SW — `enum:0=2.53.42`
- `208` Header.Device.Info.BatteryDesignCapacity — `uint16,precision=1,nillable=0`  MOD: `x*3.6,y/3.6`
- `209` Header.Device.Info.BatteryFullCapacity — `uint16,precision=1,nillable=0`  MOD: `x*3.6,y/3.6`
- `210` Header.Notes — `utf8`
- `211` Header.Settings.SgeeEpoTimestamp — `local64`
- `212` Header.Settings.EnabledNavigationSystems — `enum:1=GPS,3=GPS+Glonass,65=GPS+Beidou,129=GPS+Galileo`
- `213` Header.Settings.AutoPause.Enabled — `bool`
- `214` Header.Settings.AutoLap.Enabled — `bool`
- `215` Header.Settings.AutoLap.Distance — `float32,precision=2,nillable=0`
- `216` Header.Settings.AutoLap.Duration — `float32,precision=0,nillable=0`
- `217` Header.Settings.AltiBaroProfile — `enum:0=Altitude,1=Barometer,2=Automatic`
- `218` Header.Settings.FusedAltiUsed — `bool`
- `219` Header.Settings.BikePodUsed — `bool`
- `220` Header.Settings.PowerPodUsed — `bool`
- `221` Header.Settings.FootPodUsed — `bool`
- `222` Header.Settings.FootPodAutoCalib.Coeff — `float32`
- `223` Header.Settings.FootPodAutoCalib.Used — `bool`
- `224` Header.Settings.HrUsed — `bool`
- `225` Header.Temperature.Min — `float32,precision=1,nillable=0`
- `226` Header.Temperature.Max — `float32,precision=1,nillable=0`
- `227` Header.RepetitionCount — `uint32,nillable=0`
- `228` Header.DiveTime — `float32,precision=1,nillable=-16000000`
- `229` Header.DiveTimeMax — `float32,precision=1,nillable=-16000000`
- `230` Header.DiveInWorkout — `uint8`
- `231` Header.Depth.Max — `float32,precision=2,nillable=-16000000`
- `232` Header.MaxDepthAverage — `float32,precision=2,nillable=-16000000`
- `233` Header.DepthAverage — `float32,precision=2,nillable=-16000000`
- `234` Header.GroundContactTime.Avg — `float32,precision=6,nillable=-16000000`
- `235` Header.GroundContactTime.Min — `float32,precision=6,nillable=-16000000`
- `236` Header.GroundContactTime.Max — `float32,precision=6,nillable=-16000000`
- `237` Header.VerticalOscillation.Avg — `float32,precision=6,nillable=-16000000`
- `238` Header.VerticalOscillation.Min — `float32,precision=6,nillable=-16000000`
- `239` Header.VerticalOscillation.Max — `float32,precision=6,nillable=-16000000`
- `240` Header.ZoneSenseZones.Zone1Duration — `float32,precision=3`
- `241` Header.ZoneSenseZones.Zone2Duration — `float32,precision=3`
- `242` Header.ZoneSenseZones.Zone3Duration — `float32,precision=3`
- `243` Header.ZoneSenseZones.Zone2LowerLimit — `float32,precision=3,nillable=0`
- `244` Header.ZoneSenseZones.Zone3LowerLimit — `float32,precision=3,nillable=0`
- `245` Header.Targets.ZoneSenseZones — `enum:0=None,1=Zone1,2=Zone2,3=Zone3`
- `246` Header.Settings.UserOwned — `bool`
- `247` Header.FlightTime.Avg — `float32,precision=6,nillable=-16000000`
- `248` Header.FlightTime.Min — `float32,precision=6,nillable=-16000000`
- `249` Header.FlightTime.Max — `float32,precision=6,nillable=-16000000`
- `250` Header.LeftGroundContactBalance.Avg — `float32,precision=6,nillable=-16000000`
- `251` Header.LeftGroundContactBalance.Min — `float32,precision=6,nillable=-16000000`
- `252` Header.LeftGroundContactBalance.Max — `float32,precision=6,nillable=-16000000`
- `253` Header.RightGroundContactBalance.Avg — `float32,precision=6,nillable=-16000000`
- `254` Header.RightGroundContactBalance.Min — `float32,precision=6,nillable=-16000000`
- `255` Header.RightGroundContactBalance.Max — `float32,precision=6,nillable=-16000000`
- `256` Header.ContactTimeRatio.Avg — `float32,precision=6,nillable=-16000000`
- `257` Header.ContactTimeRatio.Min — `float32,precision=6,nillable=-16000000`
- `258` Header.ContactTimeRatio.Max — `float32,precision=6,nillable=-16000000`
- `259` Header.Targets.Ascent — `float32,precision=1,nillable=0`
- `260` Header.HrRecovery.Drop — `dint16`
- `261` Header.HrRecovery.Level — `enum:0=Low,1=Medium,2=High,3=Invalid`
- `262` Header.HrRecovery.ComparisonLevel — `enum:0=Slow,1=Normal,2=Excellent,3=Invalid`
- `263` Windows+Window.Type — `enum:0=Move,1=Activity,2=Lap,3=Autolap,4=PoolLength,5=Interval,6=IntervalSet,7=Rest,8=Downhill,9=DownhillTotal,10=Dive`
- `264` Windows.Window.ActivityId — `int32,nillable=-2147483647`
- `265` Windows.Window.Duration — `float32,precision=1,nillable=-16000000`
- `266` Windows.Window.Distance — `float32,precision=1,nillable=-16000000`
- `267` Windows.Window.DistanceMax — `float32,precision=1,nillable=-16000000`
- `268` Windows.Window.Energy — `float32,precision=1,nillable=-16000000`
- `269` Windows.Window+Speed.Min — `float32,precision=3,nillable=-16000000`
- `270` Windows.Window.Speed.Max — `float32,precision=3,nillable=-16000000`
- `271` Windows.Window.Speed.Avg — `float32,precision=3,nillable=-16000000`
- `272` Windows.Window+VerticalSpeed.Min — `float32,precision=3,nillable=-16000000`
- `273` Windows.Window.VerticalSpeed.Max — `float32,precision=3,nillable=-16000000`
- `274` Windows.Window.VerticalSpeed.Avg — `float32,precision=3,nillable=-16000000`
- `275` Windows.Window+HR.Min — `float32,precision=3,nillable=-16000000`
- `276` Windows.Window.HR.Max — `float32,precision=3,nillable=-16000000`
- `277` Windows.Window.HR.Avg — `float32,precision=3,nillable=-16000000`
- `278` Windows.Window+Cadence.Min — `float32,precision=3,nillable=-16000000`
- `279` Windows.Window.Cadence.Max — `float32,precision=3,nillable=-16000000`
- `280` Windows.Window.Cadence.Avg — `float32,precision=3,nillable=-16000000`
- `281` Windows.Window+Power.Min — `float32,precision=1,nillable=-16000000`
- `282` Windows.Window.Power.Max — `float32,precision=1,nillable=-16000000`
- `283` Windows.Window.Power.Avg — `float32,precision=1,nillable=-16000000`
- `284` Windows.Window+DownhillGrade.Min — `float32,precision=1,nillable=-16000000`
- `285` Windows.Window.DownhillGrade.Max — `float32,precision=1,nillable=-16000000`
- `286` Windows.Window.DownhillGrade.Avg — `float32,precision=1,nillable=-16000000`
- `287` Windows.Window+Temperature.Min — `float32,precision=1,nillable=-16000000`
- `288` Windows.Window.Temperature.Max — `float32,precision=1,nillable=-16000000`
- `289` Windows.Window.Temperature.Avg — `float32,precision=1,nillable=-16000000`
- `290` Windows.Window+Altitude.Min — `float32,precision=1,nillable=-16000000`
- `291` Windows.Window.Altitude.Max — `float32,precision=1,nillable=-16000000`
- `292` Windows.Window.Altitude.Avg — `float32,precision=1,nillable=-16000000`
- `293` Windows.Window.Ascent — `float32,precision=1,nillable=-16000000`
- `294` Windows.Window.AscentTime — `float32,precision=1,nillable=-16000000`
- `295` Windows.Window.Descent — `float32,precision=1,nillable=-16000000`
- `296` Windows.Window.DescentMax — `float32,precision=1,nillable=-16000000`
- `297` Windows.Window.DescentTime — `float32,precision=1,nillable=-16000000`
- `298` Windows.Window.RecoveryTime — `float32,precision=1,nillable=-16000000`
- `299` Windows.Window+Swolf.Min — `float32,precision=1,nillable=-16000000`
- `300` Windows.Window.Swolf.Max — `float32,precision=1,nillable=-16000000`
- `301` Windows.Window.Swolf.Avg — `float32,precision=1,nillable=-16000000`
- `302` Windows.Window+Strokes.Min — `float32,precision=1,nillable=-16000000`
- `303` Windows.Window.Strokes.Max — `float32,precision=1,nillable=-16000000`
- `304` Windows.Window.Strokes.Avg — `float32,precision=1,nillable=-16000000`
- `305` Windows.Window+StrokeRate.Min — `float32,precision=3,nillable=-16000000`
- `306` Windows.Window.StrokeRate.Max — `float32,precision=3,nillable=-16000000`
- `307` Windows.Window.StrokeRate.Avg — `float32,precision=3,nillable=-16000000`
- `308` Windows.Window.SwimStyle — `enum:1=Rest,2=Butterfly,3=Back,4=Breast,5=Free,6=Drill`
- `309` Windows.Window.RepetitionCount — `float32,precision=1,nillable=0`
- `310` Windows.Window+Depth.Min — `float32,precision=2,nillable=-16000000`
- `311` Windows.Window.Depth.Max — `float32,precision=2,nillable=-16000000`
- `312` Windows.Window.Depth.Avg — `float32,precision=2,nillable=-16000000`
- `313` Windows.Window.DiveTime — `float32,precision=1,nillable=-16000000`
- `314` Windows.Window.DiveTimeMax — `float32,precision=1,nillable=-16000000`
- `315` Windows.Window.DiveRecoveryTime — `float32,precision=1,nillable=-16000000`
- `316` Windows.Window.DiveInWorkout — `uint8`
- `317` Windows.Window.MaxDepthAverage — `float32,precision=2,nillable=-16000000`
- `318` Windows.Window.DepthAverage — `float32,precision=2,nillable=-16000000`
- `319` Windows.Window.DiveAscentSpeedMax — `float32,precision=1,nillable=-16000000`
- `320` Windows.Window.DiveDescentSpeedMax — `float32,precision=1,nillable=-16000000`
- `321` Windows.Window.SwimTurnStartTime — `float32,precision=1,nillable=-16000000`
- `322` Windows.Window.SwimTurnStopTime — `float32,precision=1,nillable=-16000000`
- `323` Windows.Window+GroundContactTime.Min — `float32,precision=6,nillable=-16000000`
- `324` Windows.Window.GroundContactTime.Max — `float32,precision=6,nillable=-16000000`
- `325` Windows.Window.GroundContactTime.Avg — `float32,precision=6,nillable=-16000000`
- `326` Windows.Window+VerticalOscillation.Min — `float32,precision=6,nillable=-16000000`
- `327` Windows.Window.VerticalOscillation.Max — `float32,precision=6,nillable=-16000000`
- `328` Windows.Window.VerticalOscillation.Avg — `float32,precision=6,nillable=-16000000`
- `329` Windows.Window.IntervalType — `enum:0=Warmup,1=Interval,2=Recovery,3=Rest,4=Cooldown,5=RepeatStart,6=RepeatEnd,7=Finished,8=Unknown`
- `330` Windows.Window.IntervalLoopNum — `uint16,nillable=0`
- `331` Windows.Window+FlightTime.Min — `float32,precision=6,nillable=-16000000`
- `332` Windows.Window.FlightTime.Max — `float32,precision=6,nillable=-16000000`
- `333` Windows.Window.FlightTime.Avg — `float32,precision=6,nillable=-16000000`
- `334` Windows.Window+LeftGroundContactBalance.Min — `float32,precision=6,nillable=-16000000`
- `335` Windows.Window.LeftGroundContactBalance.Max — `float32,precision=6,nillable=-16000000`
- `336` Windows.Window.LeftGroundContactBalance.Avg — `float32,precision=6,nillable=-16000000`
- `337` Windows.Window+RightGroundContactBalance.Min — `float32,precision=6,nillable=-16000000`
- `338` Windows.Window.RightGroundContactBalance.Max — `float32,precision=6,nillable=-16000000`
- `339` Windows.Window.RightGroundContactBalance.Avg — `float32,precision=6,nillable=-16000000`
- `340` Windows.Window+ContactTimeRatio.Min — `float32,precision=6,nillable=-16000000`
- `341` Windows.Window.ContactTimeRatio.Max — `float32,precision=6,nillable=-16000000`
- `342` Windows.Window.ContactTimeRatio.Avg — `float32,precision=6,nillable=-16000000`
- `343` Windows.Window.IntervalNotes — `utf8`
- `344` Zapps+Zapp.Name — `utf8`
- `345` Zapps.Zapp.Id — `utf8`
- `346` Zapps.Zapp.HeaderId — `uint8`
- `347` Zapps.Zapp.Version — `utf8`
- `348` Zapps.Zapp.AuthorId — `utf8`
- `349` Zapps.Zapp.ExternalId — `utf8`
- `350` Zapps.Zapp.SummaryOutputs+Output.Name — `utf8`
- `351` Zapps.Zapp.SummaryOutputs.Output.SummaryValue — `float32`
- `352` Zapps.Zapp.SummaryOutputs.Output.Format — `utf8`
- `353` Zapps.Zapp.SummaryOutputs.Output.Postfix — `utf8`
- `354` Zapps.Zapp.SummaryOutputs.Output.Id — `utf8`
- `355` Zapps.Zapp.Channels+Info.Format — `utf8`
- `356` Zapps.Zapp.Channels.Info.Name — `utf8`
- `357` Zapps.Zapp.Channels.Info.VariableId — `utf8`
- `358` Zapps.Zapp.Channels.Info.ChannelId — `uint8`
- `359` Zapps.Zapp.Channels.Info.Inverted — `bool`
- `360` Sample.ZappSample.ChannelId — `uint8`
- `361` Sample.ZappSample.Value — `float32`
