#include "Crash/Introspection/Introspection.h"

#include "Crash/Modules/ModuleHandler.h"
#include "Crash/PDB/PdbHandler.h"
#define MAGIC_ENUM_RANGE_MAX 256
#include <DbgHelp.h>
#include <SKSE/Logger.h>
#include <atomic>
#include <magic_enum/magic_enum.hpp>
#include <unordered_map>

namespace Crash::Introspection::SSE
{
	using filter_results = std::vector<std::pair<std::string, std::string>>;

	[[nodiscard]] std::string quoted(std::string_view a_str)
	{
		return fmt::format("\"{}\""sv, a_str);
	}

	template <class T = RE::TESForm>
	class TESForm
	{
	public:
		using value_type = T;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto form = static_cast<const value_type*>(a_ptr);

			try {
				const auto file = form->GetDescriptionOwnerFile();
				const auto filename = file ? file->GetFilename() : ""sv;
				if (!filename.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}File"sv,
							"",
							tab_depth),
						quoted(filename));
			} catch (...) {}

			try {
				auto sourcefiles = form->sourceFiles.array;
				if (sourcefiles && sourcefiles->size() > 1) {
					std::string filesString = "";
					for (auto index = 0; index < sourcefiles->size(); index++) {
						auto sourcefile = sourcefiles->data()[index];
						filesString = filesString.empty() ? fmt::format("{}"sv,
																sourcefile->GetFilename().data()) :
						                                    fmt::format("{} -> {}"sv,
																filesString, sourcefile->GetFilename().data());
					}
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Modified by"sv,
							"",
							tab_depth),
						filesString);
				}
			} catch (...) {}

			try {
				const auto formFlags = form->GetFormFlags();
				using recordFlags = value_type::RecordFlags::RecordFlag;
				std::string flagString = "";
				constexpr auto flagEntries = magic_enum::enum_entries<recordFlags>();
				for (const auto& entry : flagEntries) {
					const auto flag = entry.first;
					const auto flagName = entry.second;
					if (flag & formFlags)
						flagString = flagString.empty() ?
						                 flagName :
						                 flagString.append(" | "sv).append(flagName);
				}
				a_results.emplace_back(
					fmt::format(
						"{:\t>{}}Flags"sv,
						"",
						tab_depth),
					fmt::format(
						"0x{:08X} {}"sv,
						formFlags,
						flagString));
			} catch (...) {}

			try {
				const auto name = form->GetName();
				if (name && name[0])
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Name"sv,
							"",
							tab_depth),
						quoted(name));
			} catch (...) {}

			try {
				const auto editorID = form->GetFormEditorID();
				if (editorID && editorID[0])
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}EditorID"sv,
							"",
							tab_depth),
						quoted(editorID));
			} catch (...) {}

			try {
				const auto formID = form->GetFormID();
				a_results.emplace_back(
					fmt::format(
						"{:\t>{}}FormID"sv,
						"",
						tab_depth),
					fmt::format(
						"0x{:08X}"sv,
						formID));
			} catch (...) {}

			try {
				const auto formType = form->GetFormType();
				const auto formTypeName = magic_enum::enum_name(formType);
				if (!formTypeName.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}FormType"sv,
							"",
							tab_depth),
						fmt::format(
							"{} ({:02})"sv,
							formTypeName, std::to_underlying(formType)));
			} catch (...) {}
		}
	};

	class TESFullName
	{
	public:
		using value_type = RE::TESFullName;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);

			if (!object)
				return;
			try {
				const auto name = object->GetFullName();
				if (name && name[0])
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}GetFullName"sv,
							"",
							tab_depth),
						quoted(name));
			} catch (...) {}
		}
	};

	class ActorKnowledge
	{
	public:
		using value_type = RE::ActorKnowledge;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);

			if (!object)
				return;
			try {
				const auto owner = object->owner.get().get();
				if (owner) {
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Owner"sv,
							"",
							tab_depth),
						"---");
					TESForm<RE::Actor>::filter(a_results, owner, tab_depth + 1);
				}
			} catch (...) {}
			try {
				const auto target = object->target.get().get();
				if (target) {
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Target"sv,
							"",
							tab_depth),
						"---");
					TESForm<RE::Actor>::filter(a_results, target, tab_depth + 1);
				}
			} catch (...) {}
		}
	};

	class BSShaderProperty
	{
	public:
		using value_type = RE::BSShaderProperty;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto form = static_cast<const value_type*>(a_ptr);

			try {
				const auto formFlags = form->flags;
				a_results.emplace_back(
					fmt::format(fmt::runtime(
									"{:\t>{}}Flags"sv),
						"",
						tab_depth),
					fmt::format(fmt::runtime(
									"0x{:08X}"sv),
						(std::uint64_t)formFlags.get()));
			} catch (...) {}
			try {
				const auto name = form->name.c_str();
				if (name && name[0])
					a_results.emplace_back(
						fmt::format(fmt::runtime(
										"{:\t>{}}Name"sv),
							"",
							tab_depth),
						quoted(name));
			} catch (...) {}
			try {
				const auto rttiname = form->GetRTTI() ? form->GetRTTI()->GetName() : ""sv;
				if (!rttiname.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}RTTIName"sv,
							"",
							tab_depth),
						quoted(rttiname));
			} catch (...) {}
			try {
				const auto formType = form->GetType();
				const auto formTypeName = magic_enum::enum_name(formType);
				if (!formTypeName.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}NiPropertyType"sv,
							"",
							tab_depth),
						fmt::format(
							"{} ({:02})"sv,
							formTypeName,
							std::to_underlying(formType)));
			} catch (...) {}
			try {
				for (auto i = 0; i < form->GetExtraDataSize(); i++) {
					const auto extraData = form->GetExtraDataAt(i);
					if (!extraData->GetName().empty()) {
						const auto name = extraData->GetName().c_str();
						if (name && name[0])
							a_results.emplace_back(
								fmt::format(
									"{:\t>{}}ExtraData[{}] Name"sv,
									"",
									tab_depth,
									i),
								quoted(name));
					}
				}
			} catch (...) {}
		}
	};

	// Next introspection from Buffout4 by fudgyduff under MIT
	// https://github.com/clayne/Buffout4/blob/master/src/Crash/Introspection/Introspection.cpp
	class TESObjectREFR
	{
	public:
		using value_type = RE::TESObjectREFR;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto ref = static_cast<const value_type*>(a_ptr);

			try {
				const auto objRef = ref->data.objectReference;
				if (objRef) {
					filter_results xResults;
					TESForm<RE::TESForm>::filter(xResults, objRef);

					if (!xResults.empty()) {
						a_results.emplace_back(
							fmt::format(
								"{:\t>{}}Object Reference"sv,
								"",
								tab_depth),
							"");
						for (auto& [key, value] : xResults) {
							a_results.emplace_back(
								fmt::format(
									"{:\t>{}}{}"sv,
									"",
									tab_depth,
									key),
								std::move(value));
						}
					}
				} else {
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Object Reference"sv,
							"",
							tab_depth),
						"None");
				}
			} catch (...) {}

			try {
				const auto parentCell = ref->GetParentCell();
				if (parentCell) {
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}ParentCell"sv,
							"",
							tab_depth),
						"---");
					TESForm<RE::TESObjectCELL>::filter(a_results, parentCell, tab_depth + 1);
				} else {
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}ParentCell"sv,
							"",
							tab_depth),
						"None");
				}
			} catch (...) {}
		}
	};

	class NiAVObject
	{
	public:
		using value_type = RE::NiAVObject;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);
			if (!object)
				return;
			try {
				const auto name = object ? object->name.c_str() : ""sv;
				if (!name.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Name"sv,
							"",
							tab_depth),
						quoted(name));
			} catch (...) {}

			try {
				const auto name = object->GetRTTI() ? object->GetRTTI()->GetName() : ""sv;
				if (!name.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}RTTIName"sv,
							"",
							tab_depth),
						quoted(name));
			} catch (...) {}

			try {
				for (auto i = 0; i < object->GetExtraDataSize(); i++) {
					const auto extraData = object->GetExtraDataAt(i);
					if (!extraData->GetName().empty()) {
						const auto name = extraData->GetName().c_str();
						if (name && name[0])
							a_results.emplace_back(
								fmt::format(
									"{:\t>{}}ExtraData[{}] Name"sv,
									"",
									tab_depth,
									i),
								quoted(name));
					}
				}
			} catch (...) {}

			try {
				const auto& flags = object->GetFlags();
				std::string flagString = "";
				constexpr auto flagEntries = magic_enum::enum_entries<RE::NiAVObject::Flag>();
				for (const auto& entry : flagEntries) {
					const auto flag = entry.first;
					const auto flagName = entry.second;
					if (flags.any(flag))
						flagString = flagString.empty() ?
						                 flagName :
						                 flagString.append(" | "sv).append(flagName);
				}
				a_results.emplace_back(
					fmt::format(
						"{:\t>{}}Flags"sv,
						"",
						tab_depth),
					fmt::format(
						"{}"sv,
						flagString));
			} catch (...) {}

			try {
				const auto userdata = object->GetUserData();
				if (userdata) {
					const auto name = userdata->GetDisplayFullName();
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Full Name"sv,
							"",
							tab_depth),
						quoted(name));
					const auto objectref = userdata->GetObjectReference();
					const auto filename = objectref && objectref->As<RE::TESModel>() ? objectref->As<RE::TESModel>()->GetModel() :
					                                                                   ""sv;
					if (!filename.empty())
						a_results.emplace_back(
							fmt::format(
								"{:\t>{}}File"sv,
								"",
								tab_depth),
							quoted(filename));
					if (auto owner = userdata->GetOwner()) {
						a_results.emplace_back(
							fmt::format(
								"{:\t>{}}Checking Owner"sv,
								"",
								tab_depth),
							"-----"sv);
						TESForm<RE::TESForm>::filter(a_results, owner, tab_depth + 1);
					}
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Checking User Data"sv,
							"",
							tab_depth),
						"-----"sv);
					TESObjectREFR::filter(a_results, userdata, tab_depth + 1);
				}
			} catch (...) {}
			try {
				const auto objectRefr = RE::TESObjectREFR::FindReferenceFor3D((RE::NiAVObject*)a_ptr);
				if (objectRefr) {
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Checking TESObjectREFR"sv,
							"",
							tab_depth),
						"{}"sv);
					TESObjectREFR::filter(a_results, objectRefr, tab_depth + 1);
				}
			} catch (...) {}
			// Texture introspection for BSGeometry objects
			try {
				const auto geom = netimmerse_cast<RE::BSGeometry*>(object);
				if (geom) {
					const auto effect_ptr = geom->GetGeometryRuntimeData().properties[RE::BSGeometry::States::kEffect];
					const auto effect = effect_ptr.get();
					if (effect) {
						// Check for lighting shader (most common)
						const auto lighting_shader = netimmerse_cast<RE::BSLightingShaderProperty*>(effect);
						if (lighting_shader && lighting_shader->material) {
							const auto base_material = lighting_shader->material;
							const auto material = static_cast<RE::BSLightingShaderMaterialBase*>(static_cast<RE::BSShaderMaterial*>(base_material));
							if (material) {
								const auto texture_set_ptr = material->GetTextureSet();
								const auto texture_set = texture_set_ptr.get();
								if (texture_set) {
									// Texture slot names
									static constexpr std::array<std::string_view, 8> slot_names = {
										"Diffuse", "Normal", "Glow", "Parallax",
										"Cubemap", "EnvMask", "Subsurface", "BackLighting"
									};

									for (int i = 0; i < 8; i++) {
										const char* tex_path = texture_set->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(i));
										if (tex_path && tex_path[0] != '\0') {
											// Check if texture exists in BSA or loose files
											bool exists = false;
											try {
												RE::BSResourceNiBinaryStream stream(tex_path);
												exists = stream.good();
											} catch (...) {
												exists = false;
											}

											a_results.emplace_back(
												fmt::format(
													"{:\t>{}}Texture[{}]"sv,
													"",
													tab_depth,
													slot_names[i]),
												exists ? quoted(tex_path) : fmt::format("[MISSING] {}"sv, tex_path));
										}
									}
								}
							}
						}

						// Check for effect shader
						const auto effect_shader = netimmerse_cast<RE::BSEffectShaderProperty*>(effect);
						if (effect_shader && effect_shader->material) {
							const auto material = static_cast<RE::BSEffectShaderMaterial*>(effect_shader->material);
							if (material) {
								// Source texture
								const char* source_tex = material->sourceTexturePath.c_str();
								if (source_tex && source_tex[0] != '\0') {
									bool exists = false;
									try {
										RE::BSResourceNiBinaryStream stream(source_tex);
										exists = stream.good();
									} catch (...) {
										exists = false;
									}
									a_results.emplace_back(
										fmt::format(
											"{:\t>{}}EffectTexture[Source]"sv,
											"",
											tab_depth),
										exists ? quoted(source_tex) : fmt::format("[MISSING] {}"sv, source_tex));
								}

								// Greyscale texture
								const char* grey_tex = material->greyscaleTexturePath.c_str();
								if (grey_tex && grey_tex[0] != '\0') {
									bool exists = false;
									try {
										RE::BSResourceNiBinaryStream stream(grey_tex);
										exists = stream.good();
									} catch (...) {
										exists = false;
									}
									a_results.emplace_back(
										fmt::format(
											"{:\t>{}}EffectTexture[Greyscale]"sv,
											"",
											tab_depth),
										exists ? quoted(grey_tex) : fmt::format("[MISSING] {}"sv, grey_tex));
								}
							}
						}
					}
				}
			} catch (...) {}

			try {
				const auto parent = object->parent;
				const auto parentIndex = object->parentIndex;
				if (parent) {
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Checking Parent"sv,
							"",
							tab_depth),
						fmt::format(
							"{}"sv, parentIndex));
					filter(a_results, parent, tab_depth + 1);
				}
			} catch (...) {}
		}
	};

	class NiTexture
	{
	public:
		using value_type = RE::NiTexture;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);
			if (!object)
				return;
			try {
				const auto name = object ? object->name.c_str() : ""sv;
				if (!name.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Name"sv,
							"",
							tab_depth),
						quoted(name));
			} catch (...) {}

			try {
				const auto name = object->GetRTTI() ? object->GetRTTI()->GetName() : ""sv;
				if (!name.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}RTTIName"sv,
							"",
							tab_depth),
						quoted(name));
			} catch (...) {}
		}
	};

	class NiStream
	{
	public:
		using value_type = RE::NiStream;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);
			if (!object)
				return;
			try {
				const auto& header = object->header;
				a_results.emplace_back(
					fmt::format(
						"{:\t>{}}Header"sv,
						"",
						tab_depth),
					fmt::format(
						"author: {} version: {} processScript: {} exportScript: {}",
						header.author,
						header.version,
						header.processScript,
						header.exportScript));
			} catch (...) {}

			try {
				const auto lastLoadedRTTI = object->lastLoadedRTTI;
				if (lastLoadedRTTI && lastLoadedRTTI[0])
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}lastLoadedRTTI"sv,
							"",
							tab_depth),
						quoted(lastLoadedRTTI));
			} catch (...) {}

			try {
				const auto inputFilePath = object->inputFilePath;
				if (inputFilePath && inputFilePath[0])
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}inputFilePath"sv,
							"",
							tab_depth),
						quoted(inputFilePath));
			} catch (...) {}

			try {
				const auto filePath = object->filePath;
				if (filePath && filePath[0])
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}filePath"sv,
							"",
							tab_depth),
						quoted(filePath));
			} catch (...) {}
		}
	};

	class BSShader
	{
	public:
		using value_type = RE::BSShader;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);
			if (!object)
				return;
			try {
				const auto& filename = object->fxpFilename;
				if (filename && filename[0])
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}fxpFilename"sv,
							"",
							tab_depth),
						quoted(filename));
			} catch (...) {}

			try {
				const auto type = object->shaderType;
				using recordFlags = value_type::Type;
				std::string typeString = "";
				constexpr auto typeEntries = magic_enum::enum_entries<recordFlags>();
				for (const auto& entry : typeEntries) {
					const auto _type = entry.first;
					const auto typeName = entry.second;
					if (_type == type)
						typeString = typeName;
					break;
				}

				a_results.emplace_back(
					fmt::format(fmt::runtime(
									"{:\t>{}}ShaderType"),
						"",
						tab_depth),
					fmt::format(fmt::runtime(
									"0x{:08X}"),
						typeString));
			} catch (...) {}
		}
	};

	class BSShaderMaterial
	{
	public:
		using value_type = RE::BSShaderMaterial;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);
			if (!object)
				return;
			try {
				const auto& feature = object->GetFeature();
				const auto featureName = magic_enum::enum_name(feature);
				if (!featureName.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Feature"sv,
							"",
							tab_depth),
						featureName);
			} catch (...) {}

			try {
				const auto type = object->GetType();
				const auto typeName = magic_enum::enum_name(type);
				if (!typeName.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Type"sv,
							"",
							tab_depth),
						quoted(typeName));
			} catch (...) {}
		}
	};

	class hkaAnimationBinding
	{
	public:
		using value_type = RE::hkaAnimationBinding;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);
			if (!object)
				return;
			try {
				const auto& name = object->originalSkeletonName;
				if (!name.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Skeleton Name"sv,
							"",
							tab_depth),
						quoted(name.data()));
			} catch (...) {}
		};
	};

	class hkbCharacter
	{
	public:
		using value_type = RE::hkbCharacter;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);
			if (!object)
				return;
			try {
				const auto& name = object->name;
				if (!name.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Name"sv,
							"",
							tab_depth),
						quoted(name.c_str()));
			} catch (...) {}
		};
	};

	class hkbClipGenerator
	{
	public:
		using value_type = RE::hkbClipGenerator;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);
			if (!object)
				return;
			try {
				const auto& name = object->animationName;
				if (!name.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Animation Name"sv,
							"",
							tab_depth),
						quoted(name.data()));
			} catch (...) {}
			try {
				const auto flags = object->mode;
				using recordFlags = value_type::PlaybackMode;
				std::string flagString = "";
				constexpr auto flagEntries = magic_enum::enum_entries<recordFlags>();
				for (const auto& entry : flagEntries) {
					const auto flag = entry.first;
					const auto flagName = entry.second;
					if (flag == flags)
						flagString = flagName;
					break;
				}
				a_results.emplace_back(
					fmt::format(
						"{:\t>{}}Playback Mode"sv,
						"",
						tab_depth),
					fmt::format(
						"{} {}"sv,
						flags.underlying(),
						flagString));
			} catch (...) {}
			try {
				const auto& binding = object->binding;
				if (binding) {
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Checking Binding"sv,
							"",
							tab_depth),
						"-----"sv);
					hkaAnimationBinding::filter(a_results, binding, tab_depth + 1);
				}
			} catch (...) {}
		};
	};

	class hkpConstraintInstance
	{
	public:
		using value_type = RE::hkpConstraintInstance;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);
			if (!object)
				return;
			try {
				const auto& entities = object->entities;
				if (entities)
					for (int i = 0; i < 2; i++) {
						auto entity = entities[i];
						if (entity) {
							a_results.emplace_back(
								fmt::format(
									"{:\t>{}}Entity [{}]"sv,
									"",
									tab_depth,
									i),
								"-----"sv);
							if (!entity->name.empty())
								a_results.emplace_back(
									fmt::format(
										"{:\t>{}}Name"sv,
										"",
										tab_depth),
									quoted(entity->name.data()));
							if (entity->GetUserData()) {
								a_results.emplace_back(
									fmt::format(
										"{:\t>{}}Checking User Data"sv,
										"",
										tab_depth),
									"-----"sv);
								TESObjectREFR::filter(a_results, entity->GetUserData(), tab_depth + 1);
							}
						}
					}
			} catch (...) {}
		};
	};

	class hkbNode
	{
	public:
		using value_type = RE::hkbNode;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);
			if (!object)
				return;
			try {
				const auto& name = object->name;
				if (!name.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Name"sv,
							"",
							tab_depth),
						quoted(name.data()));
			} catch (...) {}
			try {
				const auto& id = object->id;
				a_results.emplace_back(
					fmt::format(
						"{:\t>{}}ID"sv,
						"",
						tab_depth),
					fmt::format(
						"0x{:08X}"sv,
						id));
			} catch (...) {}
		};
	};

	class hkpWorldObject
	{
	public:
		using value_type = RE::hkpWorldObject;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);
			if (!object)
				return;
			try {
				const auto& name = object->name;
				if (!name.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Name"sv,
							"",
							tab_depth),
						quoted(name.data()));
			} catch (...) {}
			try {
				const auto userdata = object->GetUserData();
				if (userdata) {
					const auto name = userdata->GetDisplayFullName();
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Full Name"sv,
							"",
							tab_depth),
						quoted(name));
					const auto objectref = userdata->GetObjectReference();
					const auto filename = objectref && objectref->As<RE::TESModel>() ? objectref->As<RE::TESModel>()->GetModel() :
					                                                                   ""sv;
					if (!filename.empty())
						a_results.emplace_back(
							fmt::format(
								"{:\t>{}}File"sv,
								"",
								tab_depth),
							quoted(filename));
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Checking User Data"sv,
							"",
							tab_depth),
						"-----"sv);
					TESObjectREFR::filter(a_results, userdata, tab_depth + 1);
					if (auto owner = userdata->GetOwner()) {
						a_results.emplace_back(
							fmt::format(
								"{:\t>{}}Checking Owner"sv,
								"",
								tab_depth),
							"-----"sv);
						TESForm<RE::TESForm>::filter(a_results, owner, tab_depth + 1);
					}
				}
			} catch (...) {}
		};
	};

	class BShkbAnimationGraph
	{
	public:
		using value_type = RE::BShkbAnimationGraph;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);
			if (!object)
				return;
			try {
				const auto& name = object->projectName;
				if (!name.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Project Name"sv,
							"",
							tab_depth),
						quoted(name.data()));
			} catch (...) {}
			try {
				auto characterInstance = &(object->characterInstance);
				if (characterInstance)
					hkbCharacter::filter(a_results, characterInstance, tab_depth + 1);
			} catch (...) {}
			try {
				auto& holder = object->holder;
				if (holder) {
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Holder"sv,
							""sv,
							tab_depth),
						""sv);
					TESObjectREFR::filter(a_results, holder, tab_depth + 1);
				}
			} catch (...) {}
		};
	};

	// Next set of introspection from Buffout4 by fudgyduff under MIT
	// https://github.com/clayne/Buffout4/blob/master/src/Crash/Introspection/Introspection.cpp
	namespace BSResource
	{
		class LooseFileStreamBase
		{
		public:
			using value_type = RE::BSResource::LooseFileStreamBase;

			static void filter(
				filter_results& a_results,
				const void* a_ptr, int tab_depth = 0) noexcept
			{
				const auto stream = static_cast<const value_type*>(a_ptr);

				try {
					const auto dirName = stream->dirName.c_str();
					if (dirName && dirName[0])
						a_results.emplace_back(
							fmt::format(
								"{:\t>{}}Directory Name"sv,
								"",
								tab_depth),
							quoted(dirName));
				} catch (...) {}

				try {
					const auto fileName = stream->fileName.c_str();
					if (fileName && fileName[0])
						a_results.emplace_back(
							fmt::format(
								"{:\t>{}}File Name"sv,
								"",
								tab_depth),
							quoted(fileName));
				} catch (...) {}

				try {
					const auto prefix = stream->prefix.c_str();
					if (prefix && prefix[0])
						a_results.emplace_back(
							fmt::format(
								"{:\t>{}}Prefix"sv,
								"",
								tab_depth),
							quoted(prefix));
				} catch (...) {}
			}
		};
	};

	namespace BSScript
	{
		namespace NF_util
		{
			class NativeFunctionBase
			{
			public:
				using value_type = RE::BSScript::NF_util::NativeFunctionBase;

				static void filter(
					filter_results& a_results,
					const void* a_ptr, int tab_depth = 0) noexcept
				{
					const auto function = static_cast<const value_type*>(a_ptr);

					try {
						const std::string_view name = function->GetName();
						const std::string_view objName = function->GetObjectTypeName();
						if (!name.empty() && !objName.empty())
							a_results.emplace_back(
								fmt::format(
									"{:\t>{}}Function"sv,
									"",
									tab_depth),
								fmt::format("\"{}.{}\""sv, objName, name));
						else if (!name.empty())
							a_results.emplace_back(
								fmt::format(
									"{:\t>{}}Function"sv,
									"",
									tab_depth),
								quoted(name));
					} catch (...) {}

					try {
						const std::string_view stateName = function->GetStateName();
						if (!stateName.empty())
							a_results.emplace_back(
								fmt::format(
									"{:\t>{}}State"sv,
									"",
									tab_depth),
								quoted(stateName));

					} catch (...) {}
				}
			};
		};

		class ObjectTypeInfo
		{
		public:
			using value_type = RE::BSScript::ObjectTypeInfo;

			static void filter(
				filter_results& a_results,
				const void* a_ptr, int tab_depth = 0) noexcept
			{
				const auto info = static_cast<const value_type*>(a_ptr);

				try {
					const std::string_view name = info->name;
					if (!name.empty())
						a_results.emplace_back(
							fmt::format(
								"{:\t>{}}Name"sv,
								"",
								tab_depth),
							quoted(name));
				} catch (...) {}

				try {
					const std::string_view docString = info->docString;
					if (!docString.empty())
						a_results.emplace_back(
							fmt::format(
								"{:\t>{}}DocString"sv,
								"",
								tab_depth),
							quoted(docString));
				} catch (...) {}
			}
		};

		class SimpleAllocMemoryPagePolicy
		{
		public:
			using value_type = RE::BSScript::SimpleAllocMemoryPagePolicy;

			static void filter(
				filter_results& a_results,
				const void* a_ptr, int tab_depth = 0) noexcept
			{
				const auto object = static_cast<const value_type*>(a_ptr);

				if (!object)
					return;

				try {
					const auto minPageSize = object->minPageSize;
					const auto maxPageSize = object->maxPageSize;
					a_results.emplace_back(
						fmt::format("{:\t>{}}Page Sizes", "", tab_depth),
						fmt::format("{} - {} bytes", minPageSize, maxPageSize));
				} catch (...) {}

				try {
					const auto maxAllocatedMemory = object->maxAllocatedMemory;
					const auto ignoreMemoryLimit = object->ignoreMemoryLimit;
					a_results.emplace_back(
						fmt::format("{:\t>{}}Memory Limit", "", tab_depth),
						fmt::format("{} bytes (Ignored: {})", maxAllocatedMemory, ignoreMemoryLimit));
				} catch (...) {}

				try {
					const auto currentMemorySize = object->currentMemorySize;
					const auto maxAllocatedMemory = object->maxAllocatedMemory;
					const auto percentage = maxAllocatedMemory > 0 ? (static_cast<double>(currentMemorySize) / maxAllocatedMemory) * 100.0 : 0.0;
					a_results.emplace_back(
						fmt::format("{:\t>{}}Current Usage", "", tab_depth),
						fmt::format("{} / {} bytes ({:.2f}%)", currentMemorySize, maxAllocatedMemory, percentage));
				} catch (...) {}

				try {
					const auto maxAdditionalAllocations = object->maxAdditionalAllocations;
					if (maxAdditionalAllocations > 0) {
						a_results.emplace_back(
							fmt::format("{:\t>{}}Max Additional Allocations", "", tab_depth),
							fmt::format("{}", maxAdditionalAllocations));
					}
				} catch (...) {}
			}
		};

		namespace Internal
		{
			class VirtualMachine
			{
			public:
				using value_type = RE::BSScript::Internal::VirtualMachine;

				static void filter(
					filter_results& a_results,
					const void* a_ptr, int tab_depth = 0) noexcept
				{
					const auto object = static_cast<const value_type*>(a_ptr);

					if (!object)
						return;

					try {
						a_results.emplace_back(
							fmt::format("{:\t>{}}Overstressed", "", tab_depth),
							fmt::format("{}", object->overstressed));
					} catch (...) {}

					try {
						a_results.emplace_back(
							fmt::format("{:\t>{}}Initialized", "", tab_depth),
							fmt::format("{}", object->initialized));
					} catch (...) {}

					try {
						const auto freezeState = object->freezeState;
						a_results.emplace_back(
							fmt::format("{:\t>{}}Freeze State", "", tab_depth),
							fmt::format("{}", magic_enum::enum_name(freezeState.get())));
					} catch (...) {}

					try {
						a_results.emplace_back(
							fmt::format("{:\t>{}}Frozen Stacks Count", "", tab_depth),
							fmt::format("{}", object->frozenStacksCount));
					} catch (...) {}

					try {
						a_results.emplace_back(
							fmt::format("{:\t>{}}Waiting Function Messages", "", tab_depth),
							fmt::format("{}", object->uiWaitingFunctionMessages));
					} catch (...) {}

					try {
						a_results.emplace_back(
							fmt::format("{:\t>{}}Object Table Size", "", tab_depth),
							fmt::format("{}", object->objectTable.size()));
					} catch (...) {}

					try {
						a_results.emplace_back(
							fmt::format("{:\t>{}}Array Table Size", "", tab_depth),
							fmt::format("{}", object->arrays.size()));
					} catch (...) {}

					try {
						a_results.emplace_back(
							fmt::format("{:\t>{}}Running Stacks Count", "", tab_depth),
							fmt::format("{}", object->allRunningStacks.size()));
					} catch (...) {}
				}
			};
		}
	};

	class NiObjectNET
	{
	public:
		using value_type = RE::NiObjectNET;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);

			try {
				const auto name = object->name.c_str();
				if (name && name[0])
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Name"sv,
							"",
							tab_depth),
						quoted(name));
			} catch (...) {}
		};
	};
	// End set of introspection from Buffout4 by fudgyduff under MIT

	class TESRegionDataSound
	{
	public:
		using value_type = RE::TESRegionDataSound;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);

			try {
				const auto name = object->music ? object->music->GetName() : "";
				if (name && name[0])
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Music Name"sv,
							"",
							tab_depth),
						quoted(name));
			} catch (...) {}
			try {
				const auto& sounds = object->sounds;
				for (const auto& soundItem : sounds) {
					if (soundItem) {
						a_results.emplace_back(
							fmt::format(
								"{:\t>{}}Sound Chance"sv,
								"",
								tab_depth),
							fmt::format(
								"{:f}"sv,
								soundItem->chance));
						if (soundItem->sound) {
							TESForm<RE::BGSSoundDescriptorForm>::filter(a_results, soundItem->sound, tab_depth + 1);
						}
						const auto flags = soundItem->flags;
						using recordFlags = value_type::Sound::Flag;
						std::string flagString = "";
						constexpr auto flagEntries = magic_enum::enum_entries<recordFlags>();
						for (const auto& entry : flagEntries) {
							const auto flag = entry.first;
							const auto flagName = entry.second;
							if (flag & flags)
								flagString = flagString.empty() ?
								                 flagName :
								                 flagString.append(" | "sv).append(flagName);
						}
						a_results.emplace_back(
							fmt::format(
								"{:\t>{}}Flags"sv,
								"",
								tab_depth),
							fmt::format(
								"0x{:08X} {}"sv,
								flags.underlying(),
								flagString));
					}
				}
			} catch (...) {}
		};
	};

	class TESQuest
	{
	public:
		using value_type = RE::TESQuest;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);
			if (!object)
				return;
			try {
				a_results.emplace_back(
					fmt::format(
						"{:\t>{}}Active Quest"sv,
						"",
						tab_depth),
					fmt::format(
						"{}",
						object->IsActive()));
				a_results.emplace_back(
					fmt::format(
						"{:\t>{}}Current Stage"sv,
						"",
						tab_depth),
					fmt::format(
						"{}",
						object->GetCurrentStageID()));
				a_results.emplace_back(
					fmt::format(
						"{:\t>{}}Type"sv,
						"",
						tab_depth),
					magic_enum::enum_name(object->GetType()));
			} catch (...) {}
		};
	};

	class ExtraTextDisplayData
	{
	public:
		using value_type = RE::ExtraTextDisplayData;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);

			try {
				const auto name = object->displayName.c_str();
				if (name && name[0])
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Display Name"sv,
							"",
							tab_depth),
						quoted(name));
			} catch (...) {}
			try {
				const auto& displayNameText = object->displayNameText;
				if (displayNameText)
					TESForm<RE::BGSMessage>::filter(a_results, displayNameText, tab_depth + 1);
			} catch (...) {}
			try {
				const auto quest = object->ownerQuest;
				if (quest) {
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Owner Quest"sv,
							""sv,
							tab_depth),
						""sv);
					TESQuest::filter(a_results, quest, tab_depth + 1);
				}
			} catch (...) {}
		};
	};

	// Code from Nightfallstorm
	class CodeTasklet
	{
	public:
		using value_type = RE::BSScript::Internal::CodeTasklet;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);
			const auto& handlePolicy = RE::SkyrimVM::GetSingleton()->handlePolicy;
			const auto datahandler = RE::TESDataHandler::GetSingleton();
			try {
				auto currentStackFrame = object->stack->top;  // get stack from BSScript::Internal::CodeTasklet (or get stack directly if it's a stack object
				std::string stackTrace = "\n";
				std::map<std::string, bool> objectReferences;
				while (currentStackFrame) {
					auto& function = currentStackFrame->owningFunction;
					auto& functionObjecTypeName = function.get()->GetObjectTypeName();
					auto& functionName = function.get()->GetName();
					auto objectInstanceString = RE::BSFixedString("None");
					auto& objectRef = currentStackFrame->self;
					if (objectRef.IsObject()) {
						auto objectHandle = objectRef.GetObject().get()->GetHandle();
						handlePolicy.ConvertHandleToString(objectHandle, objectInstanceString);
						const auto handleString = std::string{ objectInstanceString };
						const auto paranStart = handleString.find("(");
						const auto paranEnd = handleString.rfind(")");
						const auto formIDString = (paranStart != std::string::npos && paranEnd != std::string::npos && paranStart <= paranEnd) ? handleString.substr(paranStart + 1, paranEnd - 1) : "";
						if (!formIDString.empty())
							objectReferences.emplace(formIDString, true);
					}
					auto& sourceFileName = function->GetSourceFilename();
					auto traceFormatString = "{:\t>{}}[{}].{}.{}() - \"{}\" Line {}\n";  // Same format in Papyrus logs
					std::string lineTrace = "";
					if (function.get()->GetIsNative()) {
						lineTrace = fmt::format(fmt::runtime(traceFormatString), "", tab_depth, objectInstanceString, functionObjecTypeName, functionName, sourceFileName, "?"sv);
					} else {
						std::uint32_t lineNumber;
						function.get()->TranslateIPToLineNumber(currentStackFrame->instructionPointer, lineNumber);
						lineTrace = fmt::format(fmt::runtime(traceFormatString), "", tab_depth, objectInstanceString, functionObjecTypeName, functionName, sourceFileName, std::to_string(lineNumber));
					}
					stackTrace = stackTrace + lineTrace;
					currentStackFrame = currentStackFrame->previousFrame;
				}
				a_results.emplace_back(
					fmt::format(
						"{:\t>{}}Stack Trace"sv,
						"",
						tab_depth),
					stackTrace);
				for (auto& objectReference : objectReferences) {
					const auto& objectString = objectReference.first;
					const auto modIndex = std::stoi(objectString.substr(0, 2), nullptr, 16);
					const auto form = std::stoi(objectString.substr(3, objectString.size()), nullptr, 16);
					const auto target = datahandler->LookupForm(form, datahandler->LookupLoadedModByIndex(modIndex)->GetFilename());
					if (target)
						TESForm<RE::TESForm>::filter(a_results, target, tab_depth + 1);
				}
			} catch (...) {}
		};
	};
	class ExtraLeveledItem
	{
	public:
		using value_type = RE::ExtraLeveledItem;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);

			try {
				const auto formID = object->levItem;
				a_results.emplace_back(
					fmt::format(
						"{:\t>{}}FormID"sv,
						"",
						tab_depth),
					fmt::format(
						"0x{:08X}"sv,
						formID));
				const auto target = RE::TESForm::LookupByID(formID);
				TESForm<RE::TESForm>::filter(a_results, target, tab_depth + 1);
			} catch (...) {}
		};
	};

	class ScriptEffect
	{
	public:
		using value_type = RE::ScriptEffect;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);

			if (!object)
				return;
			try {
				const auto script = object->script;
				if (script && script->text && script->text[0])
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Script Text"sv,
							"",
							tab_depth),
						quoted(script->text));
				if (script && script->parentQuest) {
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Parent Quest"sv,
							"",
							tab_depth),
						""sv);
					TESQuest::filter(a_results, script->parentQuest, tab_depth + 1);
				}
			} catch (...) {}
			try {
				const auto scriptLocals = object->effectLocals;
				if (scriptLocals && scriptLocals->masterScript) {
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Master Script"sv,
							""sv,
							tab_depth),
						""sv);
					ScriptEffect::filter(a_results, scriptLocals->masterScript, tab_depth + 1);
				}
			} catch (...) {}
		}
	};

	class BSCullingProcess
	{
	public:
		using value_type = RE::BSCullingProcess;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);
			if (!object)
				return;

			try {
				const auto cullMode = object->cullMode.get();
				const auto cullModeName = magic_enum::enum_name(cullMode);
				a_results.emplace_back(
					fmt::format("{:\t>{}}Cull Mode"sv, "", tab_depth),
					fmt::format("{} ({})"sv, cullModeName, std::to_underlying(cullMode)));
			} catch (...) {}

			try {
				const auto objectCount = object->objectArray.size();
				a_results.emplace_back(
					fmt::format("{:\t>{}}Object Array Size"sv, "", tab_depth),
					fmt::format("{}"sv, objectCount));
			} catch (...) {}

			try {
				const auto alphaGroupCount = object->alphaGroups.size();
				a_results.emplace_back(
					fmt::format("{:\t>{}}Alpha Groups Size"sv, "", tab_depth),
					fmt::format("{}"sv, alphaGroupCount));
			} catch (...) {}

			try {
				a_results.emplace_back(
					fmt::format("{:\t>{}}Recurse to Geometry"sv, "", tab_depth),
					fmt::format("{}"sv, object->recurseToGeometry));
			} catch (...) {}

			try {
				a_results.emplace_back(
					fmt::format("{:\t>{}}Is Grouping Alphas"sv, "", tab_depth),
					fmt::format("{}"sv, object->isGroupingAlphas));
			} catch (...) {}

			try {
				const auto cullModeStackIndex = object->cullModeStackIndex;
				a_results.emplace_back(
					fmt::format("{:\t>{}}Cull Mode Stack Index"sv, "", tab_depth),
					fmt::format("{}"sv, cullModeStackIndex));
			} catch (...) {}
		}
	};

	class ServingThread
	{
	public:
		using value_type = RE::JobListManager::ServingThread;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);
			if (!object)
				return;

			try {
				const auto threadID = object->threadID;
				a_results.emplace_back(
					fmt::format("{:\t>{}}Thread ID"sv, "", tab_depth),
					fmt::format("0x{:08X}"sv, threadID));
			} catch (...) {}

			try {
				const auto ownerThreadID = object->ownerThreadID;
				a_results.emplace_back(
					fmt::format("{:\t>{}}Owner Thread ID"sv, "", tab_depth),
					fmt::format("0x{:08X}"sv, ownerThreadID));
			} catch (...) {}

			try {
				const auto initialized = object->initialized;
				a_results.emplace_back(
					fmt::format("{:\t>{}}Initialized"sv, "", tab_depth),
					fmt::format("{}"sv, initialized));
			} catch (...) {}

			try {
				const auto bRunning = object->bRunning;
				const auto bProcessing = object->bProcessing;
				const auto bShutDown = object->bShutDown;
				a_results.emplace_back(
					fmt::format("{:\t>{}}Status"sv, "", tab_depth),
					fmt::format("Running={}, Processing={}, ShutDown={}", bRunning, bProcessing, bShutDown));
			} catch (...) {}

			try {
				const auto states = object->states;
				std::string stateStr;
				for (int i = 0; i < 2; ++i) {
					const auto state = states[i];
					const auto stateName = magic_enum::enum_name(state);
					const auto stateValue = std::to_underlying(state);
					if (!stateStr.empty())
						stateStr += ", ";
					stateStr += fmt::format("State[{}]: {} ({})", i, stateName.empty() ? "Unknown" : std::string(stateName), stateValue);
				}
				a_results.emplace_back(
					fmt::format("{:\t>{}}States"sv, "", tab_depth),
					stateStr);
			} catch (...) {}

			try {
				// Note: BSEventFlag doesn't have a public interface to check state
				a_results.emplace_back(
					fmt::format("{:\t>{}}Events"sv, "", tab_depth),
					"NewWork, WorkDone");
			} catch (...) {}
		}
	};
}

