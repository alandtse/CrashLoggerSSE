# Copies the '|' separated FILES into DEST.
#
# A file that cannot be copied (typically because the game is running and holds the DLL open)
# only warns, so the remaining deploy targets and the packaging steps still run.
#
#   cmake -DFILES=a.dll|a.pdb -DDEST=<dir> -P deploy_plugin.cmake

string(REPLACE "|" ";" files "${FILES}")

foreach(file IN LISTS files)
	execute_process(
		COMMAND "${CMAKE_COMMAND}" -E copy "${file}" "${DEST}"
		RESULT_VARIABLE result
		ERROR_VARIABLE error)

	if(NOT result EQUAL 0)
		message(WARNING "Could not deploy ${file} to ${DEST}: ${error}")
	endif()
endforeach()
