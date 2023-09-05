#include "Crash/Modules/ModuleHandler.h"

//#define WIN32_LEAN_AND_MEAN

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
//#define NOUSER
#define NONLS
#define NOMB
#define NOMEMMGR
#define NOMETAFILE
//#define NOMINMAX
//#define NOMSG
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

#include "Crash/PDB/PdbHandler.h"
#include <Psapi.h>
#include <Zydis/Zydis.h>

#undef max
#undef min

namespace Crash::Modules
{
	namespace detail
	{
		class VTable
		{
		public:
			VTable(
				std::string_view a_name,
				std::span<const std::byte> a_module,
				std::span<const std::byte> a_data,
				std::span<const std::byte> a_rdata)
			{
				const auto typeDesc = type_descriptor(a_name, a_data);
				const auto col = typeDesc ? complete_object_locator(typeDesc, a_module, a_rdata) : nullptr;
				_vtable = col ? virtual_table(col, a_rdata) : nullptr;
			}

			[[nodiscard]] const void* get() const noexcept { return _vtable; }

		private:
			[[nodiscard]] static auto type_descriptor(
				std::string_view a_name,
				std::span<const std::byte> a_data)
				-> const RE::RTTI::TypeDescriptor*
			{
				constexpr std::size_t offset = 0x10;  // offset of name into type descriptor
				std::boyer_moore_horspool_searcher search(a_name.cbegin(), a_name.cend());
				const auto& [first, last] = search(
					reinterpret_cast<const char*>(a_data.data()),
					reinterpret_cast<const char*>(a_data.data() + a_data.size()));
				return first != last ?
				           reinterpret_cast<const RE::RTTI::TypeDescriptor*>(first - offset) :
				           nullptr;
			}

			[[nodiscard]] static auto complete_object_locator(
				const RE::RTTI::TypeDescriptor* a_typeDesc,
				std::span<const std::byte> a_module,
				std::span<const std::byte> a_rdata)
				-> const RE::RTTI::CompleteObjectLocator*
			{
				assert(a_typeDesc != nullptr);

				const auto typeDesc = reinterpret_cast<std::uintptr_t>(a_typeDesc);
				const auto rva = static_cast<std::uint32_t>(typeDesc - reinterpret_cast<std::uintptr_t>(a_module.data()));

				const auto offset = static_cast<std::size_t>(a_rdata.data() - a_module.data());
				const auto base = a_rdata.data();
				const auto start = reinterpret_cast<const std::uint32_t*>(base);
				const auto end = reinterpret_cast<const std::uint32_t*>(base + a_rdata.size());

				for (auto iter = start; iter < end; ++iter) {
					if (*iter == rva) {
						// both base class desc and col can point to the type desc so we check
						// the next int to see if it can be an rva to decide which type it is
						if ((iter[1] < offset) || (offset + a_rdata.size() <= iter[1])) {
							continue;
						}

						const auto ptr = reinterpret_cast<const std::byte*>(iter);
						const auto col = reinterpret_cast<const RE::RTTI::CompleteObjectLocator*>(ptr - offsetof(RE::RTTI::CompleteObjectLocator, typeDescriptor));
						if (col->offset != 0) {
							continue;
						}

						return col;
					}
				}

				return nullptr;
			}

			[[nodiscard]] static const void* virtual_table(
				const RE::RTTI::CompleteObjectLocator* a_col,
				std::span<const std::byte> a_rdata)
			{
				assert(a_col != nullptr);

				const auto col = reinterpret_cast<std::uintptr_t>(a_col);

				const auto base = a_rdata.data();
				const auto start = reinterpret_cast<const std::uintptr_t*>(base);
				const auto end = reinterpret_cast<const std::uintptr_t*>(base + a_rdata.size());

				for (auto iter = start; iter < end; ++iter) {
					if (*iter == col) {
						return iter + 1;
					}
				}

				return nullptr;
			}