namespace Crash::Introspection
{
	[[nodiscard]] const Modules::Module* get_module_for_pointer(
		const void* a_ptr,
		std::span<const module_pointer> a_modules) noexcept
	{
		const auto it = std::lower_bound(
			a_modules.rbegin(),
			a_modules.rend(),
			reinterpret_cast<std::uintptr_t>(a_ptr),
			[](auto&& a_lhs, auto&& a_rhs) noexcept {
				return a_lhs->address() >= a_rhs;
			});
		return it != a_modules.rend() && (*it)->in_range(a_ptr) ? it->get() : nullptr;
	}

	namespace detail
	{
		static std::unordered_map<const void*, std::string> seen_objects;
		static std::function<std::string(size_t)> label_generator;
		static std::size_t total_backfill_count = 0;
		static bool backfill_logged_this_crash = false;
		class Integer
		{
		public:
			Integer(std::size_t a_value) noexcept :
				_value(a_value),
				name_string(a_value >> 63 ?
								fmt::format(fmt::runtime("(size_t) [uint: {} int: {}]"s), _value, static_cast<std::make_signed_t<size_t>>(_value)) :
								fmt::format(fmt::runtime("(size_t) [{}]"s), _value))
			{
			}

			[[nodiscard]] std::string name() const { return name_string; }

