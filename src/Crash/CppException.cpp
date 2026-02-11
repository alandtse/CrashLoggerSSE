#include "Crash/CppException.h"

#include <DbgHelp.h>
#include <array>
#include <cstring>

namespace Crash
{
	namespace
	{
		// Safely read memory with exception handling
		template <typename T>
		bool SafeRead(std::uintptr_t address, T& out) noexcept
		{
			__try {
				out = *reinterpret_cast<const T*>(address);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// Try to demangle a C++ type name using UnDecorateSymbolName
		std::string DemangleTypeName(const char* mangledName) noexcept
		{
			try {
				if (!mangledName || mangledName[0] == '\0') {
					return "<unknown type>";
				}

				// UnDecorateSymbolName expects the name without the leading '.'
				const char* nameStart = mangledName;
				if (nameStart[0] == '.') {
					++nameStart;
				}

				std::array<char, 1024> buffer{};
				const auto result = UnDecorateSymbolName(
					nameStart,
					buffer.data(),
					static_cast<DWORD>(buffer.size()),
					UNDNAME_NAME_ONLY);  // Use UNDNAME_NAME_ONLY for cleaner output

				if (result != 0) {
					std::string demangled(buffer.data());

					// Remove leading "class ", "struct ", etc. if present
					const char* prefixes[] = { "class ", "struct ", "union ", "enum " };
					for (const auto* prefix : prefixes) {
						const size_t prefixLen = std::strlen(prefix);
						if (demangled.compare(0, prefixLen, prefix) == 0) {
							demangled = demangled.substr(prefixLen);
							break;
						}
					}

					return demangled;
				}

				// Fallback to mangled name if undecorating fails
				return std::string(mangledName);
			} catch (...) {
				// If any exception occurs (e.g., bad_alloc), return fallback
				return "<demangle failed>";
			}
		}

		// Extract type name from ThrowInfo structure
		// Based on: https://devblogs.microsoft.com/oldnewthing/20100730-00/?p=13273
		std::string ExtractTypeName(std::uintptr_t throwInfoAddress, std::uintptr_t moduleBase) noexcept
		{
			try {
				// ThrowInfo structure (x64, from ehdata.h):
				// struct ThrowInfo {
				//     DWORD attributes;
				//     DWORD pmfnUnwind;     // RVA of destructor (or 0)
				//     DWORD pForwardCompat; // RVA (unused)
				//     DWORD pCatchableTypeArray; // RVA of CatchableTypeArray
				// };

				std::uint32_t pCatchableTypeArrayRVA;
				if (!SafeRead(throwInfoAddress + 12, pCatchableTypeArrayRVA)) {
					return "<failed to read ThrowInfo>";
				}

				// Check for null RVA
				if (pCatchableTypeArrayRVA == 0) {
					return "<null CatchableTypeArray RVA>";
				}

				// Convert RVA to absolute address
				const std::uintptr_t pCatchableTypeArray = moduleBase + pCatchableTypeArrayRVA;

				// CatchableTypeArray structure:
				// struct CatchableTypeArray {
				//     int nCatchableTypes;
				//     DWORD arrayOfCatchableTypes[]; // Array of RVAs
				// };

				int nCatchableTypes;
				if (!SafeRead(pCatchableTypeArray, nCatchableTypes)) {
					return "<failed to read CatchableTypeArray count>";
				}

				if (nCatchableTypes <= 0 || nCatchableTypes > 100) {
					return "<invalid CatchableTypeArray count>";
				}

				// Read the first CatchableType RVA
				std::uint32_t firstCatchableTypeRVA;
				if (!SafeRead(pCatchableTypeArray + 4, firstCatchableTypeRVA)) {
					return "<failed to read CatchableType RVA>";
				}

				if (firstCatchableTypeRVA == 0) {
					return "<null CatchableType RVA>";
				}

				const std::uintptr_t pCatchableType = moduleBase + firstCatchableTypeRVA;

				// CatchableType structure (x64):
				// struct CatchableType {
				//     DWORD properties;
				//     DWORD pType; // RVA of type_info
				//     ... more fields ...
				// };

				std::uint32_t typeInfoRVA;
				if (!SafeRead(pCatchableType + 4, typeInfoRVA)) {
					return "<failed to read type_info RVA>";
				}

				if (typeInfoRVA == 0) {
					return "<null type_info RVA>";
				}

				const std::uintptr_t pTypeInfo = moduleBase + typeInfoRVA;

				// type_info structure:
				// class type_info {
				//     void* pVFTable;
				//     void* _M_spare;
				//     char _M_name[]; // Decorated name starts here
				// };

				std::uintptr_t nameAddress = pTypeInfo + 16;  // Skip pVFTable (8) + _M_spare (8)

				// Read the decorated name (null-terminated string)
				std::array<char, 512> decoratedName{};
				for (size_t i = 0; i < decoratedName.size() - 1; ++i) {
					if (!SafeRead(nameAddress + i, decoratedName[i])) {
						return "<failed to read type name>";
					}
					if (decoratedName[i] == '\0') {
						break;
					}
				}

				return DemangleTypeName(decoratedName.data());
			} catch (...) {
				// If any exception occurs (e.g., bad_alloc), return fallback
				return "<type extraction failed>";
			}
		}
	}

	bool IsCppException(const EXCEPTION_RECORD& exception) noexcept
	{
		return exception.ExceptionCode == CPP_EXCEPTION_CODE &&
		       exception.NumberParameters >= 3 &&
		       exception.ExceptionInformation[0] == CPP_EXCEPTION_MAGIC_X64;
	}

	std::optional<CppExceptionInfo> ParseCppException(const EXCEPTION_RECORD& exception) noexcept
	{
		if (!IsCppException(exception)) {
			return std::nullopt;
		}

		CppExceptionInfo info{};

		// Extract parameters
		// Parameter[0]: Magic number (already validated)
		// Parameter[1]: Pointer to exception object
		// Parameter[2]: Pointer to ThrowInfo
		// Parameter[3]: Module base address (x64 only)

		info.objectAddress = exception.ExceptionInformation[1];
		info.throwInfoAddress = exception.ExceptionInformation[2];

		if (exception.NumberParameters >= 4) {
			info.moduleBase = exception.ExceptionInformation[3];
		} else {
			// Should not happen on x64, but handle gracefully
			info.moduleBase = 0;
		}

		// Extract type name
		if (info.moduleBase != 0) {
			info.typeName = ExtractTypeName(info.throwInfoAddress, info.moduleBase);
		} else {
			info.typeName = "<module base not available>";
		}

		// Try to get exception message if it's a std::exception
		info.what = TryGetExceptionWhat(info.objectAddress);

		return info;
	}

	std::optional<std::string> TryGetExceptionWhat(std::uintptr_t objectAddress) noexcept
	{
		try {
			// Attempt to treat the object as a std::exception-derived class
			// This is dangerous and relies on the object being valid

			// First, try extracting std::exception::what() message (most common case)
			// std::exception layout (MSVC):
			// - vtable pointer (8 bytes)
			// - _Mywhat pointer (8 bytes) or inline storage

			// Get the vtable pointer
			std::uintptr_t vtablePtr;
			if (SafeRead(objectAddress, vtablePtr)) {
				// Get the what() function pointer from vtable
				// The what() function is typically at offset 8 in the vtable (after destructor)
				std::uintptr_t whatFuncPtr;
				if (SafeRead(vtablePtr + 8, whatFuncPtr)) {
					// We can't safely call what() directly because:
					// 1. The object might not be a std::exception
					// 2. Calling virtual functions from a crash handler is risky
					// Instead, try to read the _Mywhat member directly

					// For std::exception, the message is usually stored at offset 8 (after vtable)
					std::uintptr_t messagePtr;
					if (SafeRead(objectAddress + 8, messagePtr)) {
						// Check if messagePtr looks valid (not null, in reasonable range)
						if (messagePtr != 0 && messagePtr >= 0x1000) {
							// Try to read the string (with a reasonable limit)
							char messageBuffer[512];
							std::memset(messageBuffer, 0, sizeof(messageBuffer));

							bool validString = true;
							for (size_t i = 0; i < sizeof(messageBuffer) - 1; ++i) {
								if (!SafeRead(messagePtr + i, messageBuffer[i])) {
									validString = false;
									break;
								}
								if (messageBuffer[i] == '\0') {
									break;
								}
								// Sanity check: only printable ASCII characters and common whitespace
								const auto ch = static_cast<unsigned char>(messageBuffer[i]);
								if ((ch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r') || ch > 0x7E) {
									validString = false;
									break;
								}
							}

							if (validString && messageBuffer[0] != '\0') {
								return std::string(messageBuffer);
							}
						}
					}
				}
			}

			// If std::exception::what() extraction failed, try reading as DX::com_exception (DirectXTK exception type)
			// DX::com_exception layout varies but HRESULT is typically stored after the vtable and base class
			// Try multiple common offsets: 8, 16, 24, 32 (covering various std::exception implementations)
			// NOTE: We do this AFTER trying std::exception::what() to avoid false positives from pointer fragments
			const std::size_t hresultOffsets[] = { 8, 16, 24, 32 };

			for (auto offset : hresultOffsets) {
				std::int32_t hresult;
				if (SafeRead(objectAddress + offset, hresult)) {
					// Check if this looks like a valid HRESULT (high bit set for failures, common error codes)
					// HRESULT format: S (1 bit) | R (2 bits) | C (1 bit) | N (1 bit) | X (11 bits) | Facility (11 bits) | Code (16 bits)
					// Failure HRESULTs have the high bit set (0x80000000)
					if ((hresult & 0x80000000) != 0) {
						// Additional validation: check for common facility codes
						// FACILITY_WIN32 (7), FACILITY_WINDOWS (8), FACILITY_ITF (4), etc.
						const auto facility = (hresult >> 16) & 0x7FF;
						if (facility <= 0x200) {  // Reasonable facility range
							char hresultStr[128];
							// Try to provide a friendly name for common HRESULT values
							const char* friendlyName = "";
							switch (static_cast<std::uint32_t>(hresult)) {
							case 0x887A0005:
								friendlyName = " (DXGI_ERROR_DEVICE_REMOVED)";
								break;
							case 0x887A0006:
								friendlyName = " (DXGI_ERROR_DEVICE_HUNG)";
								break;
							case 0x887A0007:
								friendlyName = " (DXGI_ERROR_DEVICE_RESET)";
								break;
							case 0x80070057:
								friendlyName = " (E_INVALIDARG)";
								break;
							case 0x8007000E:
								friendlyName = " (E_OUTOFMEMORY)";
								break;
							case 0x80004001:
								friendlyName = " (E_NOTIMPL)";
								break;
							case 0x80004002:
								friendlyName = " (E_NOINTERFACE)";
								break;
							case 0x80004003:
								friendlyName = " (E_POINTER)";
								break;
							case 0x80004004:
								friendlyName = " (E_ABORT)";
								break;
							case 0x80004005:
								friendlyName = " (E_FAIL)";
								break;
							}
							std::snprintf(hresultStr, sizeof(hresultStr), "HRESULT 0x%08X%s",
								static_cast<std::uint32_t>(hresult), friendlyName);
							return std::string(hresultStr);
						}
					}
				}
			}

			return std::nullopt;
		} catch (...) {
			// If any exception occurs (e.g., bad_alloc), return nullopt
			return std::nullopt;
		}
	}
}
