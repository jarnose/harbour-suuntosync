# Activity type ids

Extracted from the official Android app's `com.stt.android.infomodel.ActivityMapping`
enum (`classes3.dex`), whose constructor is
`(String enumName, int ordinal, String name, int cloudId, int smlId)` - so each
entry states both ids and a name in one place. 123 entries; 110 have a watch id,
13 are cloud-only (`smlId` = -1).

Cross-checks that make this trustworthy rather than merely plausible: `smlId` 12
= Walking and 4 = Cycling are exactly the two ids this project had already
confirmed against Jarno's own BLE-fetched workouts, and `cloudId` 1 = Running,
2 = Cycling, 11 = Hiking, 22 = Trail running reproduce all four entries of the
cloud table the project had derived separately from `tajchert/suuntool`.

**The two id spaces are different and must not be mixed** - Cycling is 4 on the
watch and 2 in the cloud, Walking is 12 and 0. `Header.ActivityType` in a
`/Logbook/byId/<id>/Data` or `/Summary` payload is the watch (SML) id; the cloud
API's `activityId` is the other one.

Names below are the enum's own `name` field with camel case split; they are the
app's internal identifiers, not its localized UI strings (those live in Android
string resources keyed off `CoreActivityType`, not extracted here).

## Watch (SML) ids

| id | name |
|---|---|
| 1 | Unspecified sport |
| 2 | Multisport |
| 3 | Running |
| 4 | Cycling |
| 5 | Mountain biking |
| 6 | Swimming |
| 7 | Gravel cycling |
| 8 | Roller skating |
| 9 | Aerobics |
| 10 | Yoga |
| 11 | Trekking |
| 12 | Walking |
| 13 | Sailing |
| 14 | Kayaking |
| 15 | Rowing |
| 16 | Climbing |
| 17 | Indoor cycling |
| 18 | Circuit training |
| 19 | Triathlon |
| 20 | Downhill skiing |
| 21 | Snowboarding |
| 22 | Nordic skiing |
| 23 | Weight training |
| 24 | Basketball |
| 25 | Soccer |
| 26 | Ice hockey |
| 27 | Volleyball |
| 28 | American football |
| 29 | Softball |
| 30 | Cheerleading |
| 31 | Baseball |
| 32 | Padel |
| 33 | Tennis |
| 34 | Badminton |
| 35 | Table tennis |
| 36 | Racquet ball |
| 37 | Squash |
| 38 | Combat sport |
| 39 | Boxing |
| 40 | Floorball |
| 41 | Mermaiding |
| 42 | Jump rope |
| 43 | Track running |
| 44 | Vertical run |
| 45 | Parkour |
| 46 | Skateboarding |
| 47 | Futsal |
| 48 | Field hockey |
| 51 | Scuba diving |
| 52 | Free diving |
| 53 | Snorkeling |
| 54 | Surfing |
| 55 | Swim run |
| 56 | Duathlon |
| 57 | Aquathlon |
| 58 | Obstacle racing |
| 59 | Classic skiing |
| 60 | Skate skiing |
| 61 | Adventure racing |
| 62 | Bowling |
| 63 | Cricket |
| 64 | Crosstrainer |
| 65 | Dancing |
| 66 | Golf |
| 67 | Gymnastics |
| 68 | Handball |
| 69 | Horseback riding |
| 70 | Ice skating |
| 71 | Indoor rowing |
| 72 | Canoeing |
| 73 | Motorsports |
| 74 | Mountaineering |
| 75 | Orienteering |
| 76 | Rugby |
| 77 | Ski mountaineering |
| 78 | Ski touring |
| 79 | Stretching |
| 80 | Telemark skiing |
| 81 | Track and field |
| 82 | Trail running |
| 83 | Openwater swimming |
| 84 | Nordic walking |
| 85 | Snow shoeing |
| 86 | Windsurfing |
| 87 | Kettlebell |
| 88 | Roller skiing |
| 89 | Standup paddling |
| 90 | Crossfit |
| 91 | Kitesurfing kiting |
| 92 | Paragliding |
| 93 | Treadmill |
| 94 | Frisbee |
| 95 | Indoor |
| 96 | Hiking |
| 97 | Fishing |
| 98 | Hunting |
| 99 | Transition |
| 100 | Chores |
| 101 | Wheel chairing |
| 102 | Pilates |
| 103 | New yoga |
| 104 | Calisthenics |
| 105 | E-biking |
| 106 | E-MTB |
| 107 | Cyclocross |
| 108 | Hand cycling |
| 109 | Backcountry skiing |
| 110 | Split boarding |
| 111 | Biathlon |
| 112 | Meditation |