		private:
			const std::size_t _value;
			const std::string name_string;
		};

		class Pointer
		{
		public:
			Pointer() noexcept = default;

			Pointer(const void* a_ptr, std::span<const module_pointer> a_modules) noexcept :
				_module(get_module_for_pointer(a_ptr, a_modules))
			{
				if (_module) {
					_ptr = a_ptr;
				}
			}

			[[nodiscard]] std::string name() const
			{
				// Check if this address was already introspected as a known object
				auto it = seen_objects.find(_ptr);
				if (it != seen_objects.end()) {
					return it->second;  // Return the full object information
				}

				if (_module) {
					const auto address = reinterpret_cast<std::uintptr_t>(_ptr);
					const auto pdbDetails = Crash::PDB::pdb_details(_module->path(), address - _module->address());
					const auto assembly = _module->assembly((const void*)address);
					if (!pdbDetails.empty())
						return fmt::format(
							"(void* -> {}+{:07X}\t{} | {})"sv,
							_module->name(),
							address - _module->address(),
							assembly,
							pdbDetails);
					return fmt::format(
						"(void* -> {}+{:07X}\t{})"sv,
						_module->name(),
						address - _module->address(),
						assembly);
				} else {
					return "(void*)"s;
				}
			}

		private:
			const Modules::Module* _module{ nullptr };
			const void* _ptr{ nullptr };
		};

