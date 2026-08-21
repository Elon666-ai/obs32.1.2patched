#include "ppcenter-client.h"

#include <util/dstr.h>
#include "whip-utils.h"

#include <util/curl/curl-helper.h>

#include <nlohmann/json.hpp>

bool ppcenter_fetch_publish_token(const std::string &ppcenter_url, const std::string &app_id,
				   const std::string &app_secret, const std::string &stream_name,
				   const std::string &request_region, const std::string &device_uuid,
				   const std::string &user_agent_header, PPCenterPublishToken &out,
				   std::string &error)
{
	nlohmann::json body = {
		{"appId", app_id},
		{"appSecret", app_secret},
		{"streamName", stream_name},
		{"deviceId", device_uuid},
	};
	if (!request_region.empty())
		body["requestRegion"] = request_region;
	std::string body_str = body.dump();

	std::string url = ppcenter_url;
	while (!url.empty() && url.back() == '/')
		url.pop_back();
	url += "/v1/publish/requests";

	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	if (!user_agent_header.empty())
		headers = curl_slist_append(headers, user_agent_header.c_str());

	std::string read_buffer;
	char error_buffer[CURL_ERROR_SIZE] = {};

	CURL *c = curl_easy_init();
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_writefunction);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, (void *)&read_buffer);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	curl_easy_setopt(c, CURLOPT_POST, 1L);
	curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS, body_str.c_str());
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, error_buffer);

	CURLcode res = curl_easy_perform(c);
	if (res != CURLE_OK) {
		error = error_buffer[0] ? error_buffer : curl_easy_strerror(res);
		curl_easy_cleanup(c);
		curl_slist_free_all(headers);
		return false;
	}

	long response_code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &response_code);
	curl_easy_cleanup(c);
	curl_slist_free_all(headers);

	if (response_code != 200) {
		try {
			auto j = nlohmann::json::parse(read_buffer);
			error = "ppcenter returned HTTP " + std::to_string(response_code) + ": " +
				j.value("message", read_buffer);
		} catch (const std::exception &) {
			error = "ppcenter returned HTTP " + std::to_string(response_code);
		}
		return false;
	}

	try {
		auto j = nlohmann::json::parse(read_buffer);
		out.whip_url = j.value("whipUrl", std::string());
		out.bearer_token = j.value("bearerToken", std::string());
	} catch (const std::exception &e) {
		error = std::string("failed to parse ppcenter response: ") + e.what();
		return false;
	}

	if (out.whip_url.empty() || out.bearer_token.empty()) {
		error = "ppcenter response is missing whipUrl/bearerToken";
		return false;
	}

	return true;
}
