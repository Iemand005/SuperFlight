# SuperFlight

Lightweight VR slight simulator game with joystick support on Quest Native!

TODO: I want to add shooting, async chunk loading, self made plane models. Also audio for the drone noises and plane noises maybe, I want a generator that incraesases oudio putch to like cahgne the sound depending on how fast the drone flies.

Table top mode

I want a table top mode where you can view a replay of the planes n a table, for scaling the area I can make a bounding box visible in 3D space where the VR user can use their controllers to push or pull the walls one by one to adjust the boundaries of the sample area, tho it might have aspect ratio preserve as well but idk yet how i'm gonna defiend the bounds, woah maybe I can make the bounds dynamic but like only load chunks inside the bounds and the like terrain moves around tbut the plane/player/drone thing stays in the center and you can see the terrain the plane replay fly arouond and shoot and stuff.

I will need the already existing state struct to be logged to a replay log file, and a new kind of struct for like maybe smoke particles that the plane can draw, or shoot events, maybe an event object and like an enum then? where the re is a union in C++ where the union has a value that says what the type is and using that type param in the struct you can go into a union in the struct which then contains ether the state struct from the player object, or the uh enum of an event like a shoot or rail stap and stop I mean trail start and stop of liek smoke. Or landing gear in out as well? yeah! part of the enum so let's uh start working on it!!!