		class Polymorphic
		{
		public:
			explicit Polymorphic(std::string_view a_mangled) noexcept :
				_mangled{ a_mangled }
			{
				// NOLINTNEXTLINE(readability-simplify-subscript-expr)
				assert(_mangled.size() > 1 && _mangled.data()[_mangled.size()] == '\0');
			}

			[[nodiscard]] std::string name() const
			{
				const std::string demangled = Crash::PDB::demangle(std::string{ _mangled });
				return fmt::format("({}*)"sv, demangled);
			}

		private:
			std::string_view _mangled;
		};

		class F4Polymorphic
		{
		public:
			F4Polymorphic(
				std::string_view a_mangled,
				const RE::RTTI::CompleteObjectLocator* a_col,
				const void* a_ptr) noexcept :
				_poly{ a_mangled },
				_col{ a_col },
				_ptr{ a_ptr }
			{
				assert(_col != nullptr);
				assert(_ptr != nullptr);
			}

			[[nodiscard]] std::string name() const
			{
				auto it = seen_objects.find(_ptr);
				if (it != seen_objects.end()) {
					return fmt::format("{} See 0x{:X}", _poly.name(), reinterpret_cast<std::uintptr_t>(_ptr));
				}

				auto result = _poly.name();
				SSE::filter_results xInfo;

				const auto moduleBase = REL::Module::get().base();
				const auto hierarchy = _col->classDescriptor.get();
				const std::span bases(
					reinterpret_cast<std::uint32_t*>(hierarchy->baseClassArray.offset() + moduleBase),
					hierarchy->numBaseClasses);
				for (const auto rva : bases) {
					const auto base = reinterpret_cast<RE::RTTI::BaseClassDescriptor*>(rva + moduleBase);
					const auto it = FILTERS.find(base->typeDescriptor->mangled_name());
					if (it != FILTERS.end()) {
						const auto root = util::adjust_pointer<void>(_ptr, -static_cast<std::ptrdiff_t>(_col->offset));
						const auto target = util::adjust_pointer<void>(root, static_cast<std::ptrdiff_t>(base->pmd.mDisp));
						it->second(xInfo, target, 0);
					} else {
						// Demangle the type name for better readability using the improved PDB demangler
						const char* mangled_name = base->typeDescriptor->mangled_name();
						if (mangled_name && mangled_name[0] != '\0') {
							std::string demangled_info = Crash::PDB::demangle(std::string(mangled_name));
							logger::info("Found unhandled type:\t{}\t{} [{}]"sv, result, mangled_name, demangled_info);
						} else {
							logger::info("Found unhandled type:\t{}\t<null>"sv, result);
						}
					}
				}

				if (!xInfo.empty()) {
					for (const auto& [key, value] : xInfo) {
						result += fmt::format(
							"\n\t\t{}: {}"sv,
							key,
							value);
					}
				}

				// Store the complete result for backfilling void* references
				seen_objects[_ptr] = result;

				return result;
			}

