#pragma once

void initApiServer();
void handleApiServer();  // call from loop()
bool isApiServerRunning();
bool isQueueEnabled();
void factoryResetCredentials();
