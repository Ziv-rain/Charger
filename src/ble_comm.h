#pragma once

#include <Arduino.h>

void initBLE();
bool bleIsConnected();
void blePollCommand();
void bleUpdateAndNotify();