		private:
			static constexpr auto FILTERS = frozen::make_map({
				std::make_pair(".?AULooseFileStreamBase@?A0x5f338b68@BSResource@@"sv, SSE::BSResource::LooseFileStreamBase::filter),
				std::make_pair(".?AVActorKnowledge@@"sv, SSE::ActorKnowledge::filter),
				std::make_pair(".?AVBSCullingProcess@@"sv, SSE::BSCullingProcess::filter),
				std::make_pair(".?AVBShkbAnimationGraph@@"sv, SSE::BShkbAnimationGraph::filter),
				std::make_pair(".?AVBSShader@@"sv, SSE::BSShader::filter),
				std::make_pair(".?AVBSShaderMaterial@@"sv, SSE::BSShaderMaterial::filter),
				std::make_pair(".?AVBSShaderProperty@@"sv, SSE::BSShaderProperty::filter),
				std::make_pair(".?AVCharacter@@"sv, SSE::TESForm<RE::Character>::filter),
				std::make_pair(".?AVCodeTasklet@Internal@BSScript@@"sv, SSE::CodeTasklet::filter),
				std::make_pair(".?AVExtraLeveledItem@@"sv, SSE::ExtraLeveledItem::filter),
				std::make_pair(".?AVExtraTextDisplayData@@"sv, SSE::ExtraTextDisplayData::filter),
				std::make_pair(".?AVhkaAnimationBinding@@"sv, SSE::hkaAnimationBinding::filter),
				std::make_pair(".?AVhkbCharacter@@"sv, SSE::hkbCharacter::filter),
				std::make_pair(".?AVhkbClipGenerator@@"sv, SSE::hkbClipGenerator::filter),
				std::make_pair(".?AVhkbNode@@"sv, SSE::hkbNode::filter),
				std::make_pair(".?AVhkpConstraintInstance@@"sv, SSE::hkpConstraintInstance::filter),
				std::make_pair(".?AVhkpWorldObject@@"sv, SSE::hkpWorldObject::filter),
				std::make_pair(".?AVNativeFunctionBase@NF_util@BSScript@@"sv, SSE::BSScript::NF_util::NativeFunctionBase::filter),
				std::make_pair(".?AVNiAVObject@@"sv, SSE::NiAVObject::filter),
				std::make_pair(".?AVNiObjectNET@@"sv, SSE::NiObjectNET::filter),
				std::make_pair(".?AVNiStream@@"sv, SSE::NiStream::filter),
				std::make_pair(".?AVNiTexture@@"sv, SSE::NiTexture::filter),
				std::make_pair(".?AVObjectTypeInfo@BSScript@@"sv, SSE::BSScript::ObjectTypeInfo::filter),
				std::make_pair(".?AVPlayerCharacter@@"sv, SSE::TESForm<RE::PlayerCharacter>::filter),
				std::make_pair(".?AVScriptEffect@@"sv, SSE::ScriptEffect::filter),
				std::make_pair(".?AVServingThread@JobListManager@@"sv, SSE::ServingThread::filter),
				std::make_pair(".?AVSimpleAllocMemoryPagePolicy@BSScript@@"sv, SSE::BSScript::SimpleAllocMemoryPagePolicy::filter),
				std::make_pair(".?AVVirtualMachine@Internal@BSScript@@"sv, SSE::BSScript::Internal::VirtualMachine::filter),
				std::make_pair(".?AVTESFaction@@"sv, SSE::TESForm<RE::TESFaction>::filter),
				std::make_pair(".?AVTESForm@@"sv, SSE::TESForm<RE::TESForm>::filter),
				std::make_pair(".?AVTESFullName@@"sv, SSE::TESFullName::filter),
				std::make_pair(".?AVTESNPC@@"sv, SSE::TESForm<RE::TESNPC>::filter),
				std::make_pair(".?AVTESObjectCELL@@"sv, SSE::TESForm<RE::TESObjectCELL>::filter),
				std::make_pair(".?AVTESObjectREFR@@"sv, SSE::TESObjectREFR::filter),
				std::make_pair(".?AVTESQuest@@"sv, SSE::TESQuest::filter),
				std::make_pair(".?AVTESRegionDataSound@@"sv, SSE::TESRegionDataSound::filter),
			});

