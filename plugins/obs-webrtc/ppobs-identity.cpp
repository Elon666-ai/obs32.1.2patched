#include "ppobs-identity.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <cstdio>
#include <fstream>
#include <random>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::string generate_random_digits(int count)
{
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<int> dist(0, 9);
	std::string out;
	for (int i = 0; i < count; i++)
		out += static_cast<char>('0' + dist(gen));
	return out;
}

// Returns the system drive's volume serial number as 8 uppercase hex
// digits (e.g. "1A2B3C4D") - a stable per-installation identifier that
// survives reboots and doesn't require WMI/COM.
std::string get_disk_serial()
{
#ifdef _WIN32
	wchar_t systemDir[MAX_PATH] = {};
	if (GetSystemWindowsDirectoryW(systemDir, MAX_PATH) == 0)
		return "00000000";

	wchar_t rootPath[4] = {systemDir[0], L':', L'\\', 0};

	DWORD serial = 0;
	if (!GetVolumeInformationW(rootPath, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0))
		return "00000000";

	char buf[9];
	snprintf(buf, sizeof(buf), "%08X", static_cast<unsigned int>(serial));
	return std::string(buf);
#else
	return "00000000";
#endif
}

std::string generate_device_uuid()
{
	return generate_random_digits(3) + get_disk_serial() + "ppobs" + generate_random_digits(3);
}

std::string trim_newline(std::string s)
{
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
		s.pop_back();
	return s;
}

} // namespace

std::string ppobs_get_device_uuid()
{
	char *path_c = obs_module_config_path("ppobs-device-id.txt");
	std::string path = path_c ? path_c : "";
	bfree(path_c);

	if (!path.empty()) {
		std::ifstream in(path);
		if (in.good()) {
			std::string existing;
			std::getline(in, existing);
			existing = trim_newline(existing);
			if (!existing.empty())
				return existing;
		}
	}

	std::string uuid = generate_device_uuid();

	if (!path.empty()) {
		char *dir_c = obs_module_config_path("");
		if (dir_c) {
			os_mkdirs(dir_c);
			bfree(dir_c);
		}
		std::ofstream out(path, std::ios::trunc);
		if (out.good())
			out << uuid;
	}

	return uuid;
}
