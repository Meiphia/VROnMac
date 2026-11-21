#include "xr_loader.h"

#include <openxr/openxr_loader_negotiation.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <vector>

std::string DefaultRuntimeManifest()
{
	if (const char* e = getenv("XR_RUNTIME_JSON"); e && *e)
		return e;
	if (const char* home = getenv("HOME"))
		return std::string(home) + "/.config/openxr/1/active_runtime.json";
	return {};
}

// Minimal extraction of "library_path" and "name" from the manifest (avoids a JSON dependency).
static std::string JsonString(const std::string& doc, const std::string& key)
{
	size_t k = doc.find("\"" + key + "\"");
	if (k == std::string::npos)
		return {};
	size_t colon = doc.find(':', k);
	size_t q1 = doc.find('"', colon + 1);
	if (colon == std::string::npos || q1 == std::string::npos)
		return {};
	std::string out;
	for (size_t i = q1 + 1; i < doc.size(); i++) {
		char c = doc[i];
		if (c == '\\' && i + 1 < doc.size()) {
			out += doc[++i];
			continue;
		}
		if (c == '"')
			break;
		out += c;
	}
	return out;
}

static std::string DirName(const std::string& p)
{
	size_t s = p.find_last_of('/');
	return s == std::string::npos ? std::string(".") : p.substr(0, s);
}

bool XrApi::Load(const std::string& manifestPath, std::string& error)
{
	std::ifstream in(manifestPath);
	if (!in) {
		error = "cannot read runtime manifest '" + manifestPath + "'";
		return false;
	}
	std::stringstream ss;
	ss << in.rdbuf();
	std::string doc = ss.str();
	std::string lib = JsonString(doc, "library_path");
	runtimeName = JsonString(doc, "name");
	if (lib.empty()) {
		error = "no library_path in " + manifestPath;
		return false;
	}

	// Relative library paths are relative to the manifest. Try the path as given (the
	// registration may be a symlink) and the symlink target's directory.
	std::vector<std::string> candidates;
	if (lib[0] == '/') {
		candidates.push_back(lib);
	} else {
		candidates.push_back(DirName(manifestPath) + "/" + lib);
		char real[PATH_MAX];
		if (realpath(manifestPath.c_str(), real))
			candidates.push_back(DirName(real) + "/" + lib);
	}
	std::string tried;
	for (const std::string& c : candidates) {
		library = dlopen(c.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (library)
			break;
		tried += "\n  " + c + ": " + dlerror();
	}
	if (!library) {
		error = "dlopen failed:" + tried;
		return false;
	}

	auto negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)dlsym(library, "xrNegotiateLoaderRuntimeInterface");
	if (!negotiate) {
		error = "runtime has no xrNegotiateLoaderRuntimeInterface";
		return false;
	}
	XrNegotiateLoaderInfo li{};
	li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
	li.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
	li.structSize = sizeof(li);
	li.minInterfaceVersion = 1;
	li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
	li.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
	li.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
	XrNegotiateRuntimeRequest rr{};
	rr.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
	rr.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
	rr.structSize = sizeof(rr);
	XrResult r = negotiate(&li, &rr);
	if (XR_FAILED(r) || !rr.getInstanceProcAddr) {
		error = "runtime negotiation failed (" + std::to_string(r) + ")";
		return false;
	}
	xrGetInstanceProcAddr = rr.getInstanceProcAddr;
	xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&xrCreateInstance);
	xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
	    (PFN_xrVoidFunction*)&xrEnumerateInstanceExtensionProperties);
	if (!xrCreateInstance || !xrEnumerateInstanceExtensionProperties) {
		error = "runtime did not provide global functions";
		return false;
	}
	printf("[xr] loaded runtime '%s' (API %u.%u.%u)\n", runtimeName.c_str(), XR_VERSION_MAJOR(rr.runtimeApiVersion),
	    XR_VERSION_MINOR(rr.runtimeApiVersion), XR_VERSION_PATCH(rr.runtimeApiVersion));
	return true;
}

bool XrApi::LoadInstanceFunctions(XrInstance instance, std::string& error)
{
#define VRCM_LOAD(name)                                                                   \
	if (!name) {                                                                          \
		xrGetInstanceProcAddr(instance, #name, (PFN_xrVoidFunction*)&name);               \
		if (!name) {                                                                      \
			error = "runtime is missing " #name;                                          \
			return false;                                                                 \
		}                                                                                 \
	}
	VRCM_XR_FUNCTIONS(VRCM_LOAD)
#undef VRCM_LOAD
	return true;
}

PFN_xrVoidFunction XrApi::Get(XrInstance instance, const char* name)
{
	PFN_xrVoidFunction fn = nullptr;
	xrGetInstanceProcAddr(instance, name, &fn);
	return fn;
}

std::string XrApi::ResultString(XrInstance inst, XrResult r)
{
	char buf[XR_MAX_RESULT_STRING_SIZE] = {};
	if (xrResultToString && inst && XR_SUCCEEDED(xrResultToString(inst, r, buf)))
		return buf;
	return std::to_string((int)r);
}