			Polymorphic _poly;
			const RE::RTTI::CompleteObjectLocator* _col{ nullptr };
			const void* _ptr{ nullptr };
		};

		class String
		{
		public:
			String(std::string_view a_str) noexcept :
				_str(a_str)
			{}

			[[nodiscard]] std::string name() const
			{
				return fmt::format("(char*) \"{}\""sv, _str);
			}

		private:
			std::string_view _str;
		};

		using analysis_result = std::variant<
			Integer,
			Pointer,
			Polymorphic,
			F4Polymorphic,
			String>;

		template <class T, class... Args>
		[[nodiscard]] analysis_result make_result(Args&&... a_args) noexcept(
			std::is_nothrow_constructible_v<T, Args...>)
		{
			return analysis_result(std::in_place_type_t<T>{}, std::forward<Args>(a_args)...);
		}

		[[nodiscard]] auto analyze_polymorphic(
			void* a_ptr,
			std::span<const module_pointer> a_modules) noexcept
			-> std::optional<analysis_result>
		{
			try {
				const auto vtable = *reinterpret_cast<void**>(a_ptr);
				const auto mod = get_module_for_pointer(vtable, a_modules);
				if (!mod || !mod->in_rdata_range(vtable)) {
					return std::nullopt;
				}

				const auto col =
					*reinterpret_cast<RE::RTTI::CompleteObjectLocator**>(
						reinterpret_cast<std::size_t*>(vtable) - 1);
				if (mod != get_module_for_pointer(col, a_modules) || !mod->in_rdata_range(col)) {
					return std::nullopt;
				}

				const auto typeDesc =
					reinterpret_cast<RE::RTTI::TypeDescriptor*>(
						mod->address() + col->typeDescriptor.offset());
				if (mod != get_module_for_pointer(typeDesc, a_modules) || !mod->in_data_range(typeDesc)) {
					return std::nullopt;
				}

				if (*reinterpret_cast<const void**>(typeDesc) != mod->type_info()) {
					return std::nullopt;
				}

				if (_stricmp(mod->name().data(), util::module_name().c_str()) == 0) {
					return make_result<F4Polymorphic>(typeDesc->mangled_name(), col, a_ptr);
				} else {
					return make_result<Polymorphic>(typeDesc->mangled_name());
				}
			} catch (...) {
				return std::nullopt;
			}
		}