## Cloud ids

| id | name |
|---|---|
| 0 | Walking |
| 1 | Running |
| 2 | Cycling |
| 3 | Nordic skiing |
| 4 | Other1 |
| 5 | Other2 |
| 6 | Other3 |
| 7 | Other4 |
| 8 | Other5 |
| 9 | Other6 |
| 9 | Unspecified sport |
| 10 | Mountain biking |
| 11 | Hiking |
| 12 | Roller skating |
| 13 | Downhill skiing |
| 14 | Paddling |
| 15 | Rowing |
| 16 | Golf |
| 17 | Indoor |
| 18 | Parkour |
| 19 | Ballgames |
| 20 | Outdoor gym |
| 21 | Swimming |
| 22 | Trail running |
| 23 | Gym |
| 23 | Weight training |
| 24 | Nordic walking |
| 25 | Horseback riding |
| 26 | Motorsports |
| 27 | Skateboarding |
| 28 | Water sports |
| 29 | Climbing |
| 30 | Snowboarding |
| 31 | Ski touring |
| 32 | Fitness class |
| 33 | Soccer |
| 34 | Tennis |
| 35 | Basketball |
| 36 | Badminton |
| 37 | Baseball |
| 38 | Volleyball |
| 39 | American football |
| 40 | Table tennis |
| 41 | Racquet ball |
| 42 | Squash |
| 43 | Floorball |
| 44 | Handball |
| 45 | Softball |
| 46 | Bowling |
| 47 | Cricket |
| 48 | Rugby |
| 49 | Ice skating |
| 50 | Ice hockey |
| 51 | Yoga |
| 52 | Indoor cycling |
| 53 | Treadmill |
| 54 | Crossfit |
| 55 | Crosstrainer |
| 56 | Roller skiing |
| 57 | Indoor rowing |
| 58 | Stretching |
| 59 | Track and field |
| 60 | Orienteering |
| 61 | Standup paddling |
| 62 | Combat sport |
| 63 | Kettlebell |
| 64 | Dancing |
| 65 | Snow shoeing |
| 66 | Frisbee |
| 66 | Frisbee golf |
| 67 | Futsal |
| 68 | Multisport |
| 69 | Aerobics |
| 70 | Trekking |
| 71 | Sailing |
| 72 | Kayaking |
| 73 | Circuit training |
| 74 | Triathlon |
| 75 | Padel |
| 76 | Cheerleading |
| 77 | Boxing |
| 78 | Scuba diving |
| 79 | Free diving |
| 80 | Adventure racing |
| 81 | Gymnastics |
| 82 | Canoeing |
| 83 | Mountaineering |
| 84 | Telemark skiing |
| 85 | Openwater swimming |
| 86 | Windsurfing |
| 87 | Kitesurfing kiting |
| 88 | Paragliding |
| 90 | Snorkeling |
| 91 | Surfing |
| 92 | Swim run |
| 93 | Duathlon |
| 94 | Aquathlon |
| 95 | Obstacle racing |
| 96 | Fishing |
| 97 | Hunting |
| 98 | Transition |
| 99 | Gravel cycling |
| 100 | Mermaiding |
| 102 | Jump rope |
| 103 | Track running |
| 104 | Calisthenics |
| 105 | E-biking |
| 106 | E-MTB |
| 107 | Backcountry skiing |
| 108 | Wheel chairing |
| 109 | Hand cycling |
| 110 | Split boarding |
| 111 | Biathlon |
| 112 | Meditation |
| 113 | Field hockey |
| 114 | Cyclocross |
| 115 | Vertical run |
| 116 | Ski mountaineering |
| 117 | Skate skiing |
| 118 | Classic skiing |
| 119 | Chores |
| 120 | Pilates |
| 121 | New yoga |