			const void* _vtable{ nullptr };
		};

		class Fallout4 final :
			public Module
		{
		private:
			using super = Module;

		protected:
			friend class Factory;

			using super::super;

			[[nodiscard]] std::string get_frame_info(const boost::stacktrace::frame& a_frame) const override
			{
				const auto offset = reinterpret_cast<std::uintptr_t>(a_frame.address()) - address();
				const auto it = std::lower_bound(
					_offset2ID.rbegin(),
					_offset2ID.rend(),
					offset,
					[](auto&& a_lhs, auto&& a_rhs) noexcept {
						return a_lhs.offset >= a_rhs;
					});

				auto result = super::get_frame_info(a_frame);
				const auto assemblyStr = assembly(a_frame.address());
				if (it != _offset2ID.rend()) {
					result += fmt::format(
						" -> {}+0x{:X}"sv,
						it->id,
						offset - it->offset);
				}
				return fmt::format("{}\t{}", result, assemblyStr);
			}

		private:
			REL::IDDatabase::Offset2ID _offset2ID{ std::execution::par_unseq };
		};

		class Factory
		{
		public:
			[[nodiscard]] static std::unique_ptr<Module> create(::HMODULE a_module)
			{
				using result_t = std::unique_ptr<Module>;

				auto name = get_name(a_module);
				const auto image = get_image(a_module);
				const auto path = get_path(a_module);
				if (_stricmp(name.c_str(), util::module_name().c_str()) == 0) {
					return result_t{ new Fallout4(std::move(name), image, std::move(path)) };
				} else {
					return result_t{ new Module(std::move(name), image, std::move(path)) };
				}
			}

		private:
			[[nodiscard]] static std::span<const std::byte> get_image(::HMODULE a_module)
			{
				const auto dosHeader = reinterpret_cast<const ::IMAGE_DOS_HEADER*>(a_module);
				const auto ntHeader = util::adjust_pointer<::IMAGE_NT_HEADERS64>(dosHeader, dosHeader->e_lfanew);
				return { reinterpret_cast<const std::byte*>(a_module), ntHeader->OptionalHeader.SizeOfImage };
			}

			[[nodiscard]] static std::string get_name(::HMODULE a_module)
			{
				std::vector<wchar_t> buf;
				buf.reserve(MAX_PATH);
				buf.resize(MAX_PATH / 2);
				std::uint32_t result = 0;
				do {
					buf.resize(buf.size() * 2);
					result = ::GetModuleFileNameW(
						a_module,
						buf.data(),
						static_cast<std::uint32_t>(buf.size()));
				} while (result && result == buf.size() && buf.size() <= std::numeric_limits<std::uint32_t>::max());
				const std::filesystem::path p = buf.data();
				return p.filename().generic_string();
			}

			[[nodiscard]] static std::string get_path(::HMODULE a_module)
			{
				std::vector<wchar_t> buf;
				buf.reserve(MAX_PATH);
				buf.resize(MAX_PATH / 2);
				std::uint32_t result = 0;
				do {
					buf.resize(buf.size() * 2);
					result = ::GetModuleFileNameW(
						a_module,
						buf.data(),
						static_cast<std::uint32_t>(buf.size()));
				} while (result && result == buf.size() && buf.size() <= std::numeric_limits<std::uint32_t>::max());
				const std::filesystem::path p = buf.data();
				return p.generic_string();
			}
		};
	}

	std::string Module::frame_info(const boost::stacktrace::frame& a_frame) const
	{
		assert(in_range(a_frame.address()));
		return get_frame_info(a_frame);
	}