		[[nodiscard]] auto analyze_string(void* a_ptr) noexcept
			-> std::optional<analysis_result>
		{
			try {
				const auto printable = [](char a_ch) noexcept {
					if (' ' <= a_ch && a_ch <= '~') {
						return true;
					} else {
						switch (a_ch) {
						case '\t':
						case '\n':
							return true;
						default:
							return false;
						}
					}
				};

				const auto str = static_cast<const char*>(a_ptr);
				constexpr std::size_t max = 1000;
				std::size_t len = 0;
				for (; len < max && str[len] != '\0'; ++len) {
					if (!printable(str[len])) {
						return std::nullopt;
					}
				}

				if (len == 0 || len >= max) {
					return std::nullopt;
				}

				return make_result<String>(std::string_view{ str, len });
			} catch (...) {
				return std::nullopt;
			}
		}

		[[nodiscard]] auto analyze_pointer(
			void* a_ptr,
			std::span<const module_pointer> a_modules) noexcept
			-> analysis_result
		{
			if (auto poly = analyze_polymorphic(a_ptr, a_modules); poly) {
				return *std::move(poly);
			}

			if (auto str = analyze_string(a_ptr); str) {
				return *std::move(str);
			}

			return make_result<Pointer>(a_ptr, a_modules);
		}

