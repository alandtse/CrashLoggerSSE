#include "PCH.h"

#define NOGDICAPMASKS
#define NOVIRTUALKEYCODES
#define NOWINMESSAGES
#define NOWINSTYLES
#define NOSYSMETRICS
#define NOMENUS
#define NOICONS
#define NOKEYSTATES
#define NOSYSCOMMANDS
#define NORASTEROPS
#define NOSHOWWINDOW
#define OEMRESOURCE
#define NOATOM
#define NOCLIPBOARD
#define NOCOLOR
#define NOCTLMGR
#define NODRAWTEXT
#define NOGDI
#define NOKERNEL
#define NOUSER
#define NONLS
#define NOMB
#define NOMEMMGR
#define NOMETAFILE
#define NOMSG
#define NOOPENFILE
#define NOSCROLL
#define NOSERVICE
#define NOSOUND
#define NOTEXTMETRIC
#define NOWH
#define NOWINOFFSETS
#define NOCOMM
#define NOKANJI
#define NOHELP
#define NOPROFILER
#define NODEFERWINDOWPOS
#define NOMCX

#include <Windows.h>

#include <dbghelp.h>

namespace WinAPI
{
	bool IsDebuggerPresent() noexcept
	{
		return static_cast<bool>(
			::IsDebuggerPresent());
	}

	std::uint32_t UnDecorateSymbolName(
		const char* a_name,
		char* a_outputString,
		std::uint32_t a_maxStringLength,
		std::uint32_t a_flags) noexcept
	{
		return static_cast<std::uint32_t>(
			::UnDecorateSymbolName(
				static_cast<::PCSTR>(a_name),
				static_cast<::PSTR>(a_outputString),
				static_cast<::DWORD>(a_maxStringLength),
				static_cast<::DWORD>(a_flags)));
	}
}
