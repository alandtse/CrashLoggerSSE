#pragma once

namespace Crash
{
	namespace Modules
	{
		namespace detail
		{
			class Factory;
		}

		class Module
		{
		public:
			virtual ~Module() noexcept = default;

			[[nodiscard]] std::uintptr_t address() const noexcept { return reinterpret_cast<std::uintptr_t>(_image.data()); }

			[[nodiscard]] std::string frame_info(const boost::stacktrace::frame& a_frame) const;

			// Return std::string of assembly for a_ptr
			[[nodiscard]] std::string assembly(const void* a_ptr) const;

			[[nodiscard]] bool in_range(const void* a_ptr) const noexcept
			{
				const auto ptr = reinterpret_cast<const std::byte*>(a_ptr);
				return _image.data() <= ptr && ptr < _image.data() + _image.size();
			}

			[[nodiscard]] bool in_data_range(const void* a_ptr) const noexcept
			{
				const auto ptr = reinterpret_cast<const std::byte*>(a_ptr);
				return _data.data() <= ptr && ptr < _data.data() + _data.size();
			}

			[[nodiscard]] bool in_rdata_range(const void* a_ptr) const noexcept
			{
				const auto ptr = reinterpret_cast<const std::byte*>(a_ptr);
				return _rdata.data() <= ptr && ptr < _rdata.data() + _rdata.size();
			}

			[[nodiscard]] std::string_view name() const { return _name; }
			[[nodiscard]] std::string_view path() const { return _path; }

			[[nodiscard]] const RE::msvc::type_info* type_info() const { return _typeInfo; }

		protected:
			friend class detail::Factory;

			Module(std::string a_name, std::span<const std::byte> a_image);
			Module(std::string a_name, std::span<const std::byte> a_image, std::string a_path);
			[[nodiscard]] virtual std::string get_frame_info(const boost::stacktrace::frame& a_frame) const;

		private:
			std::string _name;
			std::span<const std::byte> _image;
			std::span<const std::byte> _data;
			std::span<const std::byte> _rdata;
			const RE::msvc::type_info* _typeInfo{ nullptr };
			std::string _path;
		};

		[[nodiscard]] auto get_loaded_modules()
			-> std::vector<std::unique_ptr<Module>>;
	}

	using module_pointer = std::unique_ptr<Modules::Module>;
}
