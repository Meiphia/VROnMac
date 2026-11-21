#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace vrcm {

static std::string ModuleDir()
{
	char path[MAX_PATH] = {};
	GetModuleFileNameA((HMODULE)&__ImageBase, path, MAX_PATH);
	std::string s(path);
	size_t slash = s.find_last_of("\\/");
	return slash == std::string::npos ? std::string(".") : s.substr(0, slash);
}

static std::string Trim(const std::string& s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	size_t b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

static Config LoadConfig()
{
	Config c;
	c.dllDir = ModuleDir();

	std::string iniPath = c.dllDir + "\\vrconmac.ini";
	if (FILE* f = fopen(iniPath.c_str(), "r")) {
		char line[512];
		while (fgets(line, sizeof(line), f)) {
			std::string l = Trim(line);
			if (l.empty() || l[0] == '#' || l[0] == ';' || l[0] == '[')
				continue;
			size_t eq = l.find('=');
			if (eq == std::string::npos)
				continue;
			std::string k = Trim(l.substr(0, eq)), v = Trim(l.substr(eq + 1));
			if (k == "host") c.host = v;
			else if (k == "port") c.port = atoi(v.c_str());
			else if (k == "connect_timeout_ms") c.connectTimeoutMs = atoi(v.c_str());
			else if (k == "frame_latency") c.frameLatency = atoi(v.c_str());
			else if (k == "verbose") c.logVerbose = (v == "1" || v == "true");
		}
		fclose(f);
	}

	if (const char* e = getenv("VRCONMAC_HOST")) c.host = e;
	if (const char* e = getenv("VRCONMAC_PORT")) c.port = atoi(e);
	if (const char* e = getenv("VRCONMAC_TRACKER_PORT")) {
		int port=atoi(e); if(port>0 && port<=65535) c.trackerPort=port;
	}
	if (const char* e = getenv("VRCONMAC_FRAME_LATENCY")) c.frameLatency = atoi(e);
	if (const char* e = getenv("VRCONMAC_VERBOSE")) c.logVerbose = (e[0] == '1');
	if (c.frameLatency < 0) c.frameLatency = 0;
	if (c.frameLatency > 1) c.frameLatency = 1;
	return c;
}

const Config& GetConfig()
{
	static Config cfg = LoadConfig();
	return cfg;
}

void Log(const char* fmt, ...)
{
	static std::mutex mtx;
	static FILE* file = nullptr;
	static bool tried = false;

	char msg[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	std::lock_guard<std::mutex> lock(mtx);
	if (!tried) {
		tried = true;
		std::string p = ModuleDir() + "\\vrconmac_xr.log";
		file = fopen(p.c_str(), "w");
	}
	SYSTEMTIME st;
	GetLocalTime(&st);
	char line[2200];
	snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
	if (file) {
		fputs(line, file);
		fflush(file);
	}
	OutputDebugStringA(line);
}

int64_t NowNs()
{
	static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return (int64_t)((double)c.QuadPart * 1e9 / (double)freq.QuadPart);
}

} // namespace vrcm