	std::string Module::assembly(const void* a_ptr) const
	{
		// Zydis code from https://github.com/zyantific/zydis/blob/214536a814ba20d2e33d2a907198d1a329aac45c/examples/DisassembleSimple.c#L38-L63 under MIT

		ZyanUSize offset = 0;
		ZyanU8 data[8];
		ZyanU64 runtime_address = (ZyanU64)a_ptr;
		memcpy(data, (const void*)runtime_address, sizeof(data));
		std::string assembly = "";
		ZydisDisassembledInstruction instruction;
		if (ZYAN_SUCCESS(ZydisDisassembleIntel(
				/* machine_mode:    */ ZYDIS_MACHINE_MODE_LONG_64,
				/* runtime_address: */ runtime_address,
				/* buffer:          */ data + offset,
				/* length:          */ sizeof(data) - offset,
				/* instruction:     */ &instruction))) {
			assembly = std::format("{}", instruction.text);
		}
		return assembly;
	}

	Module::Module(std::string a_name, std::span<const std::byte> a_image, std::string a_path) :
		_name(std::move(a_name)),
		_image(a_image),
		_path(std::move(a_path))
	{
		auto dosHeader = reinterpret_cast<const ::IMAGE_DOS_HEADER*>(_image.data());
		auto ntHeader = util::adjust_pointer<::IMAGE_NT_HEADERS64>(dosHeader, dosHeader->e_lfanew);
		std::span sections(
			IMAGE_FIRST_SECTION(ntHeader),
			ntHeader->FileHeader.NumberOfSections);

		const std::array todo{
			std::make_pair(".data"sv, std::ref(_data)),
			std::make_pair(".rdata"sv, std::ref(_rdata)),
		};
		for (auto& [name, section] : todo) {
			const auto it = std::find_if(
				sections.begin(),
				sections.end(),
				[&](auto&& a_elem) {
					constexpr auto size = std::extent_v<decltype(a_elem.Name)>;
					const auto len = std::min(name.size(), size);
					return std::memcmp(name.data(), a_elem.Name, len) == 0;
				});
			if (it != sections.end()) {
				section = std::span{ it->VirtualAddress + _image.data(), it->SizeOfRawData };
			}
		}

		if (!_image.empty() &&
			!_data.empty() &&
			!_rdata.empty()) {
			detail::VTable v{ ".?AVtype_info@@"sv, _image, _data, _rdata };
			_typeInfo = static_cast<const RE::msvc::type_info*>(v.get());
		}
	}

	std::string Module::get_frame_info(const boost::stacktrace::frame& a_frame) const
	{
		const auto offset = reinterpret_cast<std::uintptr_t>(a_frame.address()) - address();
		const auto assembly = this->assembly(a_frame.address());
		const auto pdbDetails = Crash::PDB::pdb_details(path(), offset);
		if (!pdbDetails.empty())
			return fmt::format(
				"+{:07X}\t{} | {}"sv,
				offset,
				assembly,
				pdbDetails);
		return fmt::format(
			"+{:07X}"sv,
			offset);
	}

	auto get_loaded_modules()
		-> std::vector<std::unique_ptr<Module>>
	{
		const auto proc = ::GetCurrentProcess();
		std::vector<::HMODULE> modules;
		std::uint32_t needed = 0;
		do {
			modules.resize(needed / sizeof(::HMODULE));
			::K32EnumProcessModules(
				proc,
				modules.data(),
				static_cast<::DWORD>(modules.size() * sizeof(::HMODULE)),
				reinterpret_cast<::DWORD*>(&needed));
		} while ((modules.size() * sizeof(::HMODULE)) < needed);

		decltype(get_loaded_modules()) results;
		results.resize(modules.size());
		std::for_each(
			std::execution::par_unseq,
			modules.begin(),
			modules.end(),
			[&](auto&& a_elem) {
				const auto pos = std::addressof(a_elem) - modules.data();
				results[pos] = detail::Factory::create(a_elem);
			});
		std::sort(
			results.begin(),
			results.end(),
			[](auto&& a_lhs, auto&& a_rhs) noexcept {
				return a_lhs->address() < a_rhs->address();
			});

		return results;
	}
}
