#pragma once

class VideoCapture;
struct DriverBehavior;

void driver_lighting_init(VideoCapture *capture);
void driver_lighting_update(DriverBehavior &behavior);
const char *driver_lighting_state_name(unsigned state);
