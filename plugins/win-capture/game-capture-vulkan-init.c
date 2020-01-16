#include <windows.h>

#include <obs-module.h>

#include <util/windows/win-registry.h>
#include <util/platform.h>

#define IMPLICIT_LAYERS L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers"

static bool has_elevation()
{
	SID_IDENTIFIER_AUTHORITY sia = SECURITY_NT_AUTHORITY;
	PSID sid = NULL;
	BOOL elevated = false;
	BOOL success;

	success = AllocateAndInitializeSid(&sia, 2, SECURITY_BUILTIN_DOMAIN_RID,
					   DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0,
					   0, 0, &sid);
	if (success && sid) {
		CheckTokenMembership(NULL, sid, &elevated);
		FreeSid(sid);
	}

	return elevated;
}

#define warn(format, ...) \
	blog(LOG_WARNING, "%s: " format, "[Vulkan Capture Init]", ##__VA_ARGS__)

/* Sets vulkan layer registry if it doesn't already exist */
void init_vulkan_capture(void)
{
	char *file = obs_module_file("obs-vulkan64.json");
	wchar_t *wfile = NULL;
	HKEY key = NULL;
	LSTATUS s;

	if (!file) {
		warn("Failed to find obs-vulkan64.json");
		return;
	}

	char *abs_path = os_get_abs_path_ptr(file);
	os_utf8_to_wcs_ptr(abs_path, 0, &wfile);
	bfree(file);
	bfree(abs_path);

	struct reg_dword ret;
	get_reg_dword(HKEY_LOCAL_MACHINE, IMPLICIT_LAYERS, wfile, &ret);

	if (ret.status == ERROR_FILE_NOT_FOUND) {
		get_reg_dword(HKEY_CURRENT_USER, IMPLICIT_LAYERS, wfile, &ret);

		if (ret.status != ERROR_FILE_NOT_FOUND &&
		    ret.status != ERROR_SUCCESS) {
			warn("Failed to query registry keys: %d",
			     (int)ret.status);
			goto finish;
		}

	} else if (ret.status != ERROR_SUCCESS) {
		warn("Failed to query registry keys: %d", (int)ret.status);
		goto finish;
	}

	if (ret.status == ERROR_SUCCESS) {
		goto finish;
	}

	HKEY type = has_elevation() ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
	DWORD temp;

	s = RegCreateKeyExW(type, IMPLICIT_LAYERS, 0, NULL, 0, KEY_WRITE, NULL,
			    &key, &temp);
	if (s != ERROR_SUCCESS) {
		warn("Failed to create registry key");
		goto finish;
	}

	DWORD zero = 0;
	s = RegSetValueExW(key, wfile, 0, REG_DWORD, (const BYTE *)&zero,
			   sizeof(zero));
	if (s != ERROR_SUCCESS) {
		warn("Failed to set registry value");
		goto finish;
	}

finish:
	if (key)
		RegCloseKey(key);
	bfree(wfile);
}
