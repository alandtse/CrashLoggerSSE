#include "RelevantObjectsSimplifier.h"
#include <algorithm>
#include <utility>
#include <vector>

namespace Crash::Introspection
{
	namespace
	{
		// Helper to extract value for a specific key from filter output
		std::string extract_field(std::string_view analysis, std::string_view key_name)
		{
			std::string search_key = std::string("\n\t\t") + std::string(key_name) + ":";
			auto key_pos = analysis.find(search_key);
			if (key_pos == std::string_view::npos) {
				return "";
			}

			// Move past the key and colon
			auto value_start = key_pos + search_key.length();

			// Skip leading whitespace
			while (value_start < analysis.length() && analysis[value_start] == ' ') {
				value_start++;
			}

			// Find end of line
			auto value_end = analysis.find('\n', value_start);
			if (value_end == std::string_view::npos) {
				value_end = analysis.length();
			}

			std::string value(analysis.substr(value_start, value_end - value_start));

			// Trim quotes
			while (!value.empty() && (value.front() == '\"' || value.front() == ' ')) {
				value = value.substr(1);
			}
			while (!value.empty() && (value.back() == '\"' || value.back() == ' ')) {
				value.pop_back();
			}

			return value;
		}

		// Extract the best line from a stack trace (for CodeTasklet)
		std::string extract_best_stack_line(std::string_view stack_trace)
		{
			std::string best_line;
			size_t line_start = 0;

			while (line_start < stack_trace.length()) {
				auto line_end = stack_trace.find('\n', line_start);
				if (line_end == std::string_view::npos) {
					line_end = stack_trace.length();
				}

				auto line = stack_trace.substr(line_start, line_end - line_start);

				// Trim leading whitespace
				while (!line.empty() && line.front() == ' ') {
					line.remove_prefix(1);
				}
				// Trim trailing whitespace
				while (!line.empty() && line.back() == ' ') {
					line.remove_suffix(1);
				}

				// Look for lines with FormID and .psc file (actual script calls)
				if (!line.empty() &&
					line.find('(') != std::string_view::npos &&
					line.find(").") != std::string_view::npos &&
					line.find(".psc") != std::string_view::npos) {
					return std::string(line);
				}

				// Fallback: any non-native line
				if (best_line.empty() && !line.empty() && line.find("<native>") == std::string_view::npos) {
					best_line = std::string(line);
				}

				line_start = line_end + 1;
			}

			// If only native calls found, use first line
			if (best_line.empty() && !stack_trace.empty()) {
				auto first_line_end = stack_trace.find('\n');
				auto first_line = stack_trace.substr(0, first_line_end != std::string_view::npos ? first_line_end : stack_trace.length());
				while (!first_line.empty() && first_line.front() == ' ') {
					first_line.remove_prefix(1);
				}
				best_line = std::string(first_line);
			}

			return best_line;
		}
	}

	std::string simplify_for_relevant_objects(std::string_view full_analysis)
	{
		// Check if this has filter output (detailed game object)
		auto detail_pos = full_analysis.find("\n\t\t");
		if (detail_pos == std::string_view::npos) {
			// No filter output - return the analysis as-is (e.g., simple polymorphic pointers like "(NiCamera*)")
			// This ensures that introspected objects without detailed properties are still displayed
			return std::string(full_analysis);
		}

		// Extract type name
		std::string type_name(full_analysis.substr(0, detail_pos));

		// Extract key fields (only first-level, tab_depth=0)
		std::string name = extract_field(full_analysis, "Name");
		std::string full_name = extract_field(full_analysis, "GetFullName");
		std::string display_name = extract_field(full_analysis, "Display Name");
		std::string form_id = extract_field(full_analysis, "FormID");
		std::string file = extract_field(full_analysis, "File");
		std::string function = extract_field(full_analysis, "Function");
		std::string object = extract_field(full_analysis, "Object");
		std::string state = extract_field(full_analysis, "State");
		std::string stack_trace = extract_field(full_analysis, "Stack Trace");
		std::string active_quest = extract_field(full_analysis, "Active Quest");
		std::string current_stage = extract_field(full_analysis, "Current Stage");

		// Determine the best name to use
		std::string best_name;
		if (!display_name.empty()) {
			best_name = display_name;
		} else if (!full_name.empty()) {
			best_name = full_name;
		} else if (!name.empty()) {
			best_name = name;
		}

		// Build concise output based on type

		// Special case: BSScript::NF_util::NativeFunctionBase
		if (type_name.find("NativeFunctionBase") != std::string::npos) {
			if (!object.empty() && !function.empty()) {
				if (!state.empty()) {
					return type_name + " " + object + "." + function + "() {State=" + state + "}";
				}
				return type_name + " " + object + "." + function + "()";
			}
			return "";  // Not useful without object.function
		}

		// Special case: BSScript::ObjectTypeInfo
		if (type_name.find("ObjectTypeInfo") != std::string::npos) {
			if (!name.empty()) {
				return type_name + " " + name;
			}
			return "";
		}

		// Special case: CodeTasklet with stack trace
		if (type_name.find("CodeTasklet") != std::string::npos && !stack_trace.empty()) {
			std::string best_line = extract_best_stack_line(stack_trace);
			if (!best_line.empty()) {
				return type_name + " " + best_line;
			}
		}

		// Special case: TESQuest
		if (type_name.find("TESQuest") != std::string::npos) {
			std::string result = type_name;
			if (!best_name.empty()) {
				result += " \"" + best_name + "\"";
			}
			if (!form_id.empty()) {
				// Strip 0x prefix if present
				if (form_id.starts_with("0x") || form_id.starts_with("0X")) {
					form_id = form_id.substr(2);
				}
				result += " [0x" + form_id + "]";
			}
			// Add quest-specific info
			std::vector<std::string> quest_info;
			if (!current_stage.empty()) {
				quest_info.push_back("Stage=" + current_stage);
			}
			if (!active_quest.empty()) {
				quest_info.push_back("Active=" + active_quest);
			}
			if (!quest_info.empty()) {
				result += " {";
				for (size_t i = 0; i < quest_info.size(); ++i) {
					if (i > 0)
						result += ", ";
					result += quest_info[i];
				}
				result += "}";
			}
			return result;
		}

		// Default case: TESForm-like objects and other detailed objects
		// Format: TypeName "Name" [0xFormID] (File.esp)
		// If no standard fields but has filter output, return type name to show it was introspected
		std::string result = type_name;

		if (!best_name.empty()) {
			result += " \"" + best_name + "\"";
		}

		if (!form_id.empty()) {
			// Strip 0x prefix if present
			if (form_id.starts_with("0x") || form_id.starts_with("0X")) {
				form_id = form_id.substr(2);
			}
			result += " [0x" + form_id + "]";
		}

		if (!file.empty()) {
			result += " (" + file + ")";
		}

		return result;
	}
}