		[[nodiscard]] auto analyze_integer(
			std::size_t a_value,
			std::span<const module_pointer> a_modules) noexcept
			-> analysis_result
		{
			try {
				if (a_value != 0) {
					*reinterpret_cast<const volatile std::byte*>(a_value);
					return analyze_pointer(reinterpret_cast<void*>(a_value), a_modules);
				}
			} catch (...) {}

			return make_result<Integer>(a_value);
		}
	}

	std::vector<std::string> analyze_data(
		std::span<const std::size_t> a_data,
		std::span<const module_pointer> a_modules,
		std::function<std::string(size_t)> a_label_generator)
	{
		detail::seen_objects.clear();
		detail::label_generator = a_label_generator;
		// Reset backfill logging state for new crash
		detail::backfill_logged_this_crash = false;
		detail::total_backfill_count = 0;
		std::vector<std::string> results;
		results.resize(a_data.size());
		std::for_each(
			std::execution::par_unseq,
			a_data.begin(),
			a_data.end(),
			[&](auto& a_val) {
				const auto result = detail::analyze_integer(a_val, a_modules);
				const auto pos = std::addressof(a_val) - a_data.data();
				results[pos] = std::visit(
					[](const auto& a_analysis) { return a_analysis.name(); },
					result);
			});
		return results;
	}
}

void Crash::Introspection::backfill_void_pointers(std::vector<std::string>& a_results, std::span<const std::size_t> a_addresses)
{
	assert(a_results.size() == a_addresses.size());

	for (std::size_t i = 0; i < a_results.size(); ++i) {
		auto& result = a_results[i];
		std::size_t addr = a_addresses[i];

		// Only process entries that are still void* pointers (not already replaced)
		if (result.starts_with("(void*")) {
			// Check if this address points to a known object
			auto it = detail::seen_objects.find(reinterpret_cast<const void*>(addr));
			if (it != detail::seen_objects.end()) {
				// Replace with full object information
				result = it->second;
				++detail::total_backfill_count;
			}
		}
	}

	// Log the backfill statistics (only once per crash)
	if (!detail::backfill_logged_this_crash && detail::total_backfill_count > 0) {
		logger::info("Backfilled {} void* pointers with known object information across all analysis", detail::total_backfill_count);
		detail::backfill_logged_this_crash = true;
	}
}
