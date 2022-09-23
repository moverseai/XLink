message("Server: ${SERVER_EXECUTABLE}, Client: ${CLIENT_EXECUTABLE}, Iterations: ${NUM_ITERATIONS}")

foreach(iteration RANGE ${NUM_ITERATIONS})
    message(STATUS "Iteration ${iteration}...")
    execute_process(
        COMMAND "${SERVER_EXECUTABLE}"
        COMMAND "${SERVER_EXECUTABLE}"
        COMMAND "${SERVER_EXECUTABLE}"
        COMMAND "${SERVER_EXECUTABLE}"
        COMMAND "${SERVER_EXECUTABLE}"
        COMMAND "${SERVER_EXECUTABLE}"
        COMMAND "${SERVER_EXECUTABLE}"
        COMMAND "${SERVER_EXECUTABLE}"
        COMMAND "${CLIENT_EXECUTABLE}"
        # TIMEOUT 10
        RESULT_VARIABLE err
    )
    message(STATUS "Iteration ${iteration} error: ${err}")
    if(NOT (err EQUAL "0"))
        message(FATAL_ERROR "Iteration ${iteration} error: ${err}. Error is not 0")
    endif()
endforeach()