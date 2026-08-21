#pragma once

#include <string>

// Returns this machine's persistent ppobs device identity, generating and
// caching it (via obs_module_config_path) on first use so it survives
// restarts. Format: <3-digit-random><disk-serial><"ppobs"><3-digit-random>,
// e.g. "0471A2B3C4Dppobs829" - embedded as the "deviceId" claim in the
// ppcenter-issued WHIP publish token (see ppcenter-client.h and
// docs/obs-whip-publish-auth-protocol.md).
std::string ppobs_get_device_uuid();
