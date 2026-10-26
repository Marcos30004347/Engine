# Compile the real story once, then construct two independent runners from it.
execute_process(COMMAND "${PREVIEW}" "${ASSETS}" "${OUTPUT}/city-history-replay.json" 10 7 --verify-replay
  RESULT_VARIABLE result OUTPUT_VARIABLE report ERROR_VARIABLE problem TIMEOUT 600)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "History integration failed: ${result}: ${report} ${problem}")
endif()
message(STATUS "${report} ${problem}")
