/*
	DCNet access point services.
    Copyright (C) 2026 Flyinghead <flyinghead.github@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <curl/curl.h>
#include "json.hpp"
using namespace nlohmann;

static const char *URL;
#define DEFAULT_URL "http://172.20.0.1:8081/dcnet";

static size_t receiveData(void *buffer, size_t size, size_t nmemb, void *arg) {
	return nmemb * size;
}

static void post(const std::string& url, const json& payload)
{
	std::string body = payload.dump(4, ' ', false, json::error_handler_t::replace);

	CURL *curl = curl_easy_init();
	if (curl == nullptr) {
		fprintf(stderr, "can't create curl handle\n");
		return;
	}
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "DCNet-AP");
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receiveData);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10);	// default is 300 s
	curl_slist *headers = curl_slist_append(nullptr, "Content-Type: application/json");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.size());

	CURLcode res = curl_easy_perform(curl);
	curl_slist_free_all(headers);
	if (res != CURLE_OK) {
		fprintf(stderr, "curl error: %d\n", res);
	}
	else
	{
		long code;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
		if (code < 200 || code >= 300)
			fprintf(stderr, "HTTP error %ld\n", code);
	}
	curl_easy_cleanup(curl);
}

static std::string getUrl()
{
	if (URL == nullptr)
		URL = getenv("DCNET_LOGIN_URL");
	if (URL == nullptr)
		URL = DEFAULT_URL;
	return URL;
}

extern "C"
void dcnetConnect(const char *userName, const char *publicIp, int port, const char *dcnetIp)
{
	json jnotif = {
		{ "userName", userName == nullptr ? "?" : userName },
		{ "publicIp", publicIp },
		{ "publicPort", port },
		{ "dcnetIp", dcnetIp },
	};
	post(getUrl() + "/connect", jnotif);
}

extern "C"
void dcnetDisconnect(const char *dcnetIp)
{
	json jnotif = {
		{ "dcnetIp", dcnetIp },
	};
	post(getUrl() + "/disconnect", jnotif);
}
