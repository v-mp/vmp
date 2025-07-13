#pragma once

#include <HostSharedData.h>
#include <CfxState.h>

#include <shellapi.h>

namespace fx
{
namespace client
{
inline int GetPureLevel()
{
	static int pureLevel = -1;

	if (pureLevel != -1)
	{
		return pureLevel;
	}

	auto sharedData = CfxState::Get();
	std::wstring_view cli = (sharedData->initCommandLine[0]) ? sharedData->initCommandLine : GetCommandLineW();
	pureLevel = 0;

	int argc;
	wchar_t** wargv = CommandLineToArgvW(cli.data(), &argc);
	bool found = false;
	for (int i = 1; i < argc; i++)
	{
		std::wstring_view arg = wargv[i];
		size_t found = arg.find(L"pure_");
		if (found != std::wstring_view::npos)
		{
			pureLevel = _wtoi(&arg[found + 5]);
			found = true;
			break;
		}
	}
	LocalFree(wargv);

	if (!found)
	{
		std::wstring fpath = MakeRelativeCitPath(L"VMP.ini");

		auto tempPureLevel = GetPrivateProfileInt(L"Game", L"PureLevel", -1, fpath.c_str());

		if (tempPureLevel != -1)
		{
			pureLevel = tempPureLevel;
		}
	}

	return pureLevel;
}
}
}
