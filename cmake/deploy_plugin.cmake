# Copies every file listed in FILES_LIST into every directory listed in TARGETS_LIST
# (one path per line).
#
# A file that cannot be copied (typically because the game is running and holds the DLL open)
# only warns, so the remaining deploy targets and the packaging steps still run.
#
#   cmake -DFILES_LIST=<file> -DTARGETS_LIST=<file> -P deploy_plugin.cmake

file(STRINGS "${FILES_LIST}" files ENCODING UTF-8)
file(STRINGS "${TARGETS_LIST}" targets ENCODING UTF-8)

foreach(target IN LISTS targets)
	foreach(file IN LISTS files)
		execute_process(
			COMMAND "${CMAKE_COMMAND}" -E copy "${file}" "${target}"
			RESULT_VARIABLE result
			ERROR_VARIABLE error)

		if(NOT result EQUAL 0)
			message(WARNING "Could not deploy ${file} to ${target}: ${error}")
		endif()
	endforeach()
endforeach()